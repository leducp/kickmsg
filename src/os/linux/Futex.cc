#include "kickmsg/os/Futex.h"

#include <cerrno>
#include <climits>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace kickmsg
{
    // The futex word is the LOW 32 bits of the 64-bit counter; on a
    // big-endian target &word addresses the HIGH half and the kernel-side
    // value check silently breaks (lost wakeups until timeout).
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
        "futex word aliasing assumes the low half of write_pos at offset 0");

    bool futex_wait(std::atomic<uint64_t>& word, uint64_t expected, nanoseconds timeout)
    {
        auto* addr = reinterpret_cast<uint32_t*>(&word);
        auto  val  = static_cast<uint32_t>(expected);

        struct timespec ts = to_timespec(timeout);

        long rc = syscall(SYS_futex, addr, FUTEX_WAIT, val, &ts, nullptr, 0);
        return not (rc == -1 and errno == ETIMEDOUT);
    }

    void futex_wake_all(std::atomic<uint64_t>& word)
    {
        auto* addr = reinterpret_cast<uint32_t*>(&word);
        syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    }
}
