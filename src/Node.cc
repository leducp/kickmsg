#include "kickmsg/Node.h"

#include <cstdio>

#include "kickmsg/Naming.h"

namespace kickmsg
{
    Node::Node(std::string const& name, std::string const& kmsg_namespace)
        : name_{sanitize_shm_component(name, "node")}
        , namespace_{sanitize_shm_component(kmsg_namespace, "namespace")}
    {
    }

    Node::~Node()
    {
        if (registry_.has_value())
        {
            for (auto const& [_, rs] : registry_slots_)
            {
                registry_->deregister(rs.slot_index);
            }
        }
        registry_slots_.clear();
    }

    Registry& Node::lazy_registry()
    {
        if (not registry_.has_value())
        {
            registry_.emplace(Registry::open_or_create(namespace_));
        }
        return *registry_;
    }

    namespace
    {
        /// Ensure the logical name starts with '/' for ROS-style display.
        std::string with_leading_slash(std::string s)
        {
            if (s.empty() or s.front() != '/')
            {
                s.insert(s.begin(), '/');
            }
            return s;
        }

        /// Mailbox logical path: owner is part of the identity, so callers
        /// (both sender and recipient) see the same "/owner/tag" topic.
        std::string mailbox_topic(char const* owner, char const* tag)
        {
            std::string out = "/";
            out += owner;
            out += '/';
            out += tag;
            return out;
        }
    }

    void Node::touch_registry(std::string const& shm_name,
                              std::string const& topic_name,
                              channel::Type      channel_type,
                              registry::Kind     kind,
                              registry::Role     role)
    {
        if (registry_disabled_)
        {
            return;
        }
        auto warn_full = [&]()
        {
            if (registry_full_warned_)
            {
                return;
            }
            std::fprintf(stderr,
                "kickmsg: registry for namespace '%s' is full; "
                "participant '%s' on '%s' will not appear in discovery "
                "(further registry-full events suppressed on this Node)\n",
                namespace_.c_str(), name_.c_str(), shm_name.c_str());
            registry_full_warned_ = true;
        };
        try
        {
            auto& reg = lazy_registry();
            auto it = registry_slots_.find(shm_name);
            if (it != registry_slots_.end())
            {
                if (it->second.role != role and it->second.role != registry::Both)
                {
                    // Upgrade to Both via dereg + re-register; brief
                    // visibility gap during the swap is acceptable since
                    // the registry is diagnostic-only.  On fill-failure
                    // of the Both re-register, fall back to re-registering
                    // the original role to keep at least partial discovery.
                    reg.deregister(it->second.slot_index);
                    uint32_t slot = reg.register_participant(
                        shm_name, topic_name, channel_type, kind,
                        registry::Both, name_);
                    if (slot == INVALID_SLOT)
                    {
                        registry::Role prior = it->second.role;
                        uint32_t       fallback = reg.register_participant(
                            shm_name, topic_name, channel_type, kind,
                            prior, name_);
                        if (fallback == INVALID_SLOT)
                        {
                            warn_full();
                            registry_slots_.erase(it);
                            return;
                        }
                        it->second = RegistrySlot{fallback, prior};
                        return;
                    }
                    it->second = RegistrySlot{slot, registry::Both};
                }
                return;
            }

            uint32_t slot = reg.register_participant(
                shm_name, topic_name, channel_type, kind, role, name_);
            if (slot == INVALID_SLOT)
            {
                warn_full();
                return;
            }
            registry_slots_[shm_name] = RegistrySlot{slot, role};
        }
        catch (std::exception const& e)
        {
            // Latch to avoid stderr spam on a Node that brings up many topics.
            std::fprintf(stderr,
                "kickmsg: registry unavailable for namespace '%s': %s "
                "(further registry failures will be silent on this Node)\n",
                namespace_.c_str(), e.what());
            registry_disabled_ = true;
        }
    }

    Publisher Node::advertise(char const* topic, channel::Config const& cfg)
    {
        auto shm_name   = make_topic_name(topic);
        auto topic_path = with_leading_slash(topic);
        auto [it, _]  = regions_.emplace(
            shm_name,
            SharedRegion::create(shm_name.c_str(), channel::PubSub, cfg, name_.c_str()));
        touch_registry(shm_name, topic_path, channel::PubSub,
                       registry::Pubsub, registry::Publisher);
        return Publisher(it->second);
    }

    Subscriber Node::subscribe(char const* topic)
    {
        auto shm_name   = make_topic_name(topic);
        auto topic_path = with_leading_slash(topic);
        if (auto* r = find_region(shm_name))
        {
            touch_registry(shm_name, topic_path, channel::PubSub,
                           registry::Pubsub, registry::Subscriber);
            return Subscriber(*r);
        }
        auto [it, _] = regions_.emplace(
            shm_name, SharedRegion::open(shm_name.c_str()));
        touch_registry(shm_name, topic_path, channel::PubSub,
                       registry::Pubsub, registry::Subscriber);
        return Subscriber(it->second);
    }

    Publisher Node::advertise_or_join(char const* topic, channel::Config const& cfg)
    {
        auto shm_name   = make_topic_name(topic);
        auto topic_path = with_leading_slash(topic);
        if (auto* r = find_region(shm_name))
        {
            touch_registry(shm_name, topic_path, channel::PubSub,
                           registry::Pubsub, registry::Publisher);
            return Publisher(*r);
        }
        auto [it, _] = regions_.emplace(
            shm_name,
            SharedRegion::create_or_open(
                shm_name.c_str(), channel::PubSub, cfg, name_.c_str()));
        touch_registry(shm_name, topic_path, channel::PubSub,
                       registry::Pubsub, registry::Publisher);
        return Publisher(it->second);
    }

    Subscriber Node::subscribe_or_create(char const* topic, channel::Config const& cfg)
    {
        auto shm_name   = make_topic_name(topic);
        auto topic_path = with_leading_slash(topic);
        if (auto* r = find_region(shm_name))
        {
            touch_registry(shm_name, topic_path, channel::PubSub,
                           registry::Pubsub, registry::Subscriber);
            return Subscriber(*r);
        }
        auto [it, _] = regions_.emplace(
            shm_name,
            SharedRegion::create_or_open(
                shm_name.c_str(), channel::PubSub, cfg, name_.c_str()));
        touch_registry(shm_name, topic_path, channel::PubSub,
                       registry::Pubsub, registry::Subscriber);
        return Subscriber(it->second);
    }

    BroadcastHandle Node::join_broadcast(char const* channel, channel::Config const& cfg)
    {
        auto shm_name   = make_broadcast_name(channel);
        auto topic_path = with_leading_slash(channel);
        if (auto* r = find_region(shm_name))
        {
            touch_registry(shm_name, topic_path, channel::Broadcast,
                           registry::Broadcast, registry::Both);
            return BroadcastHandle{Publisher{*r}, Subscriber{*r}};
        }
        auto [it, _] = regions_.emplace(
            shm_name,
            SharedRegion::create_or_open(
                shm_name.c_str(), channel::Broadcast, cfg, name_.c_str()));
        touch_registry(shm_name, topic_path, channel::Broadcast,
                       registry::Broadcast, registry::Both);
        return BroadcastHandle{Publisher{it->second}, Subscriber{it->second}};
    }

    Subscriber Node::create_mailbox(char const* tag, channel::Config const& cfg)
    {
        channel::Config mbx_cfg = cfg;
        mbx_cfg.max_subscribers = 1;
        auto shm_name   = make_mailbox_name(name_.c_str(), tag);
        auto topic_path = mailbox_topic(name_.c_str(), tag);
        auto [it, _]  = regions_.emplace(
            shm_name,
            SharedRegion::create(shm_name.c_str(), channel::PubSub, mbx_cfg, name_.c_str()));
        // Mailbox owner is the one who receives — Subscriber role.
        touch_registry(shm_name, topic_path, channel::PubSub,
                       registry::Mailbox, registry::Subscriber);
        return Subscriber(it->second);
    }

    Publisher Node::open_mailbox(char const* owner_node, char const* tag)
    {
        auto shm_name   = make_mailbox_name(owner_node, tag);
        auto topic_path = mailbox_topic(owner_node, tag);
        if (auto* r = find_region(shm_name))
        {
            touch_registry(shm_name, topic_path, channel::PubSub,
                           registry::Mailbox, registry::Publisher);
            return Publisher(*r);
        }
        auto [it, _] = regions_.emplace(
            shm_name, SharedRegion::open(shm_name.c_str()));
        // Mailbox sender is the Publisher side.
        touch_registry(shm_name, topic_path, channel::PubSub,
                       registry::Mailbox, registry::Publisher);
        return Publisher(it->second);
    }

    void Node::unlink_topic(char const* topic) const
    {
        SharedMemory::unlink(make_topic_name(topic));
    }

    void Node::unlink_broadcast(char const* channel) const
    {
        SharedMemory::unlink(make_broadcast_name(channel));
    }

    void Node::unlink_mailbox(char const* tag, char const* owner_node) const
    {
        char const* owner = owner_node;
        if (owner == nullptr)
        {
            owner = name_.c_str();
        }
        SharedMemory::unlink(make_mailbox_name(owner, tag));
    }

    std::optional<SchemaInfo> Node::topic_schema(char const* topic) const
    {
        auto const* region = find_region(make_topic_name(topic));
        if (region == nullptr)
        {
            return std::nullopt;
        }
        return region->schema();
    }

    bool Node::try_claim_topic_schema(char const* topic, SchemaInfo const& info)
    {
        auto* region = find_region(make_topic_name(topic));
        if (region == nullptr)
        {
            return false;
        }
        return region->try_claim_schema(info);
    }

    std::string Node::make_topic_name(char const* topic) const
    {
        // namespace_ is pre-sanitized in the ctor; topic is user-supplied on
        // each call and may be a ROS-style "/a/b/c" path.
        return "/" + namespace_ + "_" + sanitize_shm_component(topic, "topic");
    }

    std::string Node::make_broadcast_name(char const* channel) const
    {
        return "/" + namespace_ + "_broadcast_"
             + sanitize_shm_component(channel, "channel");
    }

    std::string Node::make_mailbox_name(char const* owner, char const* tag) const
    {
        return "/" + namespace_ + "_"
             + sanitize_shm_component(owner, "mailbox owner") + "_mbx_"
             + sanitize_shm_component(tag, "mailbox tag");
    }

    SharedRegion* Node::find_region(std::string const& shm_name)
    {
        auto it = regions_.find(shm_name);
        if (it == regions_.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    SharedRegion const* Node::find_region(std::string const& shm_name) const
    {
        auto it = regions_.find(shm_name);
        if (it == regions_.end())
        {
            return nullptr;
        }
        return &it->second;
    }
}
