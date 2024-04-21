#include "utils.h"

#include <list>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>

sync_logger g_sync_logger(std::cout);


// Trivial multithreaded implementation of subscriber with an exclusive lock
// on every list iteration cycle while delivering events
class subscriber {
    struct subscription {
        std::function<void()> m_callback;
        const int m_id;
        // ids of threads which currently execute this subscription's callback
        std::vector<std::thread::id> m_active_cycle_threads;
        std::condition_variable m_waiter;
        bool m_unsubscribe_from_callback = false;

        subscription(std::function<void()> c, int id)
            : m_callback(std::move(c)), m_id(id)
        {}
    };

public:
    // Returns an opaque id which can be used for unsubscribing later. Simpler is to use
    // a std::function itself as an id for unsubscription, but c++ design such is that
    // comparing arbitrary function objects is not supported.
    int subscribe(std::function<void()> callback) {
        std::lock_guard l{m_listMutex};
        m_list.emplace_back(std::move(callback), m_next_subscr_id);
        return m_next_subscr_id++;
    }

    // Returns status whether a subscriber with specified id was really unsubscribed. It's
    // guaranteed that no callback will be called when the method returns. Except the case when the
    // method itself is called from a callback. When it's called from a callback, it's guaranteed
    // that no other thread executes the same callback and nobody will execute it in future. It's
    // will be disposed when this callback returns.
    bool unsubscribe(int id) {
        std::unique_lock l{m_listMutex};
        auto it = find_if(m_list.begin(), m_list.end(), [id](auto& v){ return v.m_id == id; });
        if (it != m_list.end()) {
            auto& threads = it->m_active_cycle_threads;
            auto thread_it = find(threads.begin(), threads.end(), std::this_thread::get_id());
            if (thread_it == threads.end()) {
                // Trivial case when the unsubscribe operation is called not from some callback
                it->m_waiter.wait(l, [&it, &threads]{ return threads.empty(); });
                m_list.erase(it);
                return true;
            } else {
                // This subscription object will be removed by a notification delivery cycle
                // eventually, which has originated a call chain yielded to this unsubscribe call.
                it->m_unsubscribe_from_callback = true;
                it->m_waiter.wait(l, [&it, &threads]{ return threads.size() <= 1; });
                return true;
            }
        }
        return false;
    }

    // Calls every subscription's callback sequentially. Can be called from multiple threads. A lock
    // is held only for subscription list iteration/manipulation.
    void notify() {
        std::list<subscription> garbage;
        std::unique_lock l{m_listMutex};
        for (auto it = m_list.begin(); it != m_list.end(); ) {
            if (it->m_unsubscribe_from_callback) {
                ++it;
                continue;
            }
            auto& threads = it->m_active_cycle_threads;

            // It's not a good to touch a heap allocator at this fast delivery cycle. But an
            // allocation inside this container is expected at beginning phase only - the active
            // threads list not going to grow in future usually
            threads.push_back(std::this_thread::get_id());
            l.unlock();

            try {
                it->m_callback();
            } catch (...) {}

            l.lock();
            threads.erase(
                find(threads.begin(), threads.end(), std::this_thread::get_id()));

            // If all callbacks have gone (no active threads registered inside the subscription),
            // issue a notification on the condition variable for somebody who may wait on it inside
            // an unsubscribe() operation.
            // If the only thread is registered and a flag about pending unsubscription is set,
            // issue a notification for the only live callback so it can return from the unsubscribe
            // operation.
            if (threads.empty() || (threads.size() == 1 && it->m_unsubscribe_from_callback))
                it->m_waiter.notify_all();
            if (threads.empty() && it->m_unsubscribe_from_callback)
                garbage.splice(garbage.begin(), m_list, it++);
            else
                ++it;
        }
        // Note that garbage will be cleared after the m_listMutex is unlocked
    }

    std::size_t count() const { return m_list.size(); }

private:
    int m_next_subscr_id = 0;
    std::list<subscription> m_list;
    std::mutex m_listMutex;
};

int main() {
    subscriber s;

    g_sync_logger() << "---- test1 ----";

    // Trivial test for delivering an event on the same thread
    int id1 = s.subscribe([]{ g_sync_logger() << "notifier1"; });
    s.notify();
    verify(s.unsubscribe(id1));
    verify(s.count() == 0);

    s.notify(); // it should be nothing to notify

    g_sync_logger() << "---- test2 ----";

    // MT test for unsubscribing while delivering an event
    id1 = s.subscribe([]{
        g_sync_logger() << "notifier2 started";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        g_sync_logger() << "notifier2 finished";
    });

    std::thread t{[&s]{ s.notify(); }};

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_sync_logger() << "trying to unsubscribe notifier2";
    verify(s.unsubscribe(id1));
    verify(s.count() == 0);
    g_sync_logger() << "finished unsubscription of notifier2";

    t.join();

    g_sync_logger() << "---- test3 ----";

    // MT test where the same subscription is called from two threads and one of them tries to
    // unsubscribe while other works for some time
    const auto mainThreadId = std::this_thread::get_id();
    id1 = s.subscribe([&id1, &s, mainThreadId]{
        if (mainThreadId == std::this_thread::get_id()) {
            g_sync_logger() << "notifier3 started from thread 1";
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            g_sync_logger() << "notifier3 - try to unsubscribe";
            s.unsubscribe(id1);
            g_sync_logger() << "notifier3 finished on thread 1";
        } else {
            g_sync_logger() << "notifier3 started from thread 2";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            g_sync_logger() << "notifier3 finished on thread 2";
        }});

    t = std::thread{[&s]{ s.notify(); }};
    s.notify();

    t.join();

    verify(! s.unsubscribe(id1));
    verify(s.count() == 0);
}
