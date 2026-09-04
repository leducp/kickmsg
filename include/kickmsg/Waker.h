#ifndef KICKMSG_WAKER_H
#define KICKMSG_WAKER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "kickmsg/os/Time.h"

namespace kickmsg
{
    /// A pollable cross-process wake: the subscriber gets a descriptor for its own
    /// poll/epoll/kqueue loop, and the publisher makes it readable.
    ///
    /// The caller injects the SAME backend into both ends -- Subscriber::wait_fd() or
    /// Waker on one, the Publisher constructor on the other. A backend carries its own
    /// address; nothing about it goes through shared memory. Wiring only one end is a
    /// caller error that shows up as a subscriber waiting out its own deadline.
    ///
    /// An implementation must:
    ///  - outlive every Publisher, Waker and Subscriber given it -- Waker calls close()
    ///    from its destructor;
    ///  - tolerate concurrent signal(), which any number of Publishers may share;
    ///    open()/close()/drain() are called only by the Waker's own thread;
    ///  - return from open() a descriptor the Waker closes exactly once, and on Windows
    ///    a SOCKET that round-trips through int, since it is polled as one.
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
    /// This costs the publisher roughly 1-1.5 us per publish above the futex path at any
    /// fan-out -- one sendto is one syscall, not one unit of work, since the kernel still
    /// copies to each joined socket. Use it where a descriptor has to compose with an
    /// event loop; on a single channel with nothing else to watch, receive() is faster.
    ///
    /// Give each channel its own instance. The port is what separates them: a socket
    /// bound to INADDR_ANY receives every datagram on its port, including groups it never
    /// joined, because membership decides whether the *host* accepts the packet, not
    /// which socket gets it. The group is what fans a wake out to all subscribers.
    ///
    /// **Availability hint, not an authenticated channel.** Any local process can join
    /// the group and sweep the 512-port range, keeping descriptors readable and driving
    /// arm/poll/drain loops. It cannot inject payloads or read shared memory -- the data
    /// path is the region, and a spurious wake costs one re-peek. Where local users are
    /// mutually untrusted, supply a permissioned backend instead.
    class UdpMulticastBackend final : public WakeBackend
    {
    public:
        /// First port derived into, and how many may be used.
        static constexpr uint16_t DEFAULT_PORT_BASE = 27182;
        static constexpr uint16_t PORT_SPAN         = 512;

        /// Derives a group in 239.255.0.0/16 and a port in [port_base, +PORT_SPAN) from
        /// `name`, so both ends agree without coordinating. Two names colliding onto one
        /// port wake each other spuriously but stay correct.
        explicit UdpMulticastBackend(char const* name,
                                     uint16_t    port_base = DEFAULT_PORT_BASE);

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
        uint32_t         group_;
        uint16_t         port_;
        /// Opened on the first wake sent, so an unused backend holds no socket.
        std::atomic<int> sender_{-1};
    };

    /// One descriptor, shared by the Subscribers attached to it, so a wait set holds one
    /// entry however many share it.
    ///
    /// A Waker belongs to the thread that waits on it: across threads, one thread's read
    /// consumes another's wake.
    class Waker
    {
    public:
        explicit Waker(WakeBackend& backend);
        ~Waker();

        Waker(Waker const&)            = delete;
        Waker& operator=(Waker const&) = delete;

        /// Opt in to a caller's wait set without handing out a descriptor: found by ADL,
        /// so a generic waiter calls `wait_descriptor(waker)` and never names kickmsg.
        friend int wait_descriptor(Waker const& waker) { return waker.fd_; }

        /// Descriptor to poll, -1 when the backend could not open one.
        int  fd() const { return fd_; }
        bool valid() const { return fd_ >= 0; }

        /// Consume pending wakes. The owner of a shared Waker calls this once per wait,
        /// after disarming: disarm_wait() will not drain a Waker it does not own.
        void drain();

        WakeBackend&       backend()       { return *backend_; }
        WakeBackend const& backend() const { return *backend_; }

    private:
        WakeBackend* backend_;
        int          fd_{-1};
    };
}

#endif
