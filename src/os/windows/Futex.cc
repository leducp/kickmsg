#include "kickmsg/os/Futex.h"

#include <cerrno>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kickmsg
{
    // Wait on the low 32 bits of the counter. Supported Windows targets
    // (x86, x64, ARM64) are little-endian.
#if defined(__BYTE_ORDER__)
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
        "WaitOnAddress word aliasing assumes the low half of write_pos at offset 0");
#endif

    int futex_wait(std::atomic<uint64_t>& word, uint64_t expected, nanoseconds timeout)
    {
        auto* addr = reinterpret_cast<void*>(&word);
        auto  val  = static_cast<uint32_t>(expected);

        // Round up fractional milliseconds and clamp below INFINITE.
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

    // WakeByAddressAll wakes only this process. Cross-process receive() can
    // wait until timeout, during which unread messages may overflow the ring.
    // See the Windows limitation in ARCHITECTURE.md.
    void futex_wake_all(std::atomic<uint64_t>& word)
    {
        WakeByAddressAll(reinterpret_cast<void*>(&word));
    }
}
