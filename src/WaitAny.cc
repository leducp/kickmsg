#include <algorithm>
#include <vector>

#include "kickmsg/os/Time.h"
#include "kickmsg/WaitSet.h"
#include "kickmsg/Subscriber.h"

namespace kickmsg
{
    bool wait_any(Subscriber* const* subscribers, std::size_t count, nanoseconds timeout,
                  nanoseconds poll_cap)
    {
        if (count == 0)
        {
            return false;
        }

        // Reused across calls: this sits in a receive loop, and every Subscriber must
        // already belong to the calling thread, so per-thread buffers need no lock and
        // stop the heap churn.
        static thread_local std::vector<Waker*> wakers;
        static thread_local WaitSet             set;
        wakers.clear();
        set.clear();

        bool covered = true;
        for (std::size_t i = 0; i < count; ++i)
        {
            Waker* waker = subscribers[i]->waker_;
            if (waker == nullptr or not waker->valid())
            {
                // Nothing can wake the set for this one; the loop re-peeks instead.
                covered = false;
                continue;
            }
            // One dedupe, not two: distinct Wakers own distinct descriptors, so the set
            // never sees a repeat. Draining a shared Waker twice would let one Subscriber
            // swallow another's wake.
            if (std::find(wakers.begin(), wakers.end(), waker) == wakers.end())
            {
                wakers.push_back(waker);
                set.add_native(wait_descriptor(*waker));
            }
        }

        nanoseconds start = kickmsg::monotonic_ns();
        while (true)
        {
            // The arming pass is also the classifying one. A separate scan could see
            // Armed where arm_wait then sees Poll, leaving that Subscriber neither armed
            // nor capped. No wake fires for a commit, so the wait would run to the deadline.
            bool ready  = false;
            bool capped = false;
            for (std::size_t i = 0; i < count; ++i)
            {
                switch (subscribers[i]->arm_wait())
                {
                    case Subscriber::Wait::Ready:  { ready  = true; break; }
                    // Head claimed but uncommitted: the commit itself fires no wake.
                    case Subscriber::Wait::Poll:   { capped = true; break; }
                    case Subscriber::Wait::Armed: { break;                }
                }
            }

            // Readiness before the deadline: a zero timeout must still report a sample.
            nanoseconds budget = 0ns;
            if (not ready)
            {
                // After the pass, not before: over a large set the scan itself takes
                // time, and a stale reading would overshoot the deadline by that much.
                nanoseconds elapsed = kickmsg::elapsed_time(start);
                if (elapsed < timeout)
                {
                    budget = timeout - elapsed;
                    if (capped or not covered)
                    {
                        budget = std::min(budget, poll_cap);
                    }
                }
            }

            if (budget > 0ns)
            {
                if (set.empty())
                {
                    // Nothing to poll: re-check on poll_cap rather than sleeping out the
                    // caller's timeout.
                    kickmsg::sleep(budget);
                }
                else
                {
                    set.wait(budget);
                }
            }

            for (std::size_t i = 0; i < count; ++i)
            {
                subscribers[i]->disarm_wait();
            }
            for (Waker* waker : wakers)
            {
                waker->drain();
            }
            if (ready)
            {
                return true;
            }
            if (budget == 0ns)
            {
                return false;
            }
        }
    }
}
