#ifndef KICKMSG_OS_FUTEX_H
#define KICKMSG_OS_FUTEX_H

#include <atomic>
#include <cstdint>

#include "kickmsg/os/Time.h"

namespace kickmsg
{
    /// Block until the low 32 bits of \p word differ from \p expected, or \p timeout
    /// elapses. Negative errno: 0 on wake, spurious wake or a value that already
    /// differed, -ETIMEDOUT on timeout, -errno otherwise.
    int futex_wait(std::atomic<uint64_t>& word, uint64_t expected, nanoseconds timeout);

    /// Wake all threads/processes blocked on \p word.
    void futex_wake_all(std::atomic<uint64_t>& word);
}

#endif
