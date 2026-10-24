#include "core/workers.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

using namespace jamnntpd;
TEST_CASE("Admission reserves capacity before startup and leases release on every exit") {
    Admission admission{1};
    {
        auto lease = admission.reserve();
        REQUIRE(lease);
        CHECK(admission.active() == 1);
        CHECK_FALSE(admission.reserve());
        auto moved = std::move(*lease);
        auto moved_again = std::move(moved);
        moved = std::move(moved_again);
        CHECK(admission.active() == 1);
    }
    CHECK(admission.active() == 0);
    CHECK(admission.reserve());
    CHECK_FALSE(Admission{0}.reserve());
}
TEST_CASE("Concurrent admission has no oversubscription across one hundred contested rounds") {
    constexpr int workers = 8;
    for (int iteration = 0; iteration < 100; ++iteration) {
        Admission admission{3};
        std::barrier start{workers};
        std::barrier reserved{workers};
        std::atomic<int> successes{};
        std::vector<std::jthread> threads;
        for (int index = 0; index < workers; ++index) {
            threads.emplace_back([&] {
                start.arrive_and_wait();
                auto lease = admission.reserve();
                if (lease) {
                    ++successes;
                }
                reserved.arrive_and_wait();
            });
        }
        threads.clear();
        CHECK(successes.load() == 3);
        CHECK(admission.active() == 0);
    }
}
TEST_CASE("Worker startup failure returns capacity and owns move-only tasks") {
    std::atomic<int> reports{};
    WorkerPool pool{1, [&](std::string_view) { ++reports; },
                    [](Worker) -> std::jthread {
                        throw std::system_error{
                            std::make_error_code(std::errc::resource_unavailable_try_again)};
                    }};
    CHECK_FALSE(pool.start([](std::stop_token) {}));
    CHECK(pool.active() == 0);
    CHECK_FALSE(pool.start([](std::stop_token) {}));
    CHECK(pool.active() == 0);
    WorkerPool no_thread{1, {}, [](Worker) { return std::jthread{}; }};
    CHECK_FALSE(no_thread.start([](std::stop_token) {}));
    CHECK(no_thread.active() == 0);
    WorkerPool allocation_failure{1, {}, [](Worker) -> std::jthread { throw std::bad_alloc{}; }};
    CHECK_FALSE(allocation_failure.start([](std::stop_token) {}));
    CHECK(allocation_failure.active() == 0);
    auto owned = std::make_unique<int>(42);
    std::promise<int> result;
    WorkerPool successful{1, {}};
    REQUIRE(successful.start(
        [owner = std::move(owned), &result](std::stop_token) { result.set_value(*owner); }));
    CHECK(result.get_future().get() == 42);
    successful.stop_join();
    CHECK(successful.active() == 0);
    CHECK_FALSE(owned);
    CHECK(reports == 0);
}
TEST_CASE(
    "Worker stop requests precede all joins and exception reporting cannot terminate threads") {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::promise<void> started;
    WorkerPool pool{1, {}};
    REQUIRE(pool.start([&](std::stop_token stop) {
        std::unique_lock lock{mutex};
        started.set_value();
        changed.wait(lock, stop, [] { return false; });
    }));
    auto future = started.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK_FALSE(pool.start([](std::stop_token) {}));
    const auto beginning = std::chrono::steady_clock::now();
    pool.stop_join();
    CHECK(std::chrono::steady_clock::now() - beginning < std::chrono::seconds{2});
    CHECK(pool.active() == 0);
    CHECK_FALSE(pool.start([](std::stop_token) {}));
    std::atomic<int> reported{};
    {
        WorkerPool exceptions{1, [&](std::string_view text) {
                                  if (text == "Session worker failed") {
                                      ++reported;
                                  }
                                  throw std::runtime_error{"broken sink"};
                              }};
        REQUIRE(exceptions.start(
            [](std::stop_token) { throw std::runtime_error{"private sentinel"}; }));
    }
    CHECK(reported == 1);
}
