#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kickmsg/os/Time.h"
#include "kickmsg/Waker.h"
#include "kickmsg/WaitSet.h"

namespace kickmsg
{
    namespace
    {
        /// Datagrams one drain() call consumes before giving up for this round.
        constexpr int DRAIN_MAX = 256;

        /// Descriptors gathered on the stack before WaitSet::wait allocates.
        constexpr std::size_t STACK_FDS = 64;

        /// Every option is load-bearing -- non-blocking keeps signal() off the publish
        /// path, TTL 0 plus the loopback interface keep the wake on this host, LOOP is
        /// what still delivers it -- so a socket missing any of them is discarded.
        bool configure_sender(int fd)
        {
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0 or ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            {
                return false;
            }
            unsigned char ttl  = 0;
            unsigned char loop = 1;
            if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0
                or ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) != 0)
            {
                return false;
            }
            in_addr iface{};
            iface.s_addr = htonl(INADDR_LOOPBACK);
            if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) != 0)
            {
                return false;
            }
            // Best effort: an fd surviving exec is untidy, not a broken guarantee.
            (void) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            return true;
        }
    }

    UdpMulticastBackend::~UdpMulticastBackend()
    {
        int fd = sender_.load(std::memory_order_acquire);
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

    int UdpMulticastBackend::open()
    {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            return -1;
        }
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 or ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            ::close(fd);
            return -1;
        }
        (void) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

        // Every subscriber of this channel binds the same port, which SO_REUSEADDR is
        // what permits; without it this socket blocks every later joiner.
        int on = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0)
        {
            ::close(fd);
            return -1;
        }
#ifdef SO_REUSEPORT
        // Best effort: SO_REUSEADDR already covers multicast rebinding.
        (void) ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(port_);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return -1;
        }

        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = htonl(group_);
        mreq.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
        if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    void UdpMulticastBackend::close(int fd)
    {
        ::close(fd);
    }

    void UdpMulticastBackend::drain(int fd)
    {
        // Bounded: the group is joinable by any local process, which could otherwise
        // feed this loop indefinitely. What is left reads as a spurious wake.
        uint8_t buffer[64];
        for (int i = 0; i < DRAIN_MAX; ++i)
        {
            ssize_t rc = ::recv(fd, buffer, sizeof(buffer), 0);
            if (rc < 0 and errno == EINTR)
            {
                // A signal did not consume a datagram, so it must not consume budget.
                --i;
                continue;
            }
            if (rc < 0)
            {
                return;
            }
        }
    }

    void UdpMulticastBackend::signal()
    {
        // Racing openers keep one socket and close the other: no lock on the publish path.
        int fd = sender_.load(std::memory_order_acquire);
        if (fd < 0)
        {
            int fresh = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fresh >= 0 and not configure_sender(fresh))
            {
                ::close(fresh);
                fresh = -1;
            }
            if (fresh < 0)
            {
                return;
            }
            int expected = -1;
            if (sender_.compare_exchange_strong(expected, fresh,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                fd = fresh;
            }
            else
            {
                ::close(fresh);
                fd = expected;
            }
        }

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(group_);
        addr.sin_port        = htons(port_);
        uint8_t byte = 1;
        while (::sendto(fd, &byte, sizeof(byte), 0,
                        reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
               and errno == EINTR)
        {
        }
    }

    void WaitSet::add_native(int fd)
    {
        if (fd < 0)
        {
            return;
        }
        if (std::find(fds_.begin(), fds_.end(), fd) == fds_.end())
        {
            fds_.push_back(fd);
        }
    }

    bool WaitSet::wait(nanoseconds timeout) const
    {
        if (fds_.empty())
        {
            return false;
        }
        auto const count = fds_.size();

        // poll() takes any count; only the stack buffer is bounded.
        pollfd              stack[STACK_FDS] = {};
        std::vector<pollfd> heap;
        pollfd*             entries = stack;
        if (count > STACK_FDS)
        {
            heap.resize(count);
            entries = heap.data();
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            entries[i].fd      = fds_[i];
            entries[i].events  = POLLIN;
            entries[i].revents = 0;
        }

#if defined(__linux__)
        timespec ts = to_timespec(timeout);
        return ::ppoll(entries, count, &ts, nullptr) > 0;
#else
        // No ppoll on darwin, and not pselect: fd_set is indexed by descriptor value
        // and cannot hold one past FD_SETSIZE.
        return ::poll(entries, count, to_poll_ms(timeout)) > 0;
#endif
    }
}
