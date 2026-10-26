// JAM storage based on JAMLIB by Bjorn Stenberg and Johan Billing.
// LGPL-2.1-or-later; original notices and license retained in src/jamlib/.
#include "storage.hpp"
#include "storage_mutex.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#include <sys/locking.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace jamnntpd::jam {
namespace {
using FileIdentity = std::pair<std::uint64_t, std::uint64_t>;
constexpr std::array extensions{".jhr", ".jdt", ".jdx", ".jlr"};
auto file_path(const std::filesystem::path &base, std::string_view extension)
    -> std::filesystem::path {
    auto path = base;
    path += extension;
    return path;
}
struct RegistryRecord {
    std::weak_ptr<BaseState> state;
    bool unavailable{};
};
struct Registry {
    // POSIX record locks are process-owned: even a second close can release them.
    // Serialize open/close as well as operations, across every registered handle.
    StorageMutex mutex;
    std::map<FileIdentity, RegistryRecord> states;
};
auto registry() -> Registry & {
    static Registry instance;
    return instance;
}
auto io_error(std::string_view message) -> std::unexpected<Error> {
    const int code = errno;
    return failure(ErrorKind::io, std::string{message}, code);
}
struct CloseFile {
    void operator()(std::FILE *file) const noexcept {
        // unique_ptr owns this borrowed deleter argument; commit checks flush explicitly.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        static_cast<void>(std::fclose(file));
    }
};
using FileHandle = std::unique_ptr<std::FILE, CloseFile>;
constexpr std::size_t kibibyte = 1024;
constexpr std::size_t mebibyte = kibibyte * kibibyte;
constexpr std::size_t maximum_read = 64U * mebibyte;
constexpr auto lock_retry_interval = std::chrono::milliseconds{10};
struct File {
    FileHandle handle;
    StorageOptions *options{};
    [[nodiscard]] auto fails(IoEvent event) const -> bool {
        return options->fail_io && options->fail_io(event);
    }
    [[nodiscard]] auto size() const -> Result<std::uint64_t> {
#ifdef _WIN32
        struct _stat64 status{};
        if (_fstat64(_fileno(handle.get()), &status) != 0) {
#else
        struct stat status{};
        if (fstat(fileno(handle.get()), &status) != 0) {
#endif
            return io_error("Cannot stat JAM file");
        }
        if (status.st_size < 0) {
            return failure(ErrorKind::corrupt, "Negative JAM file size");
        }
        return static_cast<std::uint64_t>(status.st_size);
    }
    [[nodiscard]] auto seek(std::uint64_t offset) const -> Result<void> {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return failure(ErrorKind::corrupt, "JAM file offset outside host range");
        }
#ifdef _WIN32
        if (_fseeki64(handle.get(), static_cast<std::int64_t>(offset), SEEK_SET) != 0) {
#else
        if (fseeko(handle.get(), static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
            return io_error("Cannot seek JAM file");
        }
        return {};
    }
    [[nodiscard]] auto read(std::uint64_t offset, std::size_t count) const -> Result<Bytes> {
        auto length = size();
        if (!length) {
            return std::unexpected(length.error());
        }
        if (offset > *length || count > *length - offset || count > maximum_read) {
            return failure(ErrorKind::corrupt, "Invalid JAM read extent");
        }
        if (fails(IoEvent::read)) {
            return failure(ErrorKind::io, "Injected JAM read failure");
        }
        auto positioned = seek(offset);
        if (!positioned) {
            return std::unexpected(positioned.error());
        }
        Bytes bytes(count);
        if (count != 0 && std::fread(bytes.data(), 1, count, handle.get()) != count) {
            return io_error("Cannot read JAM file");
        }
        return bytes;
    }
    [[nodiscard]] auto write(std::uint64_t offset, ByteView bytes) const -> Result<void> {
        auto positioned = seek(offset);
        if (!positioned) {
            return positioned;
        }
        if (fails(IoEvent::write)) {
            // Model a short write, not just an error before any mutation.
            if (!bytes.empty()) {
                static_cast<void>(std::fwrite(bytes.data(), 1, bytes.size() / 2, handle.get()));
            }
            return failure(ErrorKind::io, "Injected JAM partial write failure");
        }
        if (!bytes.empty() &&
            std::fwrite(bytes.data(), 1, bytes.size(), handle.get()) != bytes.size()) {
            return io_error("Cannot write JAM file");
        }
        return {};
    }
    [[nodiscard]] auto flush() const -> Result<void> {
        if (fails(IoEvent::flush)) {
            return failure(ErrorKind::io, "Injected JAM flush failure");
        }
        if (std::fflush(handle.get()) != 0) {
            return io_error("Cannot flush JAM file");
        }
        return {};
    }
    [[nodiscard]] auto resize(std::uint64_t length) const -> Result<void> {
        if (fails(IoEvent::resize)) {
            return failure(ErrorKind::io, "Injected JAM resize failure");
        }
        std::clearerr(handle.get());
#ifdef _WIN32
        if (_chsize_s(_fileno(handle.get()), length) != 0) {
#else
        if (length > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
            ftruncate(fileno(handle.get()), static_cast<off_t>(length)) != 0) {
#endif
            return io_error("Cannot restore JAM file length");
        }
        return {};
    }
};
auto identify(const std::filesystem::path &path) -> Result<FileIdentity> {
#ifdef _WIN32
    struct NativeCloser {
        void operator()(void *handle) const noexcept { CloseHandle(handle); }
    };
    std::unique_ptr<void, NativeCloser> handle{CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr)};
    if (handle.get() == INVALID_HANDLE_VALUE) {
        static_cast<void>(handle.release());
        return failure(ErrorKind::io, "Cannot identify JAM file", static_cast<int>(GetLastError()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle.get(), &information) == 0) {
        return failure(ErrorKind::io, "Cannot identify JAM file", static_cast<int>(GetLastError()));
    }
    return FileIdentity{information.dwVolumeSerialNumber,
                        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
                            information.nFileIndexLow};
#else
    struct stat status{};
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return io_error("Cannot identify JAM file");
    }
    return FileIdentity{static_cast<std::uint64_t>(status.st_dev),
                        static_cast<std::uint64_t>(status.st_ino)};
#endif
}
auto identify_open_file(const File &file) -> Result<FileIdentity> {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION information{};
    // Borrowed CRT handle; ownership stays with the FILE owner.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file.handle.get())));
    if (GetFileInformationByHandle(handle, &information) == 0) {
        return failure(ErrorKind::io, "Cannot identify open JAM file",
                       static_cast<int>(GetLastError()));
    }
    constexpr unsigned int high_shift = 32;
    return FileIdentity{information.dwVolumeSerialNumber,
                        (static_cast<std::uint64_t>(information.nFileIndexHigh) << high_shift) |
                            information.nFileIndexLow};
#else
    struct stat status{};
    if (fstat(fileno(file.handle.get()), &status) != 0) {
        return io_error("Cannot identify open JAM file");
    }
    return FileIdentity{static_cast<std::uint64_t>(status.st_dev),
                        static_cast<std::uint64_t>(status.st_ino)};
#endif
}
auto native_lock_code(const File &file, bool acquire) noexcept -> int {
#ifdef _WIN32
    if (_fseeki64(file.handle.get(), 0, SEEK_SET) != 0) {
        return errno;
    }
    if (_locking(_fileno(file.handle.get()), acquire ? _LK_NBLCK : _LK_UNLCK, 1) != 0) {
#else
    struct flock lock{};
    lock.l_type = acquire ? F_WRLCK : F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;
    // POSIX specifies a typed flock pointer for this command, not a format string.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (fcntl(fileno(file.handle.get()), F_SETLK, &lock) == -1) {
#endif
        return errno;
    }
    return 0;
}

} // namespace

struct BaseState {
    StorageOptions options;
    FileIdentity identity;
    RegistryRecord *registry_record{};
    std::array<FileIdentity, extensions.size()> file_identities{};
    File header;
    File text;
    File index;
    File lastread;
    ~BaseState() {
        const std::scoped_lock lock{registry().mutex};
        header.handle.reset();
        text.handle.reset();
        index.handle.reset();
        lastread.handle.reset();
    }
    BaseState() = default;
    BaseState(const BaseState &) = delete;
    auto operator=(const BaseState &) -> BaseState & = delete;
    BaseState(BaseState &&) = delete;
    auto operator=(BaseState &&) -> BaseState & = delete;
};
namespace {
auto acquire_registry(std::chrono::steady_clock::time_point deadline, const std::stop_token &stop)
    -> Result<std::unique_lock<StorageMutex>> {
    if (registry().mutex.acquire(deadline, stop)) {
        return std::unique_lock{registry().mutex, std::adopt_lock};
    }
    return stop.stop_requested() ? failure(ErrorKind::stopped, "JAM operation stopped")
                                 : failure(ErrorKind::timeout, "JAM lock timeout");
}
class Guard {
  public:
    static auto acquire(BaseState &state, const std::stop_token &stop) -> Result<Guard> {
        const auto deadline = std::chrono::steady_clock::now() + state.options.lock_timeout;
        auto acquired = acquire_registry(deadline, stop);
        if (!acquired) {
            return std::unexpected{acquired.error()};
        }
        auto lock = std::move(*acquired);
        if (state.registry_record->unavailable) {
            return failure(ErrorKind::corrupt, "JAM base requires restoration from backup");
        }
        if (state.header.fails(IoEvent::lock)) {
            return failure(ErrorKind::io, "Injected JAM lock failure");
        }
        while (!stop.stop_requested()) {
            const auto lock_code = native_lock_code(state.header, true);
            if (lock_code == 0) {
                return Guard{std::move(lock), state};
            }
            if (lock_code != EACCES && lock_code != EAGAIN && lock_code != EINTR) {
                return failure(ErrorKind::io, "Cannot acquire JAM record lock", lock_code);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return failure(ErrorKind::timeout, "JAM lock timeout");
            }
            std::this_thread::sleep_for(lock_retry_interval);
        }
        return failure(ErrorKind::stopped, "JAM operation stopped");
    }
    ~Guard() {
        if (state_ != nullptr) {
            static_cast<void>(native_lock_code(state_->header, false));
        }
    }
    Guard(Guard &&other) noexcept
        : lock_(std::move(other.lock_)), state_(std::exchange(other.state_, nullptr)) {}
    Guard(const Guard &) = delete;
    auto operator=(const Guard &) -> Guard & = delete;
    auto operator=(Guard &&) -> Guard & = delete;

  private:
    Guard(std::unique_lock<StorageMutex> lock, BaseState &state)
        : lock_(std::move(lock)), state_(&state) {}
    std::unique_lock<StorageMutex> lock_;
    BaseState *state_;
};
auto read_info(const BaseState &state) -> Result<BaseInfo> {
    auto bytes = state.header.read(0, base_size);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto header = decode_base(*bytes);
    if (!header) {
        return std::unexpected(header.error());
    }
    auto size = state.index.size();
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size % index_size != 0 || *size / index_size > no_crc ||
        (*size != 0 && (*size / index_size) - 1 > no_crc - header->first_number)) {
        return failure(ErrorKind::corrupt, "Invalid JAM index size or number range");
    }
    return BaseInfo{.header = *header, .slots = static_cast<std::uint32_t>(*size / index_size)};
}
// Validate before extending either file: new bytes must not revive a dangling old record.
auto validate_published_extents(const BaseState &state, const BaseInfo &info,
                                std::uint64_t header_size, std::uint64_t text_size,
                                const std::stop_token &stop) -> Result<void> {
    for (std::uint32_t slot = 0; slot < info.slots; ++slot) {
        if (stop.stop_requested()) {
            return failure(ErrorKind::stopped, "JAM validation stopped");
        }
        auto bytes = state.index.read(static_cast<std::uint64_t>(slot) * index_size, index_size);
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        const auto index = decode_index(*bytes);
        if (!index) {
            return std::unexpected{index.error()};
        }
        if (index->header_offset == no_crc && index->user_crc == no_crc) {
            continue;
        }
        const std::uint64_t offset = index->header_offset;
        if (offset < base_size || offset > header_size || message_size > header_size - offset) {
            return failure(ErrorKind::corrupt, "Published JAM header exceeds file extent");
        }
        auto fixed = state.header.read(offset, message_size);
        if (!fixed) {
            return std::unexpected{fixed.error()};
        }
        const auto header = decode_message(*fixed);
        if (!header) {
            return std::unexpected{header.error()};
        }
        if (header->number != info.header.first_number + slot) {
            return failure(ErrorKind::corrupt, "Published JAM article number mismatch");
        }
        if ((header->attributes & deleted) != 0) {
            continue;
        }
        if (header->subfield_length > header_size - offset - message_size ||
            header->text_offset > text_size ||
            header->text_length > text_size - header->text_offset) {
            return failure(ErrorKind::corrupt, "Published JAM content exceeds file extent");
        }
    }
    return {};
}
struct LocatedMessage {
    Message message;
    std::uint64_t index_offset{};
    std::uint32_t header_offset{};
};
struct HeaderLocation {
    MessageHeader header;
    std::uint64_t index_offset{};
    std::uint32_t header_offset{};
};
auto locate_header(const BaseState &state, std::uint32_t number) -> Result<HeaderLocation> {
    auto info = read_info(state);
    if (!info) {
        return std::unexpected(info.error());
    }
    if (number < info->header.first_number || number - info->header.first_number >= info->slots) {
        return failure(ErrorKind::input, "JAM article not found");
    }
    const auto index_offset =
        static_cast<std::uint64_t>(number - info->header.first_number) * index_size;
    auto index_bytes = state.index.read(index_offset, index_size);
    if (!index_bytes) {
        return std::unexpected(index_bytes.error());
    }
    auto index = decode_index(*index_bytes);
    if (!index) {
        return std::unexpected(index.error());
    }
    if (index->header_offset == no_crc && index->user_crc == no_crc) {
        return failure(ErrorKind::input, "JAM article deleted");
    }
    if (index->header_offset < base_size) {
        return failure(ErrorKind::corrupt, "Invalid JAM header offset");
    }
    auto header_bytes = state.header.read(index->header_offset, message_size);
    if (!header_bytes) {
        return std::unexpected(header_bytes.error());
    }
    auto header = decode_message(*header_bytes);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (header->number != number) {
        return failure(ErrorKind::corrupt, "JAM article number mismatch");
    }
    if ((header->attributes & deleted) != 0) {
        return failure(ErrorKind::input, "JAM article deleted");
    }
    return HeaderLocation{
        .header = *header,
        .index_offset = index_offset,
        .header_offset = index->header_offset,
    };
}
auto locate(const BaseState &state, std::uint32_t number) -> Result<LocatedMessage> {
    auto location = locate_header(state, number);
    if (!location) {
        return std::unexpected(location.error());
    }
    auto sub_bytes =
        state.header.read(static_cast<std::uint64_t>(location->header_offset) + message_size,
                          location->header.subfield_length);
    if (!sub_bytes) {
        return std::unexpected(sub_bytes.error());
    }
    auto fields = decode_subfields(*sub_bytes);
    if (!fields) {
        return std::unexpected(fields.error());
    }
    auto text_bytes = state.text.read(location->header.text_offset, location->header.text_length);
    if (!text_bytes) {
        return std::unexpected(text_bytes.error());
    }
    return LocatedMessage{
        .message =
            {
                .header = location->header,
                .fields = std::move(*fields),
                .text = text_from(*text_bytes),
            },
        .index_offset = location->index_offset,
        .header_offset = location->header_offset,
    };
}

struct Change {
    File *file;
    std::uint64_t offset;
    Bytes replacement;
    Bytes original;
};
auto reply_tail(const BaseState &state, std::uint32_t parent_number, const std::stop_token &stop)
    -> Result<HeaderLocation> {
    auto parent = locate_header(state, parent_number);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    std::set<std::uint32_t> visited{parent_number};
    auto location = *parent;
    auto next = location.header.first_reply;
    while (next != 0) {
        if (stop.stop_requested()) {
            return failure(ErrorKind::stopped, "Reply traversal stopped");
        }
        if (!visited.insert(next).second) {
            return failure(ErrorKind::corrupt, "Cycle in JAM reply chain");
        }
        auto reply = locate_header(state, next);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        if (reply->header.reply_to != parent_number) {
            return failure(ErrorKind::corrupt, "Inconsistent JAM reply parent");
        }
        location = *reply;
        next = location.header.next_reply;
    }
    return location;
}
auto reply_predecessor(const BaseState &state, const HeaderLocation &article,
                       const std::stop_token &stop) -> Result<std::optional<HeaderLocation>> {
    const auto parent_number = article.header.reply_to;
    auto parent = locate_header(state, parent_number);
    if (!parent) {
        if (parent.error().kind == ErrorKind::input) {
            return std::optional<HeaderLocation>{};
        }
        return std::unexpected(parent.error());
    }
    std::set<std::uint32_t> visited{parent_number};
    auto previous = *parent;
    auto next = previous.header.first_reply;
    while (next != 0) {
        if (stop.stop_requested()) {
            return failure(ErrorKind::stopped, "Reply traversal stopped");
        }
        if (next == article.header.number) {
            return std::optional<HeaderLocation>{previous};
        }
        if (!visited.insert(next).second) {
            return failure(ErrorKind::corrupt, "Cycle in JAM reply chain");
        }
        auto reply = locate_header(state, next);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        if (reply->header.reply_to != parent_number) {
            return failure(ErrorKind::corrupt, "Inconsistent JAM reply parent");
        }
        previous = *reply;
        next = previous.header.next_reply;
    }
    return std::optional<HeaderLocation>{};
}
auto unlink_reply(BaseState &state, const HeaderLocation &article, const std::stop_token &stop)
    -> Result<std::optional<Change>> {
    if (article.header.reply_to == 0) {
        return std::optional<Change>{};
    }
    if (article.header.reply_to == article.header.number ||
        article.header.next_reply == article.header.number ||
        article.header.next_reply == article.header.reply_to) {
        return failure(ErrorKind::corrupt, "Invalid JAM reply links");
    }
    auto predecessor = reply_predecessor(state, article, stop);
    if (!predecessor) {
        return std::unexpected(predecessor.error());
    }
    if (!*predecessor) {
        return std::optional<Change>{};
    }
    auto &location = **predecessor;
    if (article.header.next_reply != 0) {
        auto next = locate_header(state, article.header.next_reply);
        if (!next) {
            return std::unexpected(next.error());
        }
        if (next->header.reply_to != article.header.reply_to) {
            return failure(ErrorKind::corrupt, "Inconsistent JAM reply parent");
        }
    }
    if (location.header.number == article.header.reply_to) {
        location.header.first_reply = article.header.next_reply;
    } else {
        location.header.next_reply = article.header.next_reply;
    }
    return std::optional<Change>{Change{
        .file = &state.header,
        .offset = location.header_offset,
        .replacement = encode_message(location.header),
        .original = {},
    }};
}
void quarantine(BaseState &state) noexcept { state.registry_record->unavailable = true; }
auto restore(BaseState &state, std::vector<Change> &changes,
             const std::map<File *, std::uint64_t> &original_sizes) -> bool {
    bool restored = true;
    for (auto &change : changes | std::views::reverse) {
        if (!change.original.empty() && !change.file->write(change.offset, change.original)) {
            restored = false;
        }
    }
    for (const auto &[file, size] : original_sizes) {
        if (!file->resize(size)) {
            restored = false;
        }
        if (!file->flush()) {
            restored = false;
        }
    }
    if (!restored) {
        quarantine(state);
    }
    return restored;
}
class RollbackOnException {
  public:
    RollbackOnException(BaseState &state, std::vector<Change> &changes,
                        const std::map<File *, std::uint64_t> &sizes)
        : state_(state), changes_(changes), sizes_(sizes) {}
    ~RollbackOnException() noexcept {
        if (active_) {
            try {
                if (!restore(state_, changes_, sizes_)) {
                    quarantine(state_);
                }
            } catch (...) {
                quarantine(state_); // No allocation is required, including on bad_alloc.
            }
        }
    }
    RollbackOnException(const RollbackOnException &) = delete;
    auto operator=(const RollbackOnException &) -> RollbackOnException & = delete;
    RollbackOnException(RollbackOnException &&) = delete;
    auto operator=(RollbackOnException &&) -> RollbackOnException & = delete;
    void dismiss() noexcept { active_ = false; }

  private:
    BaseState &state_;
    std::vector<Change> &changes_;
    const std::map<File *, std::uint64_t> &sizes_;
    bool active_{true};
};
auto mutate(BaseState &state, std::vector<Change> changes) -> Result<void> {
    std::map<File *, std::uint64_t> original_sizes;
    for (auto &change : changes) {
        auto size = change.file->size();
        if (!size) {
            return std::unexpected(size.error());
        }
        original_sizes.emplace(change.file, *size);
        if (change.offset > *size) {
            return failure(ErrorKind::corrupt, "Noncontiguous JAM write");
        }
        const auto overlap =
            std::min<std::uint64_t>(change.replacement.size(), *size - change.offset);
        auto original = change.file->read(change.offset, static_cast<std::size_t>(overlap));
        if (!original) {
            return std::unexpected(original.error());
        }
        change.original = std::move(*original);
    }
    RollbackOnException rollback{state, changes, original_sizes};
    std::optional<Error> error;
    for (auto &change : changes) {
        auto result = change.file->write(change.offset, change.replacement);
        if (!result) {
            error = result.error();
            break;
        }
    }
    if (!error) {
        for (const auto &[file, size] : original_sizes) {
            static_cast<void>(size);
            auto flushed = file->flush();
            if (!flushed) {
                error = flushed.error();
                break;
            }
        }
    }
    if (!error) {
        rollback.dismiss();
        return {};
    }
    const bool restored = restore(state, changes, original_sizes);
    rollback.dismiss();
    if (!restored) {
        return failure(ErrorKind::corrupt,
                       "JAM rollback failed; base disabled, restore a consistent backup");
    }
    return std::unexpected(*error);
}
} // namespace

auto Storage::open(const std::filesystem::path &base, StorageOptions options,
                   const std::stop_token &stop) -> Result<Storage> {
    auto acquired = acquire_registry(std::chrono::steady_clock::now() + options.lock_timeout, stop);
    if (!acquired) {
        return std::unexpected{acquired.error()};
    }
    const auto header_path = file_path(base, ".jhr");
    auto identity = identify(header_path);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    if (registry().states[*identity].unavailable) {
        return failure(ErrorKind::corrupt, "JAM base requires restoration from backup");
    }
    if (auto existing = registry().states[*identity].state.lock()) {
        for (std::size_t index = 0; index < extensions.size(); ++index) {
            const auto companion = identify(file_path(base, extensions.at(index)));
            if (!companion || *companion != existing->file_identities.at(index)) {
                return failure(ErrorKind::corrupt, "JAM alias has inconsistent companion files");
            }
        }
        return Storage{std::move(existing)};
    }
    auto state = std::make_shared<BaseState>();
    state->options = std::move(options);
    state->identity = *identity;
    const std::array files{&state->header, &state->text, &state->index, &state->lastread};
    for (std::size_t index = 0; index < files.size(); ++index) {
        auto *file = files.at(index);
        const auto path = file_path(base, extensions.at(index));
        auto path_identity = identify(path);
        if (!path_identity) {
            return std::unexpected(path_identity.error());
        }
#ifdef _WIN32
        // Keep first-byte locking interoperable with independent readers/tossers.
        file->handle.reset(_wfsopen(path.c_str(), L"r+b", _SH_DENYNO));
#else
        // unique_ptr adopts the FILE immediately; no raw owner escapes this boundary.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        file->handle.reset(std::fopen(path.c_str(), "r+b"));
#endif
        file->options = &state->options;
        if (!file->handle) {
            const auto code = errno;
            return failure(ErrorKind::io, "Cannot open JAM base: " + path.string(), code);
        }
        if (std::setvbuf(file->handle.get(), nullptr, _IONBF, 0) != 0) {
            return io_error("Cannot configure JAM file buffering");
        }
        auto open_identity = identify_open_file(*file);
        if (!open_identity || *open_identity != *path_identity) {
            return failure(ErrorKind::io, "JAM file changed during open");
        }
        state->file_identities.at(index) = *open_identity;
    }
    auto confirmed = identify(header_path);
    if (!confirmed || *confirmed != *identity) {
        return failure(ErrorKind::io, "JAM file changed during open");
    }
    registry().states[*identity].state = state;
    state->registry_record = &registry().states[*identity];
    Storage storage{std::move(state)};
    auto info = storage.info(stop);
    if (!info) {
        return std::unexpected(info.error());
    }
    return storage;
}
auto Storage::info(const std::stop_token &stop) const -> Result<BaseInfo> {
    auto guard = Guard::acquire(*state_, stop);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    return read_info(*state_);
}
auto Storage::read(std::uint32_t number, const std::stop_token &stop) const -> Result<Message> {
    auto guard = Guard::acquire(*state_, stop);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    auto located = locate(*state_, number);
    if (!located) {
        return std::unexpected(located.error());
    }
    return std::move(located->message);
}
auto Storage::append(Message message, const std::stop_token &stop) const -> Result<std::uint32_t> {
    if (message.header.revision != 1 || message.header.first_reply != 0 ||
        message.header.next_reply != 0) {
        return failure(ErrorKind::input,
                       "New JAM article has invalid revision or existing child links");
    }
    auto fields = encode_subfields(message.fields);
    if (!fields) {
        return std::unexpected(fields.error());
    }
    auto guard = Guard::acquire(*state_, stop);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    auto info = read_info(*state_);
    if (!info) {
        return std::unexpected(info.error());
    }
    auto header_size = state_->header.size();
    auto text_size = state_->text.size();
    if (!header_size) {
        return std::unexpected(header_size.error());
    }
    if (!text_size) {
        return std::unexpected(text_size.error());
    }
    if (*header_size > no_crc || *text_size > no_crc || fields->size() > no_crc - message_size ||
        message_size + fields->size() > no_crc - *header_size ||
        message.text.size() > no_crc - *text_size ||
        info->slots > no_crc - info->header.first_number || info->header.active == no_crc) {
        return failure(ErrorKind::input, "JAM format capacity exceeded");
    }
    if (auto validated = validate_published_extents(*state_, *info, *header_size, *text_size, stop);
        !validated) {
        return std::unexpected{validated.error()};
    }
    message.header.number = info->header.first_number + info->slots;
    message.header.subfield_length = static_cast<std::uint32_t>(fields->size());
    message.header.text_offset = static_cast<std::uint32_t>(*text_size);
    message.header.text_length = static_cast<std::uint32_t>(message.text.size());
    auto header_bytes = encode_message(message.header);
    header_bytes.insert(header_bytes.end(), fields->begin(), fields->end());
    auto user_crc = no_crc;
    for (const auto &field : message.fields) {
        if (field.low == 3) {
            user_crc = crc32(text_from(field.data));
        }
    }
    std::vector<Change> changes;
    changes.push_back({
        .file = &state_->text,
        .offset = *text_size,
        .replacement = bytes_from(message.text),
        .original = {},
    });
    changes.push_back({
        .file = &state_->header,
        .offset = *header_size,
        .replacement = std::move(header_bytes),
        .original = {},
    });
    if (message.header.reply_to != 0) {
        auto tail = reply_tail(*state_, message.header.reply_to, stop);
        if (!tail) {
            return std::unexpected(tail.error());
        }
        if (tail->header.number == message.header.reply_to) {
            tail->header.first_reply = message.header.number;
        } else {
            tail->header.next_reply = message.header.number;
        }
        changes.push_back({
            .file = &state_->header,
            .offset = tail->header_offset,
            .replacement = encode_message(tail->header),
            .original = {},
        });
    }
    changes.push_back({
        .file = &state_->index,
        .offset = static_cast<std::uint64_t>(info->slots) * index_size,
        .replacement = encode_index(
            {.user_crc = user_crc, .header_offset = static_cast<std::uint32_t>(*header_size)}),
        .original = {},
    });
    if ((message.header.attributes & deleted) == 0) {
        ++info->header.active;
    }
    ++info->header.modifications;
    changes.push_back({
        .file = &state_->header,
        .offset = 0,
        .replacement = encode_base(info->header),
        .original = {},
    });
    auto committed = mutate(*state_, std::move(changes));
    if (!committed) {
        return std::unexpected(committed.error());
    }
    return message.header.number;
}
auto Storage::cancel(std::uint32_t number, const std::stop_token &stop) const -> Result<void> {
    return cancel_impl(number, std::nullopt, stop);
}
auto Storage::cancel_checked(std::uint32_t number, const Message &authorized_snapshot,
                             const std::stop_token &stop) const -> Result<void> {
    return cancel_impl(number, std::cref(authorized_snapshot), stop);
}
auto Storage::cancel_impl(std::uint32_t number,
                          std::optional<std::reference_wrapper<const Message>> snapshot,
                          const std::stop_token &stop) const -> Result<void> {
    auto guard = Guard::acquire(*state_, stop);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    auto article = locate_header(*state_, number);
    if (!article) {
        return std::unexpected(article.error());
    }
    if (snapshot) {
        auto current = locate(*state_, number);
        if (!current) {
            return std::unexpected{current.error()};
        }
        const auto &expected = snapshot->get();
        const bool fields_match = std::ranges::equal(
            current->message.fields, expected.fields,
            [](const Subfield &left, const Subfield &right) -> bool {
                return left.low == right.low && left.high == right.high && left.data == right.data;
            });
        if (encode_message(current->message.header) != encode_message(expected.header) ||
            current->message.text != expected.text || !fields_match) {
            return failure(ErrorKind::permission,
                           "JAM article changed after cancellation authorization");
        }
    }
    auto info = read_info(*state_);
    if (!info) {
        return std::unexpected(info.error());
    }
    article->header.attributes |= deleted;
    if (info->header.active == 0) {
        return failure(ErrorKind::corrupt, "Invalid JAM active count");
    }
    --info->header.active;
    ++info->header.modifications;
    auto unlink = unlink_reply(*state_, *article, stop);
    if (!unlink) {
        return std::unexpected(unlink.error());
    }
    std::vector<Change> changes{
        {
            .file = &state_->header,
            .offset = article->header_offset,
            .replacement = encode_message(article->header),
            .original = {},
        },
        {
            .file = &state_->index,
            .offset = article->index_offset,
            .replacement = encode_index({.user_crc = no_crc, .header_offset = no_crc}),
            .original = {},
        },
        {
            .file = &state_->header,
            .offset = 0,
            .replacement = encode_base(info->header),
            .original = {},
        },
    };
    if (*unlink) {
        changes.push_back(std::move(**unlink));
    }
    return mutate(*state_, std::move(changes));
}
auto Storage::lastread(std::uint32_t user, const std::stop_token &stop) const -> Result<LastRead> {
    auto guard = Guard::acquire(*state_, stop);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    auto size = state_->lastread.size();
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size % lastread_size != 0) {
        return failure(ErrorKind::corrupt, "Invalid JAM lastread length");
    }
    for (std::uint64_t offset = 0; offset < *size; offset += lastread_size) {
        auto bytes = state_->lastread.read(offset, lastread_size);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto record = decode_lastread(*bytes);
        if (!record) {
            return std::unexpected(record.error());
        }
        if (record->user_id == user) {
            return record;
        }
    }
    return failure(ErrorKind::input, "JAM lastread user not found");
}
auto Storage::set_lastread(LastRead record, const std::stop_token &stop) const -> Result<void> {
    auto guard = Guard::acquire(*state_, stop);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    auto size = state_->lastread.size();
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size % lastread_size != 0) {
        return failure(ErrorKind::corrupt, "Invalid JAM lastread length");
    }
    std::uint64_t offset{};
    for (; offset < *size; offset += lastread_size) {
        auto bytes = state_->lastread.read(offset, lastread_size);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto existing = decode_lastread(*bytes);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (existing->user_id == record.user_id) {
            break;
        }
    }
    return mutate(*state_, {
                               {
                                   .file = &state_->lastread,
                                   .offset = offset,
                                   .replacement = encode_lastread(record),
                                   .original = {},
                               },
                           });
}
} // namespace jamnntpd::jam
