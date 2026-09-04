#include <cstring>
#include <stdexcept>

#include "kickmsg/Hash.h"
#include "kickmsg/Waker.h"

namespace kickmsg
{
    UdpMulticastBackend::UdpMulticastBackend(uint32_t group, uint16_t port)
        : group_{group}
        , port_{port}
    {
        // 224.0.0.0/4: group membership is what fans a wake out, so a unicast address
        // binds and sends but never reaches a second subscriber.
        if ((group & 0xF0000000u) != 0xE0000000u)
        {
            throw std::invalid_argument("UdpMulticastBackend: group is not IPv4 multicast");
        }
        // Port 0 binds an ephemeral port but is a literal sendto destination: the
        // descriptor would look healthy while every wake vanished.
        if (port == 0)
        {
            throw std::invalid_argument("UdpMulticastBackend: port must not be 0");
        }
    }

    UdpMulticastBackend::UdpMulticastBackend(char const* name, uint16_t port_base)
    {
        if (name == nullptr)
        {
            throw std::invalid_argument("UdpMulticastBackend: name must not be null");
        }
        if (port_base == 0 or port_base > UINT16_MAX - PORT_SPAN + 1)
        {
            throw std::invalid_argument(
                "UdpMulticastBackend: port_base leaves no room for PORT_SPAN");
        }
        // 239.255.0.0/16 is administratively scoped: routers never forward it. The port
        // takes the other half of the hash, so the two collisions stay independent.
        uint64_t const h = hash::fnv1a_64(name, std::strlen(name));
        group_ = 0xEFFF0000u | static_cast<uint32_t>(h & 0xFFFFu);
        port_  = static_cast<uint16_t>(port_base + ((h >> 32) % PORT_SPAN));
    }

    Waker::Waker(WakeBackend& backend)
        : backend_{&backend}
        , fd_{backend.open()}
    {
    }

    Waker::~Waker()
    {
        if (fd_ >= 0)
        {
            backend_->close(fd_);
        }
    }

    void Waker::drain()
    {
        if (fd_ >= 0)
        {
            backend_->drain(fd_);
        }
    }
}
