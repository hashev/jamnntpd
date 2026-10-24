#include "signals.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <pthread.h>
#endif

namespace jamnntpd {
namespace {
auto signal_owner() -> std::atomic_flag & {
    static std::atomic_flag owner{};
    return owner;
}
#ifdef _WIN32
struct ConsoleFlags {
    volatile LONG requested{};
    volatile LONG completed{};
    volatile LONG handlers{};
};
auto console_flags() -> ConsoleFlags & {
    static ConsoleFlags flags;
    return flags;
}
auto WINAPI console_control(DWORD event) -> BOOL {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT &&
        event != CTRL_SHUTDOWN_EVENT && event != CTRL_LOGOFF_EVENT) {
        return FALSE;
    }
    auto &flags = console_flags();
    InterlockedIncrement(&flags.handlers);
    InterlockedExchange(&flags.requested, 1);
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        constexpr DWORD interval = 10;
        constexpr DWORD maximum_wait = 8000;
        for (DWORD elapsed = 0;
             elapsed < maximum_wait && InterlockedCompareExchange(&flags.completed, 0, 0) == 0;
             elapsed += interval) {
            Sleep(interval);
        }
    }
    InterlockedDecrement(&flags.handlers);
    return TRUE;
}
#endif
} // namespace
struct SignalStop::Impl {
    Impl() = default;
    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;
    std::jthread watcher;
    bool claimed{};
#ifdef _WIN32
    bool installed{};
#else
    sigset_t previous{};
    bool blocked{};
#endif
    ~Impl() {
        watcher.request_stop();
#ifdef _WIN32
        if (claimed) {
            auto &flags = console_flags();
            InterlockedExchange(&flags.completed, 1);
            if (installed) {
                SetConsoleCtrlHandler(console_control, FALSE);
            }
            while (InterlockedCompareExchange(&flags.handlers, 0, 0) != 0) {
                Sleep(1);
            }
        }
#else
        if (watcher.joinable()) {
            // Wake only the sigwait thread; this signal stays blocked in all workers.
            static_cast<void>(pthread_kill(watcher.native_handle(), SIGUSR1));
        }
#endif
        if (watcher.joinable()) {
            watcher.join();
        }
#ifndef _WIN32
        if (blocked) {
            static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous, nullptr));
        }
#endif
        if (claimed) {
            signal_owner().clear();
        }
    }
};
SignalStop::SignalStop(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SignalStop::~SignalStop() = default;
SignalStop::SignalStop(SignalStop &&) noexcept = default;
auto SignalStop::create(std::stop_source source) -> Result<SignalStop> {
    try {
        auto impl = std::make_unique<Impl>();
        if (signal_owner().test_and_set()) {
            return failure(ErrorKind::input, "Termination signal owner already exists");
        }
        impl->claimed = true;
#ifdef _WIN32
        auto &flags = console_flags();
        InterlockedExchange(&flags.requested, 0);
        InterlockedExchange(&flags.completed, 0);
        if (SetConsoleCtrlHandler(console_control, TRUE) == 0) {
            return failure(ErrorKind::io, "Cannot install console termination handler",
                           static_cast<int>(GetLastError()));
        }
        impl->installed = true;
        impl->watcher =
            std::jthread{[source = std::move(source)](const std::stop_token &stop) mutable -> void {
                constexpr auto interval = std::chrono::milliseconds{20};
                while (!stop.stop_requested()) {
                    if (InterlockedCompareExchange(&console_flags().requested, 0, 0) != 0) {
                        source.request_stop();
                        return;
                    }
                    std::this_thread::sleep_for(interval);
                }
            }};
#else
        sigset_t signals{};
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        sigaddset(&signals, SIGUSR1);
        const int blocked = pthread_sigmask(SIG_BLOCK, &signals, &impl->previous);
        if (blocked != 0) {
            return failure(ErrorKind::io, "Cannot block termination signals", blocked);
        }
        impl->blocked = true;
        impl->watcher = std::jthread{
            [signals, source = std::move(source)](const std::stop_token &stop) mutable -> void {
                while (!stop.stop_requested()) {
                    int signal{};
                    const int result = sigwait(&signals, &signal);
                    if (result != 0 || signal == SIGINT || signal == SIGTERM) {
                        source.request_stop();
                        return;
                    }
                }
            }};
#endif
        return SignalStop{std::move(impl)};
    } catch (const std::system_error &error) {
        return failure(ErrorKind::io, "Cannot start termination watcher", error.code().value());
    } catch (const std::bad_alloc &) {
        return failure(ErrorKind::io, "Cannot allocate termination watcher");
    }
}
} // namespace jamnntpd
