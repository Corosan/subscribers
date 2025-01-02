#include "utils.h"

#include <list>
#include <vector>
#include <functional>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <thread>

sync_logger g_sync_logger(std::cout);


// Trivial multithreaded implementation of notifier with an exclusive lock on
// every list iteration cycle while delivering events
template <class ... Args> class notifier {
public:
    typedef int sub_id_t;

private:
    // State managed for each subscription came from external world
    struct subscription {
        std::function<void(Args ...)> m_callback;
        const sub_id_t m_id;
        // ids of threads which currently execute this subscription's callback
        std::vector<std::thread::id> m_active_cycle_threads;
        std::condition_variable m_waiter;
        bool m_unsubscribe_from_callback = false;

        subscription(std::function<void(Args ...)> c, sub_id_t id)
            : m_callback(std::move(c)), m_id(id)
        {}
    };

public:
    // Subscribe provided callback to be called later when this object's
    // notify() method is called.
    //
    // Returns an opaque id which can be used for unsubscribing later. Simpler
    // is to use a std::function itself as an id for unsubscription, but c++
    // design such is that comparing arbitrary function objects is not
    // supported.
    sub_id_t subscribe(std::function<void(Args ...)> callback) {
        std::lock_guard l{m_list_mtx};
        m_list.emplace_back(std::move(callback), m_next_id);
        return m_next_id++;
    }

    // Remove previously made subscription by a subscription opague id. The
    // method guarantees that the subscriber's callback is not executed when
    // it's finished. When the method is called from a callback, it's guaranteed
    // that no other thread executes the same callback and nobody will execute
    // it in future.  It will be disposed when this callback returns.
    //
    // Returns status whether a notifier with specified id was really
    // unsubscribed
    bool unsubscribe(sub_id_t id) {
        std::unique_lock l{m_list_mtx};

        auto it = find_if(m_list.begin(), m_list.end(),
            [id](auto& v){ return v.m_id == id; });

        if (it != m_list.end()) {
            auto& threads = it->m_active_cycle_threads;
            auto thread_it = find(threads.begin(), threads.end(),
                std::this_thread::get_id());

            if (thread_it == threads.end()) {
                // Trivial case when the unsubscribe operation is called not
                // from some subscriber's callback
                it->m_waiter.wait(l, [&it, &threads]{ return threads.empty(); });
                m_list.erase(it);
                return true;
            } else {
                // This subscription object will be removed by a notification
                // delivery cycle eventually, which has originated a call chain
                // yielded to this unsubscribe call.
                it->m_unsubscribe_from_callback = true;
                it->m_waiter.wait(l, [&it, &threads]{ return threads.size() <= 1; });
                return true;
            }
        }
        return false;
    }

    // Execute every subscriber's callback sequentially. Can be called from
    // multiple threads. A lock is held only for subscription list
    // iteration/manipulation.
    void notify(Args ... args) {
        std::list<subscription> garbage;
        std::unique_lock l{m_list_mtx};

        for (auto it = m_list.begin(); it != m_list.end(); ) {
            if (it->m_unsubscribe_from_callback) {
                ++it;
                continue;
            }
            auto& threads = it->m_active_cycle_threads;

            // It's not a good to touch a heap allocator at this fast delivery
            // cycle. But an allocation inside this container is expected at
            // beginning phase only - the active threads list not going to grow
            // in future usually
            threads.push_back(std::this_thread::get_id());
            l.unlock();

            try {
                // Note that no std::forward<> optimization here because
                // arguments can't be forwarded into more than one subscriber -
                // all but the first one will get giglets
                it->m_callback(args ...);
            } catch (...) {
            }

            l.lock();
            threads.erase(
                find(threads.begin(), threads.end(), std::this_thread::get_id()));

            // If all callbacks have gone (no active threads registered inside
            // the subscription), issue a notification on the condition variable
            // for somebody who may wait on it inside an unsubscribe()
            // operation.
            // If the only thread is registered and a flag about pending
            // unsubscription is set, issue a notification for the only live
            // callback so it can return from the unsubscribe operation.
            if (threads.empty() || (threads.size() == 1 && it->m_unsubscribe_from_callback))
                it->m_waiter.notify_all();
            if (threads.empty() && it->m_unsubscribe_from_callback)
                garbage.splice(garbage.begin(), m_list, it++);
            else
                ++it;
        }
        // Note that garbage will be cleared after the m_list_mtx is unlocked
    }

    std::size_t count() const { return m_list.size(); }

private:
    sub_id_t m_next_id = 0;
    std::mutex m_list_mtx;
    std::list<subscription> m_list;
};


int main() {
    notifier<int, char> s;

    g_sync_logger() << "---- test1 ----";

    // Trivial test for delivering an event on the same thread
    auto id1 = s.subscribe([](int, char){ g_sync_logger() << "subscriber 1 executed"; });
    s.notify(1, 'a');
    verify(s.unsubscribe(id1));
    verify(s.count() == 0);

    s.notify(2, 'b'); // it should be nothing to notify

    g_sync_logger() << "---- test2 ----";

    // MT test for unsubscribing while delivering an event
    id1 = s.subscribe([](int, char){
        g_sync_logger() << "subscriber 2 started";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        g_sync_logger() << "subscrber 2 finished";
    });

    std::thread t{[&s]{ s.notify(3, 'c'); }};

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_sync_logger() << "trying to unsubscribe the subscriber 2 ";
    verify(s.unsubscribe(id1));
    verify(s.count() == 0);
    g_sync_logger() << "finished unsubscription of the subscriber 2";

    t.join();

    g_sync_logger() << "---- test3 ----";

    // MT test where the same subscription is called from two threads and one of them tries to
    // unsubscribe while other works for some time
    const auto main_thread_id = std::this_thread::get_id();
    id1 = s.subscribe([&id1, &s, main_thread_id](int, char){
        if (main_thread_id == std::this_thread::get_id()) {
            g_sync_logger() << "subscriber 3 started from thread 1";
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            g_sync_logger() << "subscriber 3 - try to unsubscribe";
            s.unsubscribe(id1);
            g_sync_logger() << "subscriber 3 finished on thread 1";
        } else {
            g_sync_logger() << "subscriber 3 started from thread 2";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            g_sync_logger() << "subscriber 3 finished on thread 2";
        }});

    t = std::thread{[&s]{ s.notify(4, 'd'); }};
    s.notify(5, 'e');

    t.join();

    verify(! s.unsubscribe(id1));
    verify(s.count() == 0);
}
