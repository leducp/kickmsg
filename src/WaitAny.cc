#include <algorithm>
#include <vector>

#include "kickmsg/os/Time.h"
#include "kickmsg/WaitSet.h"
#include "kickmsg/Subscriber.h"

namespace kickmsg
{
    bool wait_any(Subscriber* const* subscribers, std::size_t count, nanoseconds timeout)
    {
        if (count == 0)
        {
            return false;
        }

        // Distinct: draining a shared Waker twice lets one Subscriber swallow another's
        // wake. Built once, so the loop does not churn the heap per iteration.
        std::vector<Waker*> wakers;
        WaitSet             set;
        bool                covered = true;
        for (std::size_t i = 0; i < count; ++i)
        {
            Waker* waker = subscribers[i]->waker();
            if (waker == nullptr or not waker->valid())
            {
                // Nothing can wake the set for this one; the loop re-peeks instead.
                covered = false;
                continue;
            }
            if (std::find(wakers.begin(), wakers.end(), waker) == wakers.end())
            {
                wakers.push_back(waker);
                set.add(*waker);
            }
        }

        nanoseconds start = kickmsg::monotonic_ns();
        while (true)
        {
            // The arming pass is also the classifying one. A separate scan could see
            // Parked where arm_wait then sees Poll, leaving that Subscriber neither armed
            // nor capped -- a commit fires no wake, so the wait would run to the deadline.
            bool ready  = false;
            bool capped = false;
            for (std::size_t i = 0; i < count; ++i)
            {
                switch (subscribers[i]->arm_wait())
                {
                    case Subscriber::Wait::Ready:  ready  = true; break;
                    // Head claimed but uncommitted: the commit itself fires no wake.
                    case Subscriber::Wait::Poll:   capped = true; break;
                    case Subscriber::Wait::Parked:               break;
                }
            }

            // Readiness before the deadline: a zero timeout must still report a sample.
            nanoseconds budget{0};
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
                        budget = std::min(budget, Subscriber::poll_budget());
                    }
                }
            }

            if (budget > nanoseconds{0})
            {
                if (set.empty())
                {
                    // Nothing to poll: re-peek on poll_budget() rather than sleeping
                    // out the caller's timeout.
                    kickmsg::sleep(budget);
                }
                else
                {
                    set.wait(budget);
                }
            }

            for (std::size_t i = 0; i < count; ++i)
            {
                subscribers[i]->disarm_wait(false);
            }
            for (Waker* waker : wakers)
            {
                waker->drain();
            }
            if (ready)
            {
                return true;
            }
            if (budget == nanoseconds{0})
            {
                return false;
            }
        }
    }
}
