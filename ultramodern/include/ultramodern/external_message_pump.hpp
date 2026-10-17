#ifndef __ULTRAMODERN_EXTERNAL_MESSAGE_PUMP_HPP__
#define __ULTRAMODERN_EXTERNAL_MESSAGE_PUMP_HPP__

// [wcw fix] The idle thread's wait for external messages (aki-roadmap#90), kept free of rdram and the OS so the wcw
// unit tests can drive it with a scripted queue (librecomp/tests/wcw/external_message_pump_tests.cpp).
//
// External messages (VI retrace, SP/DP/PI/SI completions, timers) are posted by host threads into one queue and
// delivered to the game's message queues by game threads. A requeue_if_blocked message whose target queue is full is
// retried for a grace window and then dropped. While a game leaves completions it never reads (World Tour and VPW64
// fire-and-forget DMAs), that queue holds a standing backlog of them.
//
// When every game thread is blocked, the idle thread waits here. The backlog can't become deliverable while it waits:
// only a game thread empties a game queue, and no game thread can run until a NEW message is delivered. The old wait
// woke at once on the backlog, delivered nothing, re-enqueued it and slept 1 ms, so a retrace or task-done message
// arriving during that sleep reached the game 1-2 ms late (p50 1.5 ms in World Tour). Now the backlog is held aside
// and the wait blocks on the queue itself, so a new message wakes it immediately.

#include <chrono>
#include <cstdint>
#include <vector>

namespace ultramodern::external_message_pump {
    enum class Delivery {
        Delivered, // the message reached its queue
        Retry,     // the queue is full; keep the message for a later attempt
        Drop,      // the queue is full and the message may not wait (or its grace window is over)
    };

    // What happens to a message whose target queue is full. Hardware drops a NOBLOCK post into a full queue; the
    // runtime keeps requeue_if_blocked messages for grace_ms first, because its DMAs complete instantly and the game
    // may simply not have drained the queue yet.
    inline Delivery undeliverable(bool requeue_if_blocked, int64_t now_ms, int64_t enqueue_ms, int64_t grace_ms) {
        if (requeue_if_blocked && now_ms - enqueue_ms <= grace_ms) {
            return Delivery::Retry;
        }
        return Delivery::Drop;
    }

    struct WaitResult {
        int delivered = 0;
        int dropped = 0;
        int requeued = 0;      // backlog messages put back on the queue for the game threads' drains
        int backlog_waits = 0; // waits for a new message because only the backlog was there
    };

    // Waits for external messages and delivers what it can; returns once something was delivered, or there is no
    // backlog left, or backlog_wait passed with no new message (a safety bound; the caller loops).
    //   Queue:   wait_dequeue(Msg&), wait_dequeue_timed(Msg&, std::chrono::microseconds) -> bool,
    //            try_dequeue(Msg&) -> bool, enqueue(const Msg&)
    //   attempt: Delivery(Msg&), tries to deliver one message
    //   on_wake: void(), called after every wake (the caller's shutdown check)
    // backlog is caller-owned scratch storage (reused to avoid allocating per wait).
    template <typename Msg, typename Queue, typename Attempt, typename OnWake>
    WaitResult wait_and_deliver(Queue& queue, std::vector<Msg>& backlog, std::chrono::microseconds backlog_wait,
        Attempt&& attempt, OnWake&& on_wake) {
        WaitResult result;
        backlog.clear();
        Msg msg;
        queue.wait_dequeue(msg);
        on_wake();
        for (;;) {
            // Drain everything available: processing one message per wake let an undeliverable message cycle
            // straight back out of the queue and starve every other producer.
            do {
                switch (attempt(msg)) {
                    case Delivery::Delivered:
                        result.delivered++;
                        break;
                    case Delivery::Retry:
                        backlog.push_back(msg);
                        break;
                    case Delivery::Drop:
                        result.dropped++;
                        break;
                }
            } while (queue.try_dequeue(msg));

            if (result.delivered > 0 || backlog.empty()) {
                break;
            }
            // Only the backlog was there. Hold it aside and wait for a new message instead of re-enqueueing it
            // (which would wake this wait again at once) and sleeping (which delays the next real message).
            result.backlog_waits++;
            if (!queue.wait_dequeue_timed(msg, backlog_wait)) {
                break;
            }
            on_wake();
        }
        result.requeued = static_cast<int>(backlog.size());
        for (const Msg& m : backlog) {
            queue.enqueue(m);
        }
        return result;
    }
}

#endif
