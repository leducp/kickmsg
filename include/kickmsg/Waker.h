#ifndef KICKMSG_WAKER_H
#define KICKMSG_WAKER_H

#include <cstddef>
#include <cstdint>

#include "kickmsg/os/Time.h"

namespace kickmsg
{
    /// A pollable cross-process wake: the subscriber gets a descriptor for its own
    /// poll/epoll/kqueue loop, and the publisher makes it readable.
    ///
    /// Give the same backend to both ends: a Waker on the subscriber side, the Publisher
    /// constructor on the other. The backend holds its own address, so nothing about it
    /// is stored in shared memory. If only one end gets one, no wake is sent and the
    /// subscriber waits out its timeout.
    ///
    /// An implementation must:
    ///  - outlive every Publisher and Waker it was given to. A Waker closes its
    ///    descriptor in the destructor.
    ///  - belong to one Publisher. A Publisher is not thread safe, so concurrent
    ///    signal() calls never happen. The subscriber side is a different matter:
    ///    open/close/drain run on whatever thread waits, so signal() must tolerate them
    ///    running at the same time.
    ///  - return a descriptor from open() that the Waker closes once. On Windows it must
    ///    be a SOCKET small enough to fit an int, because it is polled as one.
    class WakeBackend
    {
    public:
        virtual ~WakeBackend() = default;

        // ---- subscriber side ----

        /// A descriptor that becomes readable on a wake, or -1 when this backend cannot
        /// open one here. Called once per Waker; several Subscribers may then share it.
        virtual int  open() = 0;
        virtual void close(int fd) = 0;

        /// Consume pending wakes so `fd` is level-clean before the next wait. Bound the
        /// work: a source other processes can reach may be fed faster than it drains.
        virtual void drain(int fd) = 0;

        // ---- publisher side ----

        /// Make every waiting descriptor readable. Called on the publish hot path, so it
        /// must not block; a full buffer means a wake is already pending and is ignored.
        virtual void signal() = 0;
    };

    /// Loopback UDP multicast: one datagram wakes every subscriber of a channel.
    ///
    /// Costs the publisher about 1 to 1.5 us more per publish than the futex path, at any
    /// number of subscribers. One sendto is one syscall, but the kernel still copies the
    /// datagram to every joined socket. Use it when the descriptor has to sit in an event
    /// loop next to other sources. For a single channel, receive() is faster.
    ///
    /// Give each channel its own instance. The port is what keeps channels apart: a
    /// socket bound to INADDR_ANY gets every datagram on its port, even for groups it
    /// never joined, because the membership only decides whether the host accepts the
    /// packet. The group is what delivers one wake to every subscriber.
    ///
    /// This is a hint that something arrived, not an authenticated channel. Any local
    /// process can join the group and send to it, which keeps descriptors readable and
    /// spins the wait loop. It cannot forge samples or read shared memory: the data lives
    /// in the region, and a false wake only costs one extra check. On a host with
    /// untrusted users, write your own backend instead.
    class UdpMulticastBackend final : public WakeBackend
    {
    public:
        /// First port derived into, and how many may be used.
        static constexpr uint16_t DEFAULT_PORT_BASE = 27182;
        static constexpr uint16_t PORT_SPAN         = 512;

        /// Derives a group in 239.255.0.0/16 and a port in [port_base, +PORT_SPAN) from
        /// `name`, so both ends agree without coordinating. Two names colliding onto one
        /// port wake each other spuriously but stay correct.
        UdpMulticastBackend(char const* name, uint16_t port_base = DEFAULT_PORT_BASE);

        /// Exact group and port, for a caller that would rather pin them than derive them.
        UdpMulticastBackend(uint32_t group, uint16_t port);

        ~UdpMulticastBackend() override;

        UdpMulticastBackend(UdpMulticastBackend const&)            = delete;
        UdpMulticastBackend& operator=(UdpMulticastBackend const&) = delete;

        int  open() override;
        void close(int fd) override;
        void drain(int fd) override;
        void signal() override;

        uint32_t group() const { return group_; }
        uint16_t port() const { return port_; }

    private:
        /// Throws std::runtime_error when the host cannot carry multicast.
        void open_sender();

        uint32_t group_;
        uint16_t port_;
        int      sender_{-1};
    };

    /// One descriptor, shared by the Subscribers attached to it, so a wait set holds one
    /// entry however many share it.
    ///
    /// A Waker belongs to the thread that waits on it: across threads, one thread's read
    /// consumes another's wake.
    class Waker
    {
    public:
        Waker(WakeBackend& backend);
        ~Waker();

        Waker(Waker const&)            = delete;
        Waker& operator=(Waker const&) = delete;

        /// Opt in to a caller's wait set without handing out a descriptor: found by ADL,
        /// so a generic waiter calls `wait_descriptor(waker)` and never names kickmsg.
        friend int wait_descriptor(Waker const& waker) { return waker.fd_; }

        bool valid() const { return fd_ >= 0; }

        /// Consume pending wakes. The owner of a shared Waker calls this once per wait,
        /// after disarming: disarm_wait() will not drain a Waker it does not own.
        void drain();

    private:
        WakeBackend* backend_;
        int          fd_{-1};
    };
}

#endif
