#include "utils.h"

#include <list>
#include <functional>
#include <shared_mutex>
#include <atomic>
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
        // how many notify cycles use this object right now
        std::atomic<unsigned> m_refs = 0;
        std::condition_variable_any m_waiter;

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
    // it's finished. Though it can't be called from a subscription execution
    // chain itself.
    //
    // Returns status whether a notifier with specified id was really
    // unsubscribed
    bool unsubscribe(sub_id_t id) {
        std::unique_lock l{m_list_mtx};

        auto it = find_if(m_list.begin(), m_list.end(),
            [id](auto& v){ return v.m_id == id; });

        if (it != m_list.end()) {
            it->m_waiter.wait(l, [&it](){
                return it->m_refs.load(std::memory_order_relaxed) == 0;
            });
            m_list.erase(it);
            return true;
        }
        return false;
    }

    // Execute every subscriber's callback sequentially. Can be called from
    // multiple threads. A lock is held only for subscription list
    // iteration/manipulation.
    void notify(Args ... args) {
        std::shared_lock l{m_list_mtx};

        for (auto &s : m_list) {
            s.m_refs.fetch_add(1, std::memory_order_relaxed);
            l.unlock();

            try {
                s.m_callback(args ...);
            } catch (...) {
            }

            l.lock();
            if (s.m_refs.fetch_sub(1, std::memory_order_relaxed) == 1)
                s.m_waiter.notify_all();
        }
    }

private:
    sub_id_t m_next_id = 0;
    std::shared_mutex m_list_mtx;
    std::list<subscription> m_list;
};


int main() {
    notifier<int> s;

    g_sync_logger() << "---- test1 ----";

    // Trivial test for delivering an event on the same thread
    auto id1 = s.subscribe(
        [](int){ g_sync_logger() << "subscriber 1 executed"; });
    s.notify(1);
    verify(s.unsubscribe(id1));

    s.notify(2); // nothing to notify

    g_sync_logger() << "---- test2 ----";

    // MT test for unsubscribing while delivering an event
    id1 = s.subscribe([](int){
        g_sync_logger() << "subscriber 2 started";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        g_sync_logger() << "subscriber 2 finihed";
    });

    std::thread t{[&s](){ s.notify(3); }};

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_sync_logger() << "trying to unsubscribe the subscriber 2";
    verify(s.unsubscribe(id1));
    g_sync_logger() << "finished unsubscription of the subscriber 2";

    t.join();
}
