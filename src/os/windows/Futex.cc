#include "kickmsg/os/Futex.h"

#include <cerrno>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kickmsg
{
    // The watched word is the LOW 32 bits of the 64-bit counter; on a
    // big-endian target &word addresses the HIGH half and the value check
    // silently breaks (lost wakeups until timeout).  MSVC has no
    // __BYTE_ORDER__, but every Windows target (x86, x64, ARM64) is
    // little-endian.
#if defined(__BYTE_ORDER__)
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
        "WaitOnAddress word aliasing assumes the low half of write_pos at offset 0");
#endif

    int futex_wait(std::atomic<uint64_t>& word, uint64_t expected, nanoseconds timeout)
    {
        auto* addr = reinterpret_cast<void*>(&word);
        auto  val  = static_cast<uint32_t>(expected);

        // to_poll_ms rounds up, so a sub-millisecond budget still waits rather than
        // returning at once and spinning, and clamps to INT_MAX -- below INFINITE, which
        // this must never pass by accident.
        DWORD timeout_ms = static_cast<DWORD>(to_poll_ms(timeout));

        if (WaitOnAddress(addr, &val, sizeof(val), timeout_ms))
        {
            return 0;
        }
        if (GetLastError() == ERROR_TIMEOUT)
        {
            return -ETIMEDOUT;
        }
        return -EINVAL;
    }

    void futex_wake_all(std::atomic<uint64_t>& word)
    {
        WakeByAddressAll(reinterpret_cast<void*>(&word));
    }
}
