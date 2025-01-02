#include "utils.h"

#include <list>
#include <vector>
#include <functional>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <type_traits>
#include <thread>
#include <tuple>

sync_logger g_sync_logger(std::cout);

// The same logic as in notifs-3.cpp but greatly simplified by replacing cb_storage with
// std::function<void(void*)> ... Thanks to @yandexgas (https://github.com/yandexgas) for the
// idea.
//
// Note that this implementation doesn't have an option for specifying maximum size of in-place
// stored callable.

namespace details {

class notifier_base {
public:
    typedef int sub_id_t;

protected:
    ~notifier_base() = default;

    struct subscription {
        std::function<void(void*)> m_callback;
        const sub_id_t m_id;
        // ids of threads which currently execute this subscription's callback
        std::vector<std::thread::id> m_active_cycle_threads;
        std::condition_variable m_waiter;
        bool m_unsubscribe_from_callback = false;

        subscription(sub_id_t id) : m_id(id)
        {}
    };

    std::tuple<subscription&, std::unique_lock<std::mutex>, sub_id_t>
    subscribe() {
        std::unique_lock l{m_list_mtx};
        auto r = std::make_tuple(
            std::ref(m_list.emplace_back(m_next_id)),
            std::move(l),
            m_next_id);
        ++m_next_id;
        return r;
    }

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

    void notify(void* ctx) {
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
                it->m_callback(ctx);
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

} // ns details

// Trivial multithreaded implementation of notifier with an exclusive lock on
// every list iteration cycle while delivering events
template <class ... Args> class notifier : public details::notifier_base {
public:
    // Subscribe provided callback to be called later when this object's
    // notify() method is called.
    //
    // Returns an opaque id which can be used for unsubscribing later. Simpler
    // is to use a std::function itself as an id for unsubscription, but c++
    // design such is that comparing arbitrary function objects is not
    // supported.
    template <class F>
    sub_id_t subscribe(F&& cb) {
        auto [s, l, id] = notifier_base::subscribe();
        s.m_callback = [f = std::forward<F>(cb)](void* ctx){
            std::apply(f, *static_cast<std::tuple<Args ...>*>(ctx));
        };
        return id;
    }

    // Remove previously made subscription by a subscription opague id. The
    // method guarantees that the subscriber's callback is not executed when
    // it's finished. When the method is called from a callback, it's guaranteed
    // that no other thread executes the same callback and nobody will execute
    // it in future.  It will be disposed when this callback returns.
    //
    // Returns status whether a notifier with specified id was really
    // unsubscribed
    using notifier_base::unsubscribe;

    // Execute every subscriber's callback sequentially. Can be called from
    // multiple threads. A lock is held only for subscription list
    // iteration/manipulation.
    template <class ... ArgsI>
    void notify(ArgsI&& ... args) {
        std::tuple<Args...> args_tuple{std::forward<ArgsI>(args) ...};
        notifier_base::notify(&args_tuple);
    }

    using notifier_base::count;
};


struct test_move_only {
    test_move_only() = default;
    test_move_only(test_move_only&&) = default;
    test_move_only& operator=(test_move_only&&) = default;
};

int main() {
    notifier<int, char, const test_move_only&> s;

    g_sync_logger() << "---- test1 ----";

    // Trivial test for delivering an event on the same thread
    auto id1 = s.subscribe([](int, char, auto&){
        asm volatile("nop");
        g_sync_logger() << "subscriber 1 executed";
        asm volatile("nop");
    });

    asm volatile("nop");
    s.notify(1, 'a', test_move_only{});
    asm volatile("nop");

    verify(s.unsubscribe(id1));
    verify(s.count() == 0);

    s.notify(2, 'b', test_move_only{}); // it should be nothing to notify

    g_sync_logger() << "---- test2 ----";

    // MT test for unsubscribing while delivering an event
    id1 = s.subscribe([](int, char, auto&){
        g_sync_logger() << "subscriber 2 started";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        g_sync_logger() << "subscrber 2 finished";
    });

    std::thread t{[&s]{ s.notify(3, 'c', test_move_only{}); }};

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
    id1 = s.subscribe([&id1, &s, main_thread_id](int, char, auto&){
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

    t = std::thread{[&s]{ s.notify(4, 'd', test_move_only{}); }};
    s.notify(5, 'e', test_move_only{});

    t.join();

    verify(! s.unsubscribe(id1));
    verify(s.count() == 0);
}
