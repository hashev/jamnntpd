#include "core/signals.hpp"
#include "core/workers.hpp"
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <thread>

int main() {
    std::stop_source stop;
    const auto signals = jamnntpd::SignalStop::create(stop);
    if (!signals || jamnntpd::SignalStop::create(stop)) {
        return 1;
    }
    std::mutex mutex;
    std::condition_variable_any changed;
    std::promise<void> started;
    jamnntpd::WorkerPool workers{1, {}};
    if (!workers.start([&](std::stop_token token) {
            std::unique_lock lock{mutex};
            started.set_value();
            changed.wait(lock, token, [] { return false; });
        })) {
        return 2;
    }
    if (started.get_future().wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
        return 3;
    }
    std::cout << "READY\n" << std::flush;
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    workers.stop_join();
    return workers.active() == 0 ? 0 : 4;
}
