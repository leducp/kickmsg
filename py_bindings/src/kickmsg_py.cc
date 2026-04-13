/// @file kickmsg_py.cc
/// @brief Python bindings for Kickmsg (nanobind-based).
///
/// Layout:
///   kickmsg                  — module
///     ChannelType            — enum
///     Config                 — channel::Config
///     SchemaInfo             — payload schema descriptor
///     HealthReport           — SharedRegion::diagnose() result
///     SharedRegion           — factory methods + schema/health/repair
///     Publisher              — send(bytes) and allocate()/publish() zero-copy
///     Subscriber             — try_receive / receive (GIL release) / *_view
///     SampleRef              — copy-based sample (bytes)
///     SampleView             — zero-copy sample (buffer protocol)
///     BroadcastHandle        — NamedTuple-like (pub, sub)
///     Node                   — high-level topic / broadcast / mailbox
///     schema (submodule)
///       Diff                 — enum (bitmask)
///       diff(a, b)           — pure diff function
///     hash (submodule)
///       fnv1a_64(data[, seed])
///       identity_from_fnv1a(descriptor)
///
/// Zero-copy contract:
///   - Publisher.allocate(len) returns a writable memoryview that points
///     directly into a shared-memory slot.  The user fills it (memcpy,
///     numpy.copyto, cv2.imencode, etc.), then calls Publisher.publish().
///     The memoryview is valid only between allocate() and publish().
///   - SampleView supports the buffer protocol as a read-only memoryview
///     pointing into the same shared-memory slot.  The slot stays pinned
///     while the SampleView is alive; release the pin by deleting the
///     SampleView or exiting its `with` block.

#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

#include "kickmsg/Node.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Region.h"
#include "kickmsg/Subscriber.h"
#include "kickmsg/Hash.h"
#include "kickmsg/types.h"

namespace nb = nanobind;
using namespace nb::literals;

namespace
{
    // Helper: build a read-only memoryview over raw bytes (no copy).
    nb::object memview_readonly(void const* ptr, std::size_t len)
    {
        PyObject* obj = PyMemoryView_FromMemory(
            reinterpret_cast<char*>(const_cast<void*>(ptr)),
            static_cast<Py_ssize_t>(len), PyBUF_READ);
        if (obj == nullptr)
        {
            throw nb::python_error();
        }
        return nb::steal(obj);
    }

    // Helper: build a writable memoryview over raw bytes (no copy).
    nb::object memview_writable(void* ptr, std::size_t len)
    {
        PyObject* obj = PyMemoryView_FromMemory(
            reinterpret_cast<char*>(ptr),
            static_cast<Py_ssize_t>(len), PyBUF_WRITE);
        if (obj == nullptr)
        {
            throw nb::python_error();
        }
        return nb::steal(obj);
    }

    // Convert SchemaInfo.name (fixed-size NUL-terminated char array) to string.
    std::string schema_name_str(kickmsg::SchemaInfo const& s)
    {
        std::size_t n = ::strnlen(s.name, sizeof(s.name));
        return std::string{s.name, n};
    }

    void set_schema_name(kickmsg::SchemaInfo& s, std::string const& name)
    {
        std::size_t n = std::min(name.size(), sizeof(s.name) - 1);
        std::memset(s.name, 0, sizeof(s.name));
        std::memcpy(s.name, name.data(), n);
    }
}

namespace kickmsg
{
    NB_MODULE(kickmsg, m)
    {
        m.doc() = "Kickmsg — lock-free shared-memory IPC";

        // -------------------------------------------------------------------
        // Enums & simple types
        // -------------------------------------------------------------------

        nb::enum_<channel::Type>(m, "ChannelType")
            .value("PubSub",    channel::PubSub)
            .value("Broadcast", channel::Broadcast);

        nb::class_<channel::Config>(m, "Config")
            .def(nb::init<>())
            .def_rw("max_subscribers",   &channel::Config::max_subscribers)
            .def_rw("sub_ring_capacity", &channel::Config::sub_ring_capacity)
            .def_rw("pool_size",         &channel::Config::pool_size)
            .def_rw("max_payload_size",  &channel::Config::max_payload_size)
            .def_prop_rw("commit_timeout_us",
                [](channel::Config const& c) -> int64_t
                { return c.commit_timeout.count(); },
                [](channel::Config& c, int64_t us)
                { c.commit_timeout = microseconds{us}; },
                "Commit timeout in microseconds.")
            .def_rw("schema", &channel::Config::schema);

        // -------------------------------------------------------------------
        // SchemaInfo + schema submodule (Diff / diff)
        // -------------------------------------------------------------------

        nb::class_<SchemaInfo>(m, "SchemaInfo")
            .def(nb::init<>())
            .def_prop_rw("identity",
                [](SchemaInfo const& s) -> nb::bytes
                { return nb::bytes(reinterpret_cast<char const*>(s.identity.data()),
                                   s.identity.size()); },
                [](SchemaInfo& s, nb::bytes const& b)
                {
                    if (b.size() != s.identity.size())
                    {
                        throw std::runtime_error(
                            "SchemaInfo.identity must be exactly 64 bytes");
                    }
                    std::memcpy(s.identity.data(), b.c_str(), s.identity.size());
                })
            .def_prop_rw("layout",
                [](SchemaInfo const& s) -> nb::bytes
                { return nb::bytes(reinterpret_cast<char const*>(s.layout.data()),
                                   s.layout.size()); },
                [](SchemaInfo& s, nb::bytes const& b)
                {
                    if (b.size() != s.layout.size())
                    {
                        throw std::runtime_error(
                            "SchemaInfo.layout must be exactly 64 bytes");
                    }
                    std::memcpy(s.layout.data(), b.c_str(), s.layout.size());
                })
            .def_prop_rw("name",
                [](SchemaInfo const& s) -> std::string { return schema_name_str(s); },
                [](SchemaInfo& s, std::string const& n) { set_schema_name(s, n); })
            .def_rw("version",       &SchemaInfo::version)
            .def_rw("identity_algo", &SchemaInfo::identity_algo)
            .def_rw("layout_algo",   &SchemaInfo::layout_algo)
            .def_rw("flags",         &SchemaInfo::flags)
            .def("__repr__", [](SchemaInfo const& s)
            {
                return "SchemaInfo(name='" + schema_name_str(s) +
                       "', version=" + std::to_string(s.version) + ")";
            });

        auto schema_mod = m.def_submodule("schema", "Schema diff helpers");
        nb::enum_<schema::Diff>(schema_mod, "Diff", nb::is_arithmetic())
            .value("Equal",        schema::Equal)
            .value("Identity",     schema::Identity)
            .value("Layout",       schema::Layout)
            .value("Version",      schema::Version)
            .value("Name",         schema::Name)
            .value("IdentityAlgo", schema::IdentityAlgo)
            .value("LayoutAlgo",   schema::LayoutAlgo);
        schema_mod.def("diff", &schema::diff, "a"_a, "b"_a,
            "Return a schema.Diff bitmask of the fields that differ.");

        // -------------------------------------------------------------------
        // hash submodule
        // -------------------------------------------------------------------

        auto hash_mod = m.def_submodule("hash", "Optional FNV-1a hash helpers");
        hash_mod.attr("FNV1A_64_OFFSET_BASIS") =
            static_cast<uint64_t>(hash::FNV1A_64_OFFSET_BASIS);
        hash_mod.def("fnv1a_64",
            [](nb::bytes const& data, uint64_t seed) -> uint64_t
            { return hash::fnv1a_64(data.c_str(), data.size(), seed); },
            "data"_a, "seed"_a = hash::FNV1A_64_OFFSET_BASIS,
            "64-bit FNV-1a of a byte string.  Chain with `seed=h` to extend.");
        hash_mod.def("identity_from_fnv1a",
            [](std::string const& descriptor) -> nb::bytes
            {
                auto arr = hash::identity_from_fnv1a(descriptor);
                return nb::bytes(reinterpret_cast<char const*>(arr.data()), arr.size());
            },
            "descriptor"_a,
            "Pack a 64-bit FNV-1a of `descriptor` into the leading 8 bytes "
            "of a 64-byte identity slot, zero-padding the rest.");

        // -------------------------------------------------------------------
        // HealthReport
        // -------------------------------------------------------------------

        nb::class_<SharedRegion::HealthReport>(m, "HealthReport")
            .def_ro("locked_entries", &SharedRegion::HealthReport::locked_entries)
            .def_ro("retired_rings",  &SharedRegion::HealthReport::retired_rings)
            .def_ro("draining_rings", &SharedRegion::HealthReport::draining_rings)
            .def_ro("live_rings",     &SharedRegion::HealthReport::live_rings)
            .def_ro("schema_stuck",   &SharedRegion::HealthReport::schema_stuck)
            .def("__repr__", [](SharedRegion::HealthReport const& r)
            {
                return "HealthReport(locked=" + std::to_string(r.locked_entries) +
                       ", retired=" + std::to_string(r.retired_rings) +
                       ", draining=" + std::to_string(r.draining_rings) +
                       ", live=" + std::to_string(r.live_rings) +
                       ", schema_stuck=" + (r.schema_stuck ? "True" : "False") + ")";
            });

        // -------------------------------------------------------------------
        // SharedRegion
        // -------------------------------------------------------------------

        nb::class_<SharedRegion>(m, "SharedRegion")
            .def_static("create",
                [](char const* name, channel::Type type,
                   channel::Config const& cfg, std::string const& creator)
                { return SharedRegion::create(name, type, cfg, creator.c_str()); },
                "name"_a, "type"_a, "cfg"_a, "creator"_a = std::string{""},
                nb::rv_policy::move)
            .def_static("open", &SharedRegion::open, "name"_a,
                nb::rv_policy::move)
            .def_static("create_or_open",
                [](char const* name, channel::Type type,
                   channel::Config const& cfg, std::string const& creator)
                { return SharedRegion::create_or_open(name, type, cfg, creator.c_str()); },
                "name"_a, "type"_a, "cfg"_a, "creator"_a = std::string{""},
                nb::rv_policy::move)
            .def("name",          &SharedRegion::name)
            .def("channel_type",  &SharedRegion::channel_type)
            .def("schema",        &SharedRegion::schema)
            .def("try_claim_schema",   &SharedRegion::try_claim_schema,   "info"_a)
            .def("reset_schema_claim", &SharedRegion::reset_schema_claim)
            .def("diagnose",               &SharedRegion::diagnose)
            .def("repair_locked_entries", &SharedRegion::repair_locked_entries)
            .def("reset_retired_rings",   &SharedRegion::reset_retired_rings)
            .def("reclaim_orphaned_slots",&SharedRegion::reclaim_orphaned_slots)
            .def("unlink",        &SharedRegion::unlink);

        m.def("unlink_shm", [](std::string const& name) { SharedMemory::unlink(name); },
              "name"_a, "Unlink a shared-memory entry by name (no-op if absent).");

        // -------------------------------------------------------------------
        // SampleRef — byte-copy sample.
        // Returns a bytes copy of the payload (the buffer is subscriber-local
        // and reused across try_receive() calls, so copying out is the only
        // way to keep the bytes beyond the next receive).
        // -------------------------------------------------------------------

        nb::class_<Subscriber::SampleRef>(m, "SampleRef")
            .def("data",
                [](Subscriber::SampleRef const& s) -> nb::bytes
                {
                    return nb::bytes(reinterpret_cast<char const*>(s.data()),
                                     s.len());
                },
                "Return the payload as a bytes object (copies out of the "
                "subscriber-local buffer).")
            .def("len",      &Subscriber::SampleRef::len)
            .def("ring_pos", &Subscriber::SampleRef::ring_pos);

        // -------------------------------------------------------------------
        // SampleView — zero-copy, pins the slot.
        // Buffer protocol via memoryview(view).  The view holds a refcount
        // pin on the slot; the pin is released when the view is destroyed.
        // -------------------------------------------------------------------

        nb::class_<Subscriber::SampleView>(m, "SampleView")
            .def("data",
                [](Subscriber::SampleView const& v) -> nb::object
                {
                    if (not v.valid())
                    {
                        return nb::none();
                    }
                    return memview_readonly(v.data(), v.len());
                },
                "Return a read-only memoryview pointing directly at the "
                "pinned shared-memory slot (zero-copy).  Valid while this "
                "SampleView is alive.")
            .def("len",      &Subscriber::SampleView::len)
            .def("ring_pos", &Subscriber::SampleView::ring_pos)
            .def("valid",    &Subscriber::SampleView::valid)
            .def("release",
                [](Subscriber::SampleView& v)
                {
                    // Destroy-in-place by move-assigning a default-constructed
                    // view: the release() private helper fires in the
                    // destructor of the temporary.
                    v = Subscriber::SampleView{};
                },
                "Release the slot pin early (equivalent to deleting the view).")
            // Note on context-manager support: initial attempts at
            // __enter__/__exit__ bindings produced nanobind dispatch
            // errors due to the interaction between the returned
            // reference and nanobind's per-argument type matching.
            // Users who want `with` semantics can implement a tiny
            // Python wrapper calling .release() — or rely on Python's
            // GC to release the pin when the SampleView goes out of
            // scope.
            ;

        // -------------------------------------------------------------------
        // Publisher
        // -------------------------------------------------------------------

        // Publisher holds raw pointers into the SharedRegion's mmap.
        // keep_alive<1, 2>: arg 2 (region) must stay alive while arg 1
        // (self) is alive — otherwise the mmap could be unmapped before
        // the Publisher's destructor runs, producing a segfault.
        nb::class_<Publisher>(m, "Publisher")
            .def(nb::init<SharedRegion&>(), "region"_a,
                 nb::keep_alive<1, 2>())
            .def("send",
                [](Publisher& p, nb::bytes const& data) -> int32_t
                { return p.send(data.c_str(), data.size()); },
                "data"_a,
                "Copy `data` into a slot and publish.  Returns bytes written "
                "or a negative errno-style code.")
            .def("allocate",
                [](Publisher& p, std::size_t len) -> nb::object
                {
                    void* ptr = p.allocate(len);
                    if (ptr == nullptr)
                    {
                        return nb::none();
                    }
                    return memview_writable(ptr, len);
                },
                "len"_a,
                "Reserve a slot of `len` bytes and return a writable memoryview "
                "pointing directly at it.  Fill the view, then call publish().  "
                "Returns None if the pool is exhausted.")
            .def("publish",
                [](Publisher& p) -> std::size_t { return p.publish(); },
                "Commit the slot reserved by the last allocate() call.  "
                "Returns the number of rings the sample was delivered to.")
            .def("dropped", &Publisher::dropped);

        // -------------------------------------------------------------------
        // Subscriber
        // -------------------------------------------------------------------

        // Same lifetime rule as Publisher — region's mmap must outlive
        // the Subscriber.
        nb::class_<Subscriber>(m, "Subscriber")
            .def(nb::init<SharedRegion&>(), "region"_a,
                 nb::keep_alive<1, 2>())
            .def("try_receive",
                [](Subscriber& s) -> nb::object
                {
                    auto sample = s.try_receive();
                    if (not sample.has_value())
                    {
                        return nb::none();
                    }
                    return nb::bytes(
                        reinterpret_cast<char const*>(sample->data()),
                        sample->len());
                },
                "Non-blocking receive.  Returns bytes on success, None if "
                "no message is available.")
            .def("receive",
                [](Subscriber& s, int64_t timeout_ns) -> nb::object
                {
                    std::optional<Subscriber::SampleRef> sample;
                    {
                        nb::gil_scoped_release release;
                        sample = s.receive(nanoseconds{timeout_ns});
                    }
                    if (not sample.has_value())
                    {
                        return nb::none();
                    }
                    return nb::bytes(
                        reinterpret_cast<char const*>(sample->data()),
                        sample->len());
                },
                "timeout_ns"_a,
                "Blocking receive with timeout.  Releases the GIL while "
                "waiting.  Returns bytes on success, None on timeout.")
            // keep_alive<0, 1>: the returned SampleView (arg 0) must keep
            // the Subscriber (arg 1 = self) alive — the view dereferences
            // mmap pointers owned transitively by self on destruction.
            .def("try_receive_view",
                [](Subscriber& s) -> std::optional<Subscriber::SampleView>
                { return s.try_receive_view(); },
                nb::rv_policy::move,
                nb::keep_alive<0, 1>(),
                "Non-blocking zero-copy receive.  Returns a SampleView (pins "
                "the slot) or None.")
            .def("receive_view",
                [](Subscriber& s, int64_t timeout_ns)
                    -> std::optional<Subscriber::SampleView>
                {
                    nb::gil_scoped_release release;
                    return s.receive_view(nanoseconds{timeout_ns});
                },
                "timeout_ns"_a,
                nb::rv_policy::move,
                nb::keep_alive<0, 1>(),
                "Blocking zero-copy receive.  Releases the GIL.  Returns a "
                "SampleView or None on timeout.")
            .def("lost",            &Subscriber::lost)
            .def("drain_timeouts",  &Subscriber::drain_timeouts);

        // -------------------------------------------------------------------
        // BroadcastHandle
        // -------------------------------------------------------------------

        // BroadcastHandle — Publisher/Subscriber are move-only, so the
        // fields are exposed read-only.  The handle itself is the owner;
        // callers use .pub / .sub as references.
        nb::class_<BroadcastHandle>(m, "BroadcastHandle")
            .def_prop_ro("pub",
                [](BroadcastHandle& h) -> Publisher& { return h.pub; },
                nb::rv_policy::reference_internal)
            .def_prop_ro("sub",
                [](BroadcastHandle& h) -> Subscriber& { return h.sub; },
                nb::rv_policy::reference_internal);

        // -------------------------------------------------------------------
        // Node
        // -------------------------------------------------------------------

        // Node-returned Publisher/Subscriber/BroadcastHandle all point
        // into SharedRegion objects stored inside the Node itself.
        // keep_alive<0, 1>: the return value (0) pins the Node (1 = self).
        nb::class_<Node>(m, "Node")
            .def(nb::init<std::string const&, std::string const&>(),
                 "name"_a, "prefix"_a = std::string{"kickmsg"})
            .def("advertise",
                [](Node& n, char const* topic, channel::Config const& cfg)
                { return n.advertise(topic, cfg); },
                "topic"_a, "cfg"_a = channel::Config{},
                nb::rv_policy::move, nb::keep_alive<0, 1>())
            .def("subscribe", &Node::subscribe, "topic"_a,
                nb::rv_policy::move, nb::keep_alive<0, 1>())
            .def("advertise_or_join", &Node::advertise_or_join, "topic"_a, "cfg"_a,
                nb::rv_policy::move, nb::keep_alive<0, 1>())
            .def("subscribe_or_create", &Node::subscribe_or_create, "topic"_a, "cfg"_a,
                nb::rv_policy::move, nb::keep_alive<0, 1>())
            .def("join_broadcast",
                [](Node& n, char const* channel, channel::Config const& cfg)
                { return n.join_broadcast(channel, cfg); },
                "channel"_a, "cfg"_a = channel::Config{},
                nb::rv_policy::move, nb::keep_alive<0, 1>())
            .def("create_mailbox",
                [](Node& n, char const* tag, channel::Config const& cfg)
                { return n.create_mailbox(tag, cfg); },
                "tag"_a, "cfg"_a = channel::Config{},
                nb::rv_policy::move, nb::keep_alive<0, 1>())
            .def("open_mailbox", &Node::open_mailbox, "owner_node"_a, "tag"_a,
                nb::rv_policy::move, nb::keep_alive<0, 1>())
            .def("unlink_topic",     &Node::unlink_topic,     "topic"_a)
            .def("unlink_broadcast", &Node::unlink_broadcast, "channel"_a)
            .def("unlink_mailbox",
                [](Node const& n, char const* tag,
                   std::optional<std::string> const& owner_node)
                {
                    if (owner_node.has_value())
                    {
                        n.unlink_mailbox(tag, owner_node->c_str());
                    }
                    else
                    {
                        n.unlink_mailbox(tag);
                    }
                },
                "tag"_a, "owner_node"_a = nb::none())
            .def("topic_schema",           &Node::topic_schema,           "topic"_a)
            .def("try_claim_topic_schema", &Node::try_claim_topic_schema,
                 "topic"_a, "info"_a)
            .def("name",   &Node::name)
            .def("prefix", &Node::prefix);
    }
}
