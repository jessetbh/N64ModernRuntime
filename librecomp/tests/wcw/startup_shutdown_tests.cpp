//
// N64ModernRuntime (librecomp)
//

// [wcw] Unit tests for librecomp/startup_shutdown.hpp (quitting while the game is still starting up). Built and run
// as part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build. Each test names the bug it
// guards against where there was one.

#include "librecomp/startup_shutdown.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "wcw_test.h"

using namespace std::chrono_literals;

namespace {
    // A cross-thread call that only completes once the owner thread services it, like a Win32 SendMessage to a
    // window owned by the main thread: the caller blocks until the owner pumps its messages.
    struct OwnerThreadCall {
        std::mutex mutex;
        std::condition_variable cv;
        bool pending = false;
        bool serviced = false;

        void call() {
            std::unique_lock lock{ mutex };
            pending = true;
            cv.wait(lock, [this] { return serviced; });
        }

        void service() {
            std::lock_guard lock{ mutex };
            if (pending && !serviced) {
                serviced = true;
                cv.notify_all();
            }
        }
    };

    void sleep_1ms() {
        std::this_thread::sleep_for(1ms);
    }
}

TEST_CASE("quit during startup: shutdown completes while the renderer still needs the window (#73 hang)") {
    // The game start thread (preinit) waits on the gfx thread, which makes a window call that only the main thread
    // can service. Quit has already been requested, so the main thread has left its normal pump loop: it must keep
    // pumping until startup is done, then join. The old code went straight to the join and hung forever.
    OwnerThreadCall window_call;
    std::atomic_bool startup_done = false;
    std::thread game_start_thread{ [&] {
        window_call.call();          // RT64 ApplicationWindow::setFullScreen -> SetWindowLongPtr
        startup_done.store(true);    // ultramodern::preinit returned
    } };

    std::atomic_bool shutdown_finished = false;
    std::thread main_thread{ [&] {
        recomp::pump_until_startup_done(startup_done, [&] { window_call.service(); }, sleep_1ms);
        game_start_thread.join();
        shutdown_finished.store(true);
    } };

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!shutdown_finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(shutdown_finished.load());

    // On failure, break the deadlock ourselves so the test binary still exits and reports.
    window_call.service();
    main_thread.join();
}

TEST_CASE("quit after startup: no extra pumping, shutdown goes straight on as before") {
    std::atomic_bool startup_done = true;
    int pumps = 0;
    int sleeps = 0;
    recomp::pump_until_startup_done(startup_done, [&] { pumps++; }, [&] { sleeps++; });
    CHECK_EQ(pumps, 0);
    CHECK_EQ(sleeps, 0);
}

TEST_CASE("quit during startup: pumping stops as soon as startup is done") {
    std::atomic_bool startup_done = false;
    int pumps = 0;
    recomp::pump_until_startup_done(startup_done, [&] {
        pumps++;
        if (pumps == 3) {
            startup_done.store(true);
        }
    }, [] {});
    CHECK_EQ(pumps, 3);
}

int main() {
    return WcwTest::runAll("librecomp wcw tests");
}
