#ifndef JAMNNTPD_CORE_STORAGE_MUTEX_HPP
#define JAMNNTPD_CORE_STORAGE_MUTEX_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stop_token>
#include <thread>

namespace jamnntpd::jam {
// Open, close and POSIX process-owned locks share this queue. In particular, a
// last-owner close must not starve behind a stream of new timed acquisitions.
class StorageMutex {
  public:
    void lock() { static_cast<void>(acquire(std::chrono::steady_clock::time_point::max(), {})); }
    [[nodiscard]] auto acquire(std::chrono::steady_clock::time_point deadline,
                               const std::stop_token &stop) -> bool {
        const std::stop_callback wake{stop, [this] -> void {
                                          const std::scoped_lock lock{mutex_};
                                          changed_.notify_all();
                                      }};
        std::unique_lock lock{mutex_};
        if (stop.stop_requested()) {
            return false;
        }
        const auto caller = std::this_thread::get_id();
        if (owner_ == caller) {
            ++depth_;
            return true;
        }
        Waiter ticket{.previous = tail_, .next = nullptr};
        if (tail_ != nullptr) {
            tail_->next = &ticket;
        } else {
            head_ = &ticket;
        }
        tail_ = &ticket;
        const auto available = [&] -> bool {
            return stop.stop_requested() || (owner_ == std::thread::id{} && head_ == &ticket);
        };
        bool ready{};
        try {
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                changed_.wait(lock, available);
                ready = !stop.stop_requested();
            } else {
                ready = changed_.wait_until(lock, deadline, available) && !stop.stop_requested();
            }
        } catch (...) {
            remove(ticket);
            throw;
        }
        remove(ticket);
        if (ready) {
            owner_ = caller;
            depth_ = 1;
        }
        return ready;
    }
    void unlock() {
        std::scoped_lock lock{mutex_};
        --depth_;
        if (depth_ == 0) {
            owner_ = {};
            changed_.notify_all();
        }
    }

  private:
    struct Waiter {
        Waiter *previous;
        Waiter *next;
    };
    void remove(const Waiter &ticket) {
        if (ticket.previous != nullptr) {
            ticket.previous->next = ticket.next;
        } else {
            head_ = ticket.next;
        }
        if (ticket.next != nullptr) {
            ticket.next->previous = ticket.previous;
        } else {
            tail_ = ticket.previous;
        }
        changed_.notify_all();
    }
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread::id owner_;
    std::size_t depth_{};
    Waiter *head_{};
    Waiter *tail_{};
};
} // namespace jamnntpd::jam
#endif
