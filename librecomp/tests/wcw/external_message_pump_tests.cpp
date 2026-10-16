//
// N64ModernRuntime (ultramodern)
//

// [wcw] Unit tests for ultramodern/external_message_pump.hpp: the idle thread's wait for external messages while
// undeliverable completions are queued (aki-roadmap#90). Built and run with librecomp_wcw_tests; a failure fails the
// build.

#include "ultramodern/external_message_pump.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <thread>
#include <vector>

#include "blockingconcurrentqueue.h"

#include "wcw_test.h"

using namespace ultramodern::external_message_pump;
using namespace std::chrono_literals;

namespace {
    struct Msg {
        int id = 0;
        bool deliverable = false;
        bool requeue = true;
    };

    // A scripted queue: `queued` is what is already waiting; each wait_dequeue_timed call takes the next entry of
    // `arrivals` (a message posted while the waiter blocks), or times out when there is none.
    struct FakeQueue {
        std::deque<Msg> queued;
        std::deque<Msg> arrivals;
        int timed_waits = 0;
        int queued_at_timed_wait = 0; // messages already queued when a timed wait started (must stay 0)
        std::chrono::microseconds last_timeout{ 0 };

        void wait_dequeue(Msg& m) {
            if (queued.empty()) {
                CHECK(!arrivals.empty()); // a real queue would block forever
                queued.push_back(arrivals.front());
                arrivals.pop_front();
            }
            m = queued.front();
            queued.pop_front();
        }

        bool wait_dequeue_timed(Msg& m, std::chrono::microseconds timeout) {
            timed_waits++;
            last_timeout = timeout;
            queued_at_timed_wait += static_cast<int>(queued.size());
            if (!queued.empty()) { // re-enqueued backlog: the wait returns at once (the old busy cycle)
                m = queued.front();
                queued.pop_front();
                return true;
            }
            if (arrivals.empty()) {
                return false;
            }
            m = arrivals.front();
            arrivals.pop_front();
            return true;
        }

        bool try_dequeue(Msg& m) {
            if (queued.empty()) {
                return false;
            }
            m = queued.front();
            queued.pop_front();
            return true;
        }

        void enqueue(const Msg& m) {
            queued.push_back(m);
        }
    };

    Msg backlog_msg(int id) {
        return Msg{ id, false, true };
    }

    Msg new_msg(int id) {
        return Msg{ id, true, false };
    }

    struct Run {
        WaitResult result;
        std::vector<int> delivered_ids;
        int wakes = 0;
    };

    Run run(FakeQueue& q, std::chrono::microseconds backlog_wait = 16ms) {
        Run r;
        std::vector<Msg> scratch;
        r.result = wait_and_deliver(q, scratch, backlog_wait,
            [&](Msg& m) {
                if (m.deliverable) {
                    r.delivered_ids.push_back(m.id);
                    return Delivery::Delivered;
                }
                return m.requeue ? Delivery::Retry : Delivery::Drop;
            },
            [&] { r.wakes++; });
        return r;
    }

    std::vector<int> ids(const std::deque<Msg>& q) {
        std::vector<int> out;
        for (const Msg& m : q) {
            out.push_back(m.id);
        }
        return out;
    }
}

// Waiting with only the backlog queued

TEST_CASE("backlog: a message posted while only the backlog is queued is delivered by the same wait (#90 latency)") {
    // World Tour / VPW64: a standing backlog of fire-and-forget DMA completions, all game threads idle, then the VI
    // retrace arrives. The old wait delivered nothing, re-enqueued the backlog and slept 1 ms, and the retrace sat in
    // the queue for that sleep. Now the wait blocks on the queue and the retrace is delivered when it arrives.
    FakeQueue q;
    q.queued = { backlog_msg(1), backlog_msg(2) };
    q.arrivals = { new_msg(10) };
    const Run r = run(q);
    CHECK_EQ(r.result.delivered, 1);
    CHECK(r.delivered_ids == std::vector<int>{ 10 });
    CHECK_EQ(r.result.backlog_waits, 1);
    CHECK_EQ(q.timed_waits, 1);
}

TEST_CASE("backlog: it is held aside during the wait, not re-enqueued (that woke the wait at once, ~550 times a second)") {
    FakeQueue q;
    q.queued = { backlog_msg(1), backlog_msg(2), backlog_msg(3) };
    q.arrivals = { new_msg(10) };
    run(q);
    CHECK_EQ(q.queued_at_timed_wait, 0);
}

TEST_CASE("backlog: it goes back on the queue in order afterwards, for the game threads' drains") {
    FakeQueue q;
    q.queued = { backlog_msg(1), backlog_msg(2), backlog_msg(3) };
    q.arrivals = { new_msg(10) };
    const Run r = run(q);
    CHECK_EQ(r.result.requeued, 3);
    CHECK(ids(q.queued) == (std::vector<int>{ 1, 2, 3 }));
}

TEST_CASE("backlog: with no new message the wait gives up after backlog_wait and keeps the backlog") {
    FakeQueue q;
    q.queued = { backlog_msg(1), backlog_msg(2) };
    const Run r = run(q, 16ms);
    CHECK_EQ(r.result.delivered, 0);
    CHECK_EQ(r.result.requeued, 2);
    CHECK_EQ(q.timed_waits, 1);
    CHECK(q.last_timeout == std::chrono::microseconds{ 16ms });
    CHECK(ids(q.queued) == (std::vector<int>{ 1, 2 }));
}

TEST_CASE("backlog: a new message that is also undeliverable joins the backlog and the wait goes on") {
    FakeQueue q;
    q.queued = { backlog_msg(1) };
    q.arrivals = { backlog_msg(2), new_msg(10) };
    const Run r = run(q);
    CHECK_EQ(r.result.delivered, 1);
    CHECK_EQ(r.result.backlog_waits, 2);
    CHECK(ids(q.queued) == (std::vector<int>{ 1, 2 }));
}

TEST_CASE("backlog: the shutdown check runs after every wake, including wakes inside the backlog wait") {
    FakeQueue q;
    q.queued = { backlog_msg(1) };
    q.arrivals = { backlog_msg(2), new_msg(10) };
    const Run r = run(q);
    CHECK_EQ(r.wakes, 3);
}

// Nothing to hold

TEST_CASE("no backlog: a deliverable message returns at once, without a timed wait") {
    FakeQueue q;
    q.queued = { new_msg(10) };
    const Run r = run(q);
    CHECK_EQ(r.result.delivered, 1);
    CHECK_EQ(q.timed_waits, 0);
    CHECK_EQ(r.wakes, 1);
}

TEST_CASE("no backlog: everything available is drained in one wake, and an undeliverable one is kept") {
    FakeQueue q;
    q.queued = { backlog_msg(1), new_msg(10), new_msg(11) };
    const Run r = run(q);
    CHECK(r.delivered_ids == (std::vector<int>{ 10, 11 }));
    CHECK_EQ(r.result.requeued, 1);
    CHECK_EQ(q.timed_waits, 0);
    CHECK(ids(q.queued) == std::vector<int>{ 1 });
}

TEST_CASE("no backlog: a dropped message counts as dropped and the wait returns") {
    FakeQueue q;
    q.queued = { Msg{ 5, false, false } };
    const Run r = run(q);
    CHECK_EQ(r.result.delivered, 0);
    CHECK_EQ(r.result.dropped, 1);
    CHECK_EQ(q.timed_waits, 0);
    CHECK(q.queued.empty());
}

// Grace window

TEST_CASE("grace: a requeue_if_blocked message is retried up to grace_ms after it was posted, then dropped") {
    CHECK(undeliverable(true, 1000, 0, 1000) == Delivery::Retry);
    CHECK(undeliverable(true, 500, 0, 1000) == Delivery::Retry);
    CHECK(undeliverable(true, 1001, 0, 1000) == Delivery::Drop);
}

TEST_CASE("grace: a message without requeue_if_blocked is dropped when its queue is full (hardware NOBLOCK)") {
    CHECK(undeliverable(false, 0, 0, 1000) == Delivery::Drop);
}

// With the runtime's real queue

TEST_CASE("real queue: a message posted from another thread wakes the backlog wait at once, not at its timeout") {
    moodycamel::BlockingConcurrentQueue<Msg> q;
    for (int i = 0; i < 50; i++) {
        q.enqueue(backlog_msg(i));
    }
    std::atomic<int64_t> posted_us{ 0 };
    auto now_us = [] {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    std::thread producer{ [&] {
        std::this_thread::sleep_for(20ms);
        posted_us.store(now_us());
        q.enqueue(new_msg(1000));
    } };
    std::vector<Msg> scratch;
    int64_t delivered_us = 0;
    // A 2 s safety bound: a wait that only polled or timed out would take that long; waking on the post takes ~us.
    const WaitResult r = wait_and_deliver(q, scratch, std::chrono::microseconds{ 2s },
        [&](Msg& m) {
            if (m.deliverable) {
                delivered_us = now_us();
                return Delivery::Delivered;
            }
            return Delivery::Retry;
        },
        [] {});
    producer.join();
    CHECK_EQ(r.delivered, 1);
    CHECK_EQ(r.requeued, 50);
    CHECK_EQ(static_cast<int>(q.size_approx()), 50);
    CHECK(delivered_us - posted_us.load() < 500000);
}
