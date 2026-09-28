#include <stdexcept>
#include <string>
#include <string_view>

#include "kickmsg/Hash.h"
#include "kickmsg/Node.h"
#include "kickmsg/Waker.h"

namespace kickmsg
{
    namespace
    {
        char const* require_name(char const* name, char const* what)
        {
            if (name == nullptr)
            {
                throw std::invalid_argument(std::string{"UdpMulticastBackend: "} + what + " must not be null");
            }
            return name;
        }
    }

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
        open_sender();
    }

    UdpMulticastBackend::UdpMulticastBackend(char const* name, uint16_t port_base)
        : UdpMulticastBackend(derive(hash::fnv1a_64(std::string_view{require_name(name, "name")}), port_base))
    {
    }

    UdpMulticastBackend UdpMulticastBackend::for_topic(Node const& node, char const* topic, uint16_t port_base)
    {
        return UdpMulticastBackend(derive(node.topic_identity(require_name(topic, "topic")), port_base));
    }

    UdpMulticastBackend UdpMulticastBackend::for_broadcast(Node const& node, char const* channel, uint16_t port_base)
    {
        return UdpMulticastBackend(derive(node.broadcast_identity(require_name(channel, "channel")), port_base));
    }

    UdpMulticastBackend UdpMulticastBackend::for_mailbox(Node const& node, char const* tag, char const* owner_node, uint16_t port_base)
    {
        char const* owner = owner_node;
        if (owner == nullptr)
        {
            owner = node.name().c_str();
        }
        return UdpMulticastBackend(derive(node.mailbox_identity(owner, require_name(tag, "tag")), port_base));
    }

    UdpMulticastBackend::UdpMulticastBackend(Address address)
        : group_{address.group}
        , port_{address.port}
    {
        open_sender();
    }

    UdpMulticastBackend::Address UdpMulticastBackend::derive(uint64_t hash, uint16_t port_base)
    {
        if (port_base == 0 or port_base > UINT16_MAX - PORT_SPAN + 1)
        {
            throw std::invalid_argument(
                "UdpMulticastBackend: port_base leaves no room for PORT_SPAN");
        }
        // 239.255.0.0/16 is administratively scoped: routers never forward it. The port
        // takes the other half of the hash, so the two collisions stay independent.
        Address address;
        address.group = 0xEFFF0000u | static_cast<uint32_t>(hash & 0xFFFFu);
        address.port  = static_cast<uint16_t>(port_base + ((hash >> 32) % PORT_SPAN));
        return address;
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
