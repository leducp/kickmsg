#include "kickmsg/os/Futex.h"

#include <cerrno>

// macOS kernel ulock API.
// Used by libc++ (std::atomic::wait) and libdispatch since macOS 10.12.
// WARNING: __ulock_wait / __ulock_wake are private Apple APIs with no public
// header. The ABI has been stable for years and is unlikely to change without
// a libc++ rebuild, but Apple has not formally committed to keeping it.
extern "C"
{
    int __ulock_wait(uint32_t operation, void* addr, uint64_t value,
                     uint32_t timeout_us);
    int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
}

constexpr uint32_t UL_COMPARE_AND_WAIT_SHARED = 0x00000003;
constexpr uint32_t ULF_NO_ERRNO               = 0x01000000;
constexpr uint32_t ULF_WAKE_ALL               = 0x00000100;

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
    "Futex implementation requires little-endian byte order");

namespace kickmsg
{
    int futex_wait(std::atomic<uint64_t>& word, uint64_t expected, nanoseconds timeout)
    {
        auto* addr = reinterpret_cast<uint32_t*>(&word);
        auto  val  = static_cast<uint64_t>(static_cast<uint32_t>(expected));

        // 0 means "wait forever" to __ulock_wait, so a budget that rounds to zero has to
        // become the shortest real wait instead.
        auto us = duration_cast<microseconds>(timeout).count();
        if (us <= 0)
        {
            us = 1;
        }
        // Clamped, not truncated: a longer budget would wrap to a short one and return
        // early. ~71 minutes, and the caller's loop re-arms past that.
        if (us > UINT32_MAX)
        {
            us = UINT32_MAX;
        }
        auto timeout_us = static_cast<uint32_t>(us);

        // ULF_NO_ERRNO makes __ulock_wait return the negative errno directly, which is
        // already this function's convention.
        int rc = __ulock_wait(
            UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO,
            addr, val, timeout_us);

        if (rc >= 0 or rc == -EINTR)
        {
            return 0;
        }
        return rc;
    }

    void futex_wake_all(std::atomic<uint64_t>& word)
    {
        auto* addr = reinterpret_cast<uint32_t*>(&word);
        __ulock_wake(
            UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL,
            addr, 0);
    }
}
