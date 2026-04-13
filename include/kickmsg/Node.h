#ifndef KICKMSG_NODE_H
#define KICKMSG_NODE_H

#include <optional>
#include <string>
#include <unordered_map>

#include "kickmsg/Region.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

namespace kickmsg
{
    struct BroadcastHandle
    {
        Publisher  pub;
        Subscriber sub;
    };

    /// High-level messaging node: manages shared-memory regions and provides
    /// Publisher/Subscriber handles for topic-centric communication.
    ///
    /// Lifetime: the Node owns the underlying shared-memory mappings. All
    /// Publisher, Subscriber, and BroadcastHandle objects returned by this
    /// Node hold raw pointers into the mapped memory. They MUST NOT outlive
    /// the Node that created them — destroying the Node unmaps the memory
    /// and silently invalidates all outstanding handles.
    class Node
    {
    public:
        Node(std::string const& name, std::string const& prefix = "kickmsg");

        // --- PubSub (topic-centric, 1-to-N by convention) ---
        //
        // Strict variants assume a fixed startup order:
        //   advertise() creates the region (publisher must arrive first),
        //   subscribe() opens an existing one (subscriber arrives later).
        //
        // The *_or_* variants relax that: either side may be the first to
        // materialize the region, mirroring join_broadcast()'s behavior.
        // Useful when startup order is unknown — e.g. a listener service
        // starting before its data source.
        //
        // NOTE on cfg.schema: only the *creator* of the region bakes
        // cfg.schema into the header.  On the open branch (region already
        // exists) cfg.schema is IGNORED and the existing region's schema
        // is preserved — schema is orthogonal to channel geometry and
        // never part of the create_or_open config-mismatch check.  Use
        // try_claim_topic_schema() afterwards to publish a descriptor
        // regardless of which side ended up creating the region.
        Publisher  advertise(char const* topic, channel::Config const& cfg = {});
        Subscriber subscribe(char const* topic);

        Publisher  advertise_or_join(char const* topic, channel::Config const& cfg);
        Subscriber subscribe_or_create(char const* topic, channel::Config const& cfg);

        // --- Broadcast (N-to-N shared channel) ---
        BroadcastHandle join_broadcast(char const* channel, channel::Config const& cfg = {});

        // --- Mailbox (N-to-1, max_subscribers=1) ---
        Subscriber create_mailbox(char const* tag, channel::Config const& cfg = {});
        Publisher  open_mailbox(char const* owner_node, char const* tag);

        // --- Optional payload schema descriptor ---
        //
        // The library never interprets schema bytes — these accessors just
        // forward to the SharedRegion backing the topic. Mismatch policy
        // is entirely the caller's.

        /// Read the schema descriptor of a topic this node has joined.
        /// Returns nullopt if no schema is published or this node hasn't
        /// joined the topic.
        std::optional<SchemaInfo> topic_schema(char const* topic) const;

        /// Atomically claim the schema slot on a topic this node has joined.
        /// Returns true if this call published the schema, false if someone
        /// else got there first (read back with topic_schema()).
        bool try_claim_topic_schema(char const* topic, SchemaInfo const& info);

        std::string const& name()   const { return name_; }
        std::string const& prefix() const { return prefix_; }

    private:
        std::string make_topic_name(char const* topic) const;
        std::string make_broadcast_name(char const* channel) const;
        std::string make_mailbox_name(char const* owner, char const* tag) const;

        SharedRegion*       find_region(std::string const& shm_name);
        SharedRegion const* find_region(std::string const& shm_name) const;

        std::string name_;
        std::string prefix_;
        // Keyed by SHM name so we can look up by topic. unordered_map
        // gives us reference stability for elements (mmap addresses are
        // stable regardless, but this keeps lookups O(1)).
        std::unordered_map<std::string, SharedRegion> regions_;
    };
}

#endif
