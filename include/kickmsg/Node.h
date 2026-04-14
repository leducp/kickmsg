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

        // Explicit non-copyable / move-only.  Node already holds SharedRegion
        // values (move-only), so it's non-copyable de facto; declaring it
        // explicitly short-circuits SFINAE probes from binding frameworks
        // that would otherwise eagerly instantiate the internal container's
        // copy machinery to check copyability.
        Node(Node const&)            = delete;
        Node& operator=(Node const&) = delete;
        Node(Node&&) noexcept        = default;
        Node& operator=(Node&&) noexcept = default;

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
        // These variants take `cfg` by reference with NO default: either
        // side of the relaxed-order protocol may end up being the creator,
        // so both sides must agree on the channel geometry the creator
        // will stamp into the header (create_or_open's config_hash check
        // enforces that agreement on the open branch).
        //
        // Idempotency: calling advertise_or_join / subscribe_or_create
        // twice on the same topic from the same Node returns a second
        // independent Publisher/Subscriber handle that wraps the SAME
        // underlying region (emplace_or_reuse dedupes the SharedRegion).
        // The freshly-opened SharedRegion from the second call is
        // discarded.  Two Publisher handles on the same topic from the
        // same Node are NOT designed for concurrent use from separate
        // threads — use one handle per thread instead.
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

        // --- Unlink helpers -----------------------------------------------
        //
        // Thin wrappers that call SharedMemory::unlink() with the same name
        // formatting used by advertise / subscribe / join_broadcast /
        // create_mailbox.  Safe to call whether or not this node currently
        // holds a region for that name — unlink is a filesystem-level
        // operation on the SHM entry, independent of in-process handles.

        void unlink_topic(char const* topic) const;
        void unlink_broadcast(char const* channel) const;
        /// Unlink the mailbox owned by `owner_node` (defaults to this node).
        void unlink_mailbox(char const* tag, char const* owner_node = nullptr) const;

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

        // unordered_map guarantees reference stability for elements
        // (only iterators are invalidated on rehash), so pointers
        // returned here remain valid across subsequent insertions.
        SharedRegion*       find_region(std::string const& shm_name);
        SharedRegion const* find_region(std::string const& shm_name) const;

        std::string name_;
        std::string prefix_;
        // Keyed by SHM name for O(1) lookup.  A telemetry node on a
        // humanoid robot can easily hold 100-300 topics (joints × (meas,
        // target) + cameras + IMUs + force sensors + hands), so O(N)
        // linear search over a vector/deque starts to matter.  The
        // duplication with SharedRegion::name() costs ~30 B per entry —
        // negligible at any scale we care about.  unordered_map also
        // guarantees reference stability for elements (the mmap addresses
        // used by Publisher/Subscriber don't move on rehash).
        std::unordered_map<std::string, SharedRegion> regions_;
    };
}

#endif
