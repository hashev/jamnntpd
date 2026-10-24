#include "workers.hpp"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <system_error>
#include <utility>

namespace jamnntpd {
struct AdmissionState {
    std::mutex mutex;
    std::uint32_t active{};
    std::uint32_t maximum{};
};
SessionLease::~SessionLease() { release(); }
SessionLease::SessionLease(SessionLease &&other) noexcept : state_(std::move(other.state_)) {}
auto SessionLease::operator=(SessionLease &&other) noexcept -> SessionLease & {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
    }
    return *this;
}
void SessionLease::release() noexcept {
    if (state_) {
        {
            std::scoped_lock lock{state_->mutex};
            --state_->active;
        }
        state_.reset();
    }
}
Admission::Admission(std::uint32_t maximum) : state_(std::make_shared<AdmissionState>()) {
    state_->maximum = maximum;
}
auto Admission::reserve() const -> Result<SessionLease> {
    std::scoped_lock lock{state_->mutex};
    if (state_->active == state_->maximum) {
        return failure(ErrorKind::permission, "Maximum number of connections reached");
    }
    ++state_->active;
    return SessionLease{state_};
}
auto Admission::active() const -> std::uint32_t {
    std::scoped_lock lock{state_->mutex};
    return state_->active;
}
namespace {
void safe_report(const WorkerErrorHandler &report) noexcept {
    try {
        if (report) {
            // Exception messages can contain private protocol data; report a fixed diagnostic.
            report("Session worker failed");
        }
    } catch (...) {
        static_cast<void>(std::fputs("JamNNTPd: session diagnostic failed\n", stderr));
    }
}
struct WorkerContext {
    SessionLease lease;
    std::shared_ptr<std::atomic<bool>> done;
    Worker task;
    WorkerErrorHandler report;
    void run(std::stop_token stop) const noexcept {
        try {
            task(std::move(stop));
        } catch (...) {
            safe_report(report);
        }
        done->store(true);
    }
};
} // namespace
WorkerPool::WorkerPool(std::uint32_t maximum, WorkerErrorHandler report, ThreadLauncher launch)
    : admission_(maximum), report_(std::move(report)), launch_(std::move(launch)) {
    if (!launch_) {
        launch_ = [](Worker worker) -> std::jthread { return std::jthread{std::move(worker)}; };
    }
}
WorkerPool::~WorkerPool() { stop_join(); }
auto WorkerPool::start_task(Worker worker) -> Result<void> {
    if (stopping_) {
        return failure(ErrorKind::stopped, "Server is stopping");
    }
    reap();
    auto lease = admission_.reserve();
    if (!lease) {
        return std::unexpected{lease.error()};
    }
    try {
        // Reserve vector storage before thread creation, so publication cannot allocate.
        entries_.reserve(entries_.size() + 1);
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto context = std::make_shared<WorkerContext>(WorkerContext{
            .lease = std::move(*lease),
            .done = done,
            .task = std::move(worker),
            .report = report_,
        });
        auto thread =
            launch_([context](std::stop_token stop) -> void { context->run(std::move(stop)); });
        if (!thread.joinable()) {
            return failure(ErrorKind::io, "Worker launcher did not start a thread");
        }
        entries_.push_back({.done = std::move(done), .thread = std::move(thread)});
    } catch (const std::system_error &error) {
        return failure(ErrorKind::io, "Cannot start session worker", error.code().value());
    } catch (const std::bad_alloc &) {
        return failure(ErrorKind::io, "Cannot allocate session worker");
    }
    return {};
}
void WorkerPool::reap() {
    std::erase_if(entries_, [](const Entry &entry) -> bool { return entry.done->load(); });
}
void WorkerPool::stop_join() noexcept {
    stopping_ = true;
    for (auto &entry : entries_) {
        entry.thread.request_stop();
    }
    entries_.clear();
}
} // namespace jamnntpd
