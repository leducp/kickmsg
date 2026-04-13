#include "kickmsg/Node.h"

namespace kickmsg
{
    Node::Node(std::string const& name, std::string const& prefix)
        : name_{name}
        , prefix_{prefix}
    {
    }

    // Called by the *_or_* variants and by subscribe / open_mailbox to
    // avoid registering two SharedRegion objects for the same SHM: two
    // regions pointing at the same mmap would double-unmap on destruction
    // and silently invalidate each other's mapping.  Strict advertise()
    // doesn't need this because SharedRegion::create() itself rejects a
    // pre-existing SHM entry.
    SharedRegion& Node::emplace_or_reuse(std::string const& shm_name,
                                         SharedRegion&&     region)
    {
        if (auto* existing = find_region(shm_name))
        {
            return *existing;
        }
        regions_.emplace_back(std::move(region));
        return regions_.back();
    }

    Publisher Node::advertise(char const* topic, channel::Config const& cfg)
    {
        auto shm_name = make_topic_name(topic);
        regions_.emplace_back(
            SharedRegion::create(shm_name.c_str(), channel::PubSub, cfg, name_.c_str()));
        return Publisher(regions_.back());
    }

    Subscriber Node::subscribe(char const* topic)
    {
        auto shm_name = make_topic_name(topic);
        auto& region  = emplace_or_reuse(
            shm_name, SharedRegion::open(shm_name.c_str()));
        return Subscriber(region);
    }

    Publisher Node::advertise_or_join(char const* topic, channel::Config const& cfg)
    {
        auto shm_name = make_topic_name(topic);
        auto& region  = emplace_or_reuse(
            shm_name,
            SharedRegion::create_or_open(
                shm_name.c_str(), channel::PubSub, cfg, name_.c_str()));
        return Publisher(region);
    }

    Subscriber Node::subscribe_or_create(char const* topic, channel::Config const& cfg)
    {
        auto shm_name = make_topic_name(topic);
        auto& region  = emplace_or_reuse(
            shm_name,
            SharedRegion::create_or_open(
                shm_name.c_str(), channel::PubSub, cfg, name_.c_str()));
        return Subscriber(region);
    }

    BroadcastHandle Node::join_broadcast(char const* channel, channel::Config const& cfg)
    {
        auto shm_name = make_broadcast_name(channel);
        auto& region  = emplace_or_reuse(
            shm_name,
            SharedRegion::create_or_open(
                shm_name.c_str(), channel::Broadcast, cfg, name_.c_str()));
        return BroadcastHandle{Publisher{region}, Subscriber{region}};
    }

    Subscriber Node::create_mailbox(char const* tag, channel::Config const& cfg)
    {
        channel::Config mbx_cfg = cfg;
        mbx_cfg.max_subscribers = 1;
        auto shm_name = make_mailbox_name(name_.c_str(), tag);
        regions_.emplace_back(
            SharedRegion::create(shm_name.c_str(), channel::PubSub, mbx_cfg, name_.c_str()));
        return Subscriber(regions_.back());
    }

    Publisher Node::open_mailbox(char const* owner_node, char const* tag)
    {
        auto shm_name = make_mailbox_name(owner_node, tag);
        auto& region  = emplace_or_reuse(
            shm_name, SharedRegion::open(shm_name.c_str()));
        return Publisher(region);
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
        return "/" + prefix_ + "_" + topic;
    }

    std::string Node::make_broadcast_name(char const* channel) const
    {
        return "/" + prefix_ + "_broadcast_" + channel;
    }

    std::string Node::make_mailbox_name(char const* owner, char const* tag) const
    {
        return "/" + prefix_ + "_" + owner + "_mbx_" + tag;
    }

    SharedRegion* Node::find_region(std::string const& shm_name)
    {
        for (auto& region : regions_)
        {
            if (region.name() == shm_name)
            {
                return &region;
            }
        }
        return nullptr;
    }

    SharedRegion const* Node::find_region(std::string const& shm_name) const
    {
        for (auto const& region : regions_)
        {
            if (region.name() == shm_name)
            {
                return &region;
            }
        }
        return nullptr;
    }
}
