#include <algorithm>
#include <climits>
#include <cstring>
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

        /// Winsock needs a process-wide init before any socket call. Function-local static,
        /// so it happens once and only if a wake backend is actually used.
        bool winsock_ready()
        {
            static bool const ready = []
            {
                WSADATA data{};
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return ready;
        }

        /// SOCKET is a UINT_PTR. Windows keeps handle values small for interop, but that
        /// is convention, not contract: a value past INT_MAX would wrap negative and read
        /// back as a failed bind, so it fails closed instead.
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

        /// Every option is load-bearing -- non-blocking keeps signal() off the publish
        /// path, TTL 0 plus the loopback interface keep the wake on this host, LOOP is
        /// what still delivers it -- so a socket missing any of them is discarded.
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

    UdpMulticastBackend::~UdpMulticastBackend()
    {
        int fd = sender_.load(std::memory_order_acquire);
        if (fd >= 0)
        {
            ::closesocket(to_socket(fd));
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

        // Every subscriber of this channel binds the same port; the group membership is
        // what fans one wake out to all of them.
        // Every subscriber of this channel binds the same port, which SO_REUSEADDR is
        // what permits; without it this socket blocks every later joiner.
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
        // Bounded, as on POSIX: the group is joinable by any local process, which could
        // otherwise feed this loop indefinitely. What is left reads as a spurious wake.
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
        // Racing openers keep one socket and close the other: no lock on the publish path.
        int fd = sender_.load(std::memory_order_acquire);
        if (fd < 0)
        {
            if (not winsock_ready())
            {
                return;
            }
            SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (socket == INVALID_SOCKET)
            {
                return;
            }
            if (not configure_sender(socket))
            {
                ::closesocket(socket);
                return;
            }
            int fresh = to_fd(socket);
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
                ::closesocket(to_socket(fresh));
                fd = expected;
            }
        }

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(group_);
        addr.sin_port        = htons(port_);
        char byte = 1;
        (void) ::sendto(to_socket(fd), &byte, sizeof(byte), 0,
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

        // WSAPoll, not select: fd_set is a fixed array of FD_SETSIZE sockets, which
        // would cap this API at a compile-time constant. WSAPoll's documented defect is
        // POLLOUT on a failed connect; this only ever asks to read.
        WSAPOLLFD              stack[STACK_FDS] = {};
        std::vector<WSAPOLLFD> heap;
        WSAPOLLFD*             entries = stack;
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

        return ::WSAPoll(entries, static_cast<ULONG>(count), to_poll_ms(timeout)) > 0;
    }
}
