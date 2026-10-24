#ifndef JAMNNTPD_CORE_WORKERS_HPP
#define JAMNNTPD_CORE_WORKERS_HPP
#include "error.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace jamnntpd {
struct AdmissionState;
class SessionLease {
  public:
    ~SessionLease();
    SessionLease(const SessionLease &) = delete;
    auto operator=(const SessionLease &) -> SessionLease & = delete;
    SessionLease(SessionLease &&other) noexcept;
    auto operator=(SessionLease &&other) noexcept -> SessionLease &;

  private:
    friend class Admission;
    explicit SessionLease(std::shared_ptr<AdmissionState> state) : state_(std::move(state)) {}
    void release() noexcept;
    std::shared_ptr<AdmissionState> state_;
};
class Admission {
  public:
    explicit Admission(std::uint32_t maximum);
    [[nodiscard]] auto reserve() const -> Result<SessionLease>;
    [[nodiscard]] auto active() const -> std::uint32_t;

  private:
    std::shared_ptr<AdmissionState> state_;
};
using Worker = std::function<void(std::stop_token)>;
using ThreadLauncher = std::function<std::jthread(Worker)>;
using WorkerErrorHandler = std::function<void(std::string_view)>;
// Single owner starts/reaps/joins workers. Admission itself is safe for concurrent use.
class WorkerPool {
  public:
    explicit WorkerPool(std::uint32_t maximum, WorkerErrorHandler report,
                        ThreadLauncher launch = {});
    ~WorkerPool();
    WorkerPool(const WorkerPool &) = delete;
    auto operator=(const WorkerPool &) -> WorkerPool & = delete;
    WorkerPool(WorkerPool &&) = delete;
    auto operator=(WorkerPool &&) -> WorkerPool & = delete;
    template <class Function> [[nodiscard]] auto start(Function &&worker) -> Result<void> {
        try {
            auto owner =
                std::make_shared<std::remove_cvref_t<Function>>(std::forward<Function>(worker));
            return start_task(
                [owner](const std::stop_token &stop) -> void { std::invoke(*owner, stop); });
        } catch (const std::bad_alloc &) {
            return failure(ErrorKind::io, "Cannot allocate session task");
        }
    }
    [[nodiscard]] auto active() const -> std::uint32_t { return admission_.active(); }
    void reap();
    void stop_join() noexcept;

  private:
    [[nodiscard]] auto start_task(Worker worker) -> Result<void>;
    struct Entry {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };
    Admission admission_;
    WorkerErrorHandler report_;
    ThreadLauncher launch_;
    std::vector<Entry> entries_;
    bool stopping_{};
};
} // namespace jamnntpd
#endif
