#include "utils.h"

#include <list>
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
        unsigned m_refs = 0;    // how many notify cycles use this object right now
        std::condition_variable m_waiter;

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
        m_list.emplace_back(std::move(callback), m_nextId);
        return m_nextId++;
    }

    // Returns whether a subscriber with specified id was really unsubscribed
    bool unsubscribe(int id) {
        std::unique_lock l{m_listMutex};
        auto it = std::find_if(m_list.begin(), m_list.end(), [id](auto& v){ return v.m_id == id; });
        if (it != m_list.end()) {
            it->m_waiter.wait(l, [&it](){ return ! it->m_refs; });
            m_list.erase(it);
            return true;
        }
        return false;
    }

    void notify() {
        std::unique_lock l{m_listMutex};
        for (auto &s : m_list) {
            ++s.m_refs;
            l.unlock();

            try {
                s.m_callback();
            } catch (...) {}

            l.lock();
            if (! --s.m_refs)
                s.m_waiter.notify_all();
        }
    }

private:
    int m_nextId = 0;
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

    s.notify(); // nothing to notify

    g_sync_logger() << "---- test2 ----";

    // MT test for unsubscribing while delivering an event
    id1 = s.subscribe([]{
        g_sync_logger() << "notifier2 started";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        g_sync_logger() << "notifier2 finihed";
    });

    std::thread t{[&s](){ s.notify(); }};

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_sync_logger() << "trying to unsubscribe notifier2";
    verify(s.unsubscribe(id1));
    g_sync_logger() << "finished unsubscription of notifier2";

    t.join();
}
