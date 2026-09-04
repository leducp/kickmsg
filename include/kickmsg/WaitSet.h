#ifndef KICKMSG_WAIT_SET_H
#define KICKMSG_WAIT_SET_H

#include <concepts>
#include <cstddef>
#include <vector>

#include "kickmsg/os/Time.h"

namespace kickmsg
{
    /// A source that can be waited on. A type opts in by providing a `wait_descriptor`
    /// findable by ADL -- a hidden friend, typically, which lets the descriptor itself
    /// stay private. Nothing derives from anything: a Subscriber, a socket from another
    /// library and a caller's own type satisfy this independently, in their own headers,
    /// none of which need to include this one.
    template <typename T>
    concept Waitable = requires(T const& t)
    {
        { wait_descriptor(t) } -> std::convertible_to<int>;
    };

    /// Sources to block on together, so one loop serves several without polling each in
    /// turn or dedicating a thread to any.
    ///
    /// A caller with its own event loop does not need this -- put the descriptor in that
    /// loop instead.
    class WaitSet
    {
    public:
        /// Add whatever `source` blocks on. Silently ignores a source with nothing to
        /// offer, so a caller composing a set need not know which of its sources those
        /// are; a duplicate is ignored too, since polling one twice would let one reader
        /// consume another's wake.
        void add(Waitable auto const& source) { add_native(wait_descriptor(source)); }

        /// For a foreign source that hands out a bare descriptor and cannot be taught to
        /// opt in. Prefer add(): a descriptor crossing an interface is worth being able
        /// to grep for.
        void add_native(int fd);

        void        clear() noexcept { fds_.clear(); }
        std::size_t size() const noexcept { return fds_.size(); }
        bool        empty() const noexcept { return fds_.empty(); }

        /// Block until one of the sources is readable or `timeout` elapses. True when at
        /// least one is. An empty set does not wait: it has nothing to wait for, and
        /// sleeping here would hide that from a caller that meant to pace itself.
        bool wait(nanoseconds timeout) const;

    private:
        std::vector<int> fds_;
    };
}

#endif
