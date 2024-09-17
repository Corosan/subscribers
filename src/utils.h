#include <cassert>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <mutex>

#include <unistd.h>

// The same as std::assert, but evaluates an expression even in release build
// mode
#define verify(expr) \
    do { \
        bool r = (expr); \
        assert(r); \
    } while (false)

// Allows to output any data into specified ostream under lock
class sync_logger {
    // While there is a live object of this type, no other thread can
    // output anything using the sync_logger object
    struct step_out {
        std::unique_lock<std::mutex> m_l;
        std::ostream& m_os;
        bool m_add_endl;

        template <class T>
        step_out& operator<<(T&& obj) {
            m_os << std::forward<T>(obj);
            return *this;
        }
        step_out& operator<<(std::ostream& (*func)(std::ostream&)) {
            m_os << func;
            return *this;
        }
        ~step_out() {
            if (m_add_endl)
                m_os << std::endl;
        }
    };

public:
    sync_logger(std::ostream& os) : m_os(os) {}

    // Prefixes an output with steady clock's seconds and milliseconds
    step_out operator()(bool add_endl = true) {
        ::timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);

        std::unique_lock l{m_mutex};
        m_os << std::dec << ts.tv_sec << '.'
            << std::setw(3) << std::setfill('0') << ts.tv_nsec / 1'000'000L
            << " [" << ::gettid() << "] ";
        return {std::move(l), m_os, add_endl};
    }

private:
    std::mutex m_mutex;
    std::ostream& m_os;
};
