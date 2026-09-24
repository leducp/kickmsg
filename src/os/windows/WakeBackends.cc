#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

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

        /// Initialize Winsock once per process, when first used.
        bool winsock_ready()
        {
            static bool const ready = []
            {
                WSADATA data{};
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return ready;
        }

        /// SOCKET is pointer-sized; reject values that cannot fit this API's int.
        int to_fd(SOCKET socket)
        {
            if (socket == INVALID_SOCKET)
            {
                return -1;
            }
            if (socket > static_cast<SOCKET>(INT_MAX))
            {
                ::closesocket(socket);
                return -1;
            }
            return static_cast<int>(socket);
        }

        SOCKET to_socket(int fd)
        {
            return static_cast<SOCKET>(fd);
        }

        /// Non-blocking sockets keep signal() from waiting. TTL 0, loopback
        /// interface, and multicast loopback restrict delivery to this host.
        bool configure_sender(SOCKET socket)
        {
            u_long non_blocking = 1;
            if (::ioctlsocket(socket, FIONBIO, &non_blocking) == SOCKET_ERROR)
            {
                return false;
            }
            DWORD ttl  = 0;
            DWORD loop = 1;
            if (::setsockopt(socket, IPPROTO_IP, IP_MULTICAST_TTL,
                             reinterpret_cast<char const*>(&ttl), sizeof(ttl)) == SOCKET_ERROR
                or ::setsockopt(socket, IPPROTO_IP, IP_MULTICAST_LOOP,
                                reinterpret_cast<char const*>(&loop), sizeof(loop)) == SOCKET_ERROR)
            {
                return false;
            }
            in_addr iface{};
            iface.s_addr = htonl(INADDR_LOOPBACK);
            return ::setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF,
                                reinterpret_cast<char const*>(&iface),
                                sizeof(iface)) != SOCKET_ERROR;
        }
    }

    void UdpMulticastBackend::open_sender()
    {
        if (winsock_ready())
        {
            SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (socket != INVALID_SOCKET)
            {
                if (configure_sender(socket))
                {
                    sender_ = to_fd(socket);
                }
                else
                {
                    ::closesocket(socket);
                }
            }
        }
        if (sender_ < 0)
        {
            throw std::runtime_error("UdpMulticastBackend: cannot open a multicast sender");
        }
    }

    UdpMulticastBackend::~UdpMulticastBackend()
    {
        if (sender_ >= 0)
        {
            ::closesocket(to_socket(sender_));
        }
    }

    int UdpMulticastBackend::open()
    {
        if (not winsock_ready())
        {
            return -1;
        }
        SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket == INVALID_SOCKET)
        {
            return -1;
        }

        u_long non_blocking = 1;
        if (::ioctlsocket(socket, FIONBIO, &non_blocking) == SOCKET_ERROR)
        {
            ::closesocket(socket);
            return -1;
        }

        // SO_REUSEADDR allows all channel subscribers to bind the same port.
        BOOL on = TRUE;
        if (::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<char const*>(&on), sizeof(on)) == SOCKET_ERROR)
        {
            ::closesocket(socket);
            return -1;
        }

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(port_);
        if (::bind(socket, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == SOCKET_ERROR)
        {
            ::closesocket(socket);
            return -1;
        }

        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = htonl(group_);
        mreq.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
        if (::setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         reinterpret_cast<char const*>(&mreq),
                         sizeof(mreq)) == SOCKET_ERROR)
        {
            ::closesocket(socket);
            return -1;
        }
        return to_fd(socket);
    }

    void UdpMulticastBackend::close(int fd)
    {
        ::closesocket(to_socket(fd));
    }

    void UdpMulticastBackend::drain(int fd)
    {
        // Bound draining so incoming traffic cannot keep the caller here forever.
        char buffer[64];
        for (int i = 0; i < DRAIN_MAX; ++i)
        {
            if (::recv(to_socket(fd), buffer, sizeof(buffer), 0) == SOCKET_ERROR)
            {
                return;
            }
        }
    }

    void UdpMulticastBackend::signal()
    {
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(group_);
        addr.sin_port        = htons(port_);
        char byte = 1;
        (void) ::sendto(to_socket(sender_), &byte, sizeof(byte), 0,
                        reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
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
        if (fds_.empty() or not winsock_ready())
        {
            return false;
        }
        auto const count = fds_.size();

        // WSAPoll avoids select's FD_SETSIZE limit; only readability is requested.
        // Reuse per-thread storage beyond the stack buffer.
        WSAPOLLFD  stack[STACK_FDS] = {};
        WSAPOLLFD* entries = stack;
        static thread_local std::vector<WSAPOLLFD> heap;
        if (count > STACK_FDS)
        {
            heap.resize(count);
            entries = heap.data();
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            if (fds_[i] < 0)
            {
                return false;
            }
            entries[i].fd      = to_socket(fds_[i]);
            entries[i].events  = POLLRDNORM;
            entries[i].revents = 0;
        }

        int const rc = ::WSAPoll(entries, static_cast<ULONG>(count), to_poll_ms(timeout));
        if (rc <= 0)
        {
            return false;
        }
        // Only readability counts: poll also returns errors and closed descriptors.
        for (std::size_t i = 0; i < count; ++i)
        {
            if ((entries[i].revents & POLLRDNORM) != 0)
            {
                return true;
            }
        }
        return false;
    }
}
