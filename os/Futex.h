#ifndef KICKMSG_OS_FUTEX_H
#define KICKMSG_OS_FUTEX_H

#include <atomic>
#include <cstdint>

#include "kickmsg/os/Time.h"

namespace kickmsg
{
    /// Block until the low 32 bits of \p word differ from \p expected,
    /// or \p timeout elapses.
    /// Returns false on timeout, true on wake / spurious / value-change.
    bool futex_wait(std::atomic<uint64_t>& word, uint64_t expected, nanoseconds timeout);

    /// Wake all threads/processes blocked on \p word.
    void futex_wake_all(std::atomic<uint64_t>& word);
}

#endif
