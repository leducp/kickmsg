/// @file kickmsg_py.cc
/// @brief Python bindings for Kickmsg (nanobind-based).
///
/// Layout:
///   kickmsg                  — module
///     ChannelType            — enum
///     Config                 — channel::Config
///     SchemaInfo             — payload schema descriptor
///     HealthReport           — SharedRegion::diagnose() result
///     RingStats / RegionStats — SharedRegion::stats() result
///     SharedRegion           — factory methods + schema/health/repair/stats
///     Publisher              — send(bytes) + allocate() → AllocatedSlot
///     AllocatedSlot          — writable zero-copy handle + .publish()
///     Subscriber             — try_receive / receive (GIL release) / *_view
///     SampleView             — read-only zero-copy sample (buffer protocol)
///     BroadcastHandle        — NamedTuple-like (pub, sub)
///     Role                   — registry::Role enum (Publisher/Subscriber/Both)
///     Participant            — registry snapshot entry
///     Registry               — per-namespace participant discovery
///     Node                   — high-level topic / broadcast / mailbox
///     schema (submodule)
///       Diff                 — enum (bitmask)
///       diff(a, b)           — pure diff function
///     hash (submodule)
///       fnv1a_64(data[, seed])
///       identity_from_fnv1a(descriptor)
///
/// Zero-copy contract (lifetime-safe via the Python buffer protocol):
///
///   slot = pub.allocate(N)      → AllocatedSlot.  memoryview(slot) is a
///                                 writable view into the SHM slot.  The
///                                 memoryview pins the slot, which pins
///                                 the Publisher, which pins the mmap —
///                                 so retained memoryviews stay valid
///                                 (at the mmap level) as long as Python
///                                 holds them.
///   slot.publish()              → commits.  NEW memoryview(slot) after
///                                 this raises BufferError.  Memoryviews
///                                 obtained BEFORE publish remain pointer-
///                                 valid but writing through them after
///                                 publish would corrupt in-flight
///                                 subscribers — user contract: don't.
///
///   view = sub.try_receive_view() → SampleView.  memoryview(view) is a
///                                 read-only view into the SHM slot.  The
///                                 memoryview pins the SampleView, which
///                                 pins the slot's refcount and the mmap.
///   view.release()              → drops the pin.  NEW memoryview(view)
///                                 after this raises BufferError.
///
///   Equivalent context-manager form (preferred for short scopes):
///       with sub.try_receive_view() as view:
///           mv = memoryview(view)
///           ... use mv ...
///       # pin released on block exit, even on exception
///
/// The pinning is enforced by Py_buffer::obj = self + Py_INCREF inside
/// the buffer-protocol getbuffer slot, so it works with numpy.asarray(),
/// torch.frombuffer(), and any other consumer that respects the protocol.

#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

#include <nanobind/nanobind.h>
#include <nanobind/stl/chrono.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "kickmsg/Node.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Region.h"
#include "kickmsg/Registry.h"
#include "kickmsg/Subscriber.h"
#include "kickmsg/Hash.h"
#include "kickmsg/os/Process.h"
#include "kickmsg/types.h"

namespace nb = nanobind;
using namespace nb::literals;

namespace kickmsg
{
    // Python-only wrapper around a Publisher reservation.  Holds the slot
    // pointer + length returned by Publisher::allocate(), exposes the
    // writable buffer protocol so `memoryview(slot)` points directly into
    // the shared-memory slot (zero-copy), and has a .publish() method
    // that commits via the Publisher.
    //
    // Lifetime: the Py_buffer obtained through buffer protocol pins this
    // AllocatedSlot alive (view->obj = self; Py_INCREF), which in turn
    // pins the Publisher (via nb::keep_alive<1, 2> on the constructor),
    // which pins the SharedRegion mmap.  A memoryview retained past
    // `.publish()` stays technically valid as a pointer — but any NEW
    // memoryview(slot) after publish is refused with BufferError so
    // accidental reuse is caught.
    struct PyAllocatedSlot
    {
        Publisher*  publisher;
        void*       ptr;
        std::size_t len;
        bool        published;

        PyAllocatedSlot(Publisher& p, void* data, std::size_t n)
            : publisher{&p}, ptr{data}, len{n}, published{false}
        {
        }
    };
}

namespace
{
    // Buffer protocol for Subscriber::SampleView (read-only zero-copy).
    // Sets view->obj = self + Py_INCREF so the resulting memoryview pins
    // the SampleView alive, which transitively pins the slot refcount
    // and the mmap.
    int sv_getbuffer(PyObject* self, Py_buffer* view, int /*flags*/) noexcept
    {
        using SV = kickmsg::Subscriber::SampleView;
        auto* sv = nb::inst_ptr<SV>(nb::handle(self));

        if (not sv->valid())
        {
            PyErr_SetString(PyExc_BufferError,
                "SampleView is no longer valid (pin already released)");
            view->obj = nullptr;
            return -1;
        }

        view->buf        = const_cast<void*>(sv->data());
        view->obj        = self;
        Py_INCREF(self);
        view->len        = static_cast<Py_ssize_t>(sv->len());
        view->itemsize   = 1;
        view->readonly   = 1;
        view->ndim       = 1;
        view->format     = nullptr;          // defaults to "B" (raw bytes)
        view->shape      = &view->len;       // borrow: lives in the Py_buffer
        view->strides    = &view->itemsize;
        view->suboffsets = nullptr;
        view->internal   = nullptr;
        return 0;
    }

    void sv_releasebuffer(PyObject* /*self*/, Py_buffer* /*view*/) noexcept
    {
        // Nothing to free: shape/strides borrow from the Py_buffer itself,
        // and Py_DECREF(view->obj) is handled by CPython's memoryview.
    }

    PyType_Slot sv_slots[] = {
        { Py_bf_getbuffer,     reinterpret_cast<void*>(sv_getbuffer)     },
        { Py_bf_releasebuffer, reinterpret_cast<void*>(sv_releasebuffer) },
        { 0, nullptr }
    };

    // Buffer protocol for PyAllocatedSlot (writable zero-copy).  Refuses
    // new buffer requests once .publish() has been called so stale writes
    // don't corrupt messages that are already in flight to subscribers.
    int as_getbuffer(PyObject* self, Py_buffer* view, int /*flags*/) noexcept
    {
        auto* slot = nb::inst_ptr<kickmsg::PyAllocatedSlot>(nb::handle(self));

        if (slot->published)
        {
            PyErr_SetString(PyExc_BufferError,
                "AllocatedSlot has already been published; its buffer is "
                "no longer writable");
            view->obj = nullptr;
            return -1;
        }

        view->buf        = slot->ptr;
        view->obj        = self;
        Py_INCREF(self);
        view->len        = static_cast<Py_ssize_t>(slot->len);
        view->itemsize   = 1;
        view->readonly   = 0;                // writable
        view->ndim       = 1;
        view->format     = nullptr;
        view->shape      = &view->len;
        view->strides    = &view->itemsize;
        view->suboffsets = nullptr;
        view->internal   = nullptr;
        return 0;
    }

    void as_releasebuffer(PyObject* /*self*/, Py_buffer* /*view*/) noexcept
    {
    }

    PyType_Slot as_slots[] = {
        { Py_bf_getbuffer,     reinterpret_cast<void*>(as_getbuffer)     },
        { Py_bf_releasebuffer, reinterpret_cast<void*>(as_releasebuffer) },
        { 0, nullptr }
    };

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
    // Native module name is `_native`; the outer `kickmsg/__init__.py` does
    // `from ._native import *` so user-visible import paths (kickmsg.Publisher,
    // kickmsg.Node, …) are unchanged.
    NB_MODULE(_native, m)
    {
        m.doc() = "Kickmsg — lock-free shared-memory IPC (native bindings)";

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
            .def_prop_rw("commit_timeout",
                [](channel::Config const& c) -> microseconds
                { return c.commit_timeout; },
                [](channel::Config& c, microseconds us)
                { c.commit_timeout = us; },
                "Commit timeout as a timedelta (microsecond resolution).")
            .def_rw("schema", &channel::Config::schema)
            .def("__repr__", [](channel::Config const& c)
            {
                return std::string{"Config(max_subscribers="} +
                       std::to_string(c.max_subscribers) +
                       ", pool_size=" + std::to_string(c.pool_size) +
                       ", max_payload_size=" + std::to_string(c.max_payload_size) + ")";
            });

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
                        throw nb::value_error(
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
                        throw nb::value_error(
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
        // RingStats / RegionStats — runtime counter snapshot via stats()
        // -------------------------------------------------------------------

        nb::class_<RingStats>(m, "RingStats")
            .def_ro("state",         &RingStats::state)
            .def_ro("in_flight",     &RingStats::in_flight)
            .def_ro("write_pos",     &RingStats::write_pos)
            .def_ro("dropped_count", &RingStats::dropped_count)
            .def_ro("lost_count",    &RingStats::lost_count)
            .def("__repr__", [](RingStats const& r)
            {
                char const* state_name = "?";
                switch (r.state)
                {
                    case ring::Free:     state_name = "Free";     break;
                    case ring::Live:     state_name = "Live";     break;
                    case ring::Draining: state_name = "Draining"; break;
                }
                return std::string{"RingStats(state="} + state_name +
                       ", in_flight=" + std::to_string(r.in_flight) +
                       ", write_pos=" + std::to_string(r.write_pos) +
                       ", dropped=" + std::to_string(r.dropped_count) +
                       ", lost=" + std::to_string(r.lost_count) + ")";
            });

        nb::class_<RegionStats>(m, "RegionStats")
            .def_ro("rings",        &RegionStats::rings)
            .def_ro("total_writes", &RegionStats::total_writes)
            .def_ro("total_drops",  &RegionStats::total_drops)
            .def_ro("total_losses", &RegionStats::total_losses)
            .def_ro("live_rings",   &RegionStats::live_rings)
            .def_ro("pool_free",    &RegionStats::pool_free)
            .def_ro("pool_size",    &RegionStats::pool_size)
            .def("__repr__", [](RegionStats const& s)
            {
                return std::string{"RegionStats(live_rings="} +
                       std::to_string(s.live_rings) +
                       ", total_writes=" + std::to_string(s.total_writes) +
                       ", total_drops=" + std::to_string(s.total_drops) +
                       ", total_losses=" + std::to_string(s.total_losses) +
                       ", pool_free=" + std::to_string(s.pool_free) +
                       "/" + std::to_string(s.pool_size) + ")";
            });

        nb::class_<RegionInfo>(m, "RegionInfo")
            .def_ro("shm_name",          &RegionInfo::shm_name)
            .def_ro("channel_type",      &RegionInfo::channel_type)
            .def_ro("version",           &RegionInfo::version)
            .def_ro("config_hash",       &RegionInfo::config_hash)
            .def_ro("total_size",        &RegionInfo::total_size)
            .def_ro("max_subs",          &RegionInfo::max_subs)
            .def_ro("sub_ring_capacity", &RegionInfo::sub_ring_capacity)
            .def_ro("pool_size",         &RegionInfo::pool_size)
            .def_ro("max_payload_size",  &RegionInfo::max_payload_size)
            .def_ro("commit_timeout_us", &RegionInfo::commit_timeout_us)
            .def_ro("creator_pid",       &RegionInfo::creator_pid)
            .def_ro("creator_name",      &RegionInfo::creator_name)
            .def_ro("created_at_ns",     &RegionInfo::created_at_ns)
            .def("__repr__", [](RegionInfo const& i)
            {
                return std::string{"RegionInfo(shm='"} + i.shm_name +
                       "', version=" + std::to_string(i.version) +
                       ", creator_pid=" + std::to_string(i.creator_pid) +
                       ", creator='" + i.creator_name + "')";
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
            .def_prop_ro("name",         &SharedRegion::name)
            .def_prop_ro("channel_type", &SharedRegion::channel_type)
            .def("schema",               &SharedRegion::schema)
            .def("try_claim_schema",   &SharedRegion::try_claim_schema,   "info"_a)
            .def("reset_schema_claim", &SharedRegion::reset_schema_claim)
            .def("diagnose",               &SharedRegion::diagnose)
            .def("stats",                  &SharedRegion::stats,
                 "Runtime counter snapshot (per-ring + aggregate). "
                 "Safe under live traffic.")
            .def("info",                   &SharedRegion::info,
                 "Static header metadata: geometry, creator, version.")
            .def("repair_locked_entries", &SharedRegion::repair_locked_entries)
            .def("reset_retired_rings",   &SharedRegion::reset_retired_rings)
            .def("reclaim_orphaned_slots",&SharedRegion::reclaim_orphaned_slots)
            .def("unlink",        &SharedRegion::unlink)
            .def("__repr__", [](SharedRegion const& r)
            {
                std::string type_str =
                    (r.channel_type() == channel::PubSub) ? "PubSub" : "Broadcast";
                return std::string{"SharedRegion(name='"} + r.name() +
                       "', type=" + type_str + ")";
            });

        m.def("unlink_shm", [](std::string const& name) { SharedMemory::unlink(name); },
              "name"_a, "Unlink a shared-memory entry by name (no-op if absent).");

        // -------------------------------------------------------------------
        // Registry — per-namespace participant directory
        // -------------------------------------------------------------------

        nb::enum_<registry::Role>(m, "Role")
            .value("Publisher",  registry::Publisher)
            .value("Subscriber", registry::Subscriber)
            .value("Both",       registry::Both);

        nb::enum_<registry::Kind>(m, "Kind")
            .value("Pubsub",    registry::Pubsub)
            .value("Broadcast", registry::Broadcast)
            .value("Mailbox",   registry::Mailbox);

        nb::class_<Participant>(m, "Participant")
            .def_ro("pid",            &Participant::pid)
            .def_ro("pid_starttime",  &Participant::pid_starttime)
            .def_ro("created_at_ns",  &Participant::created_at_ns)
            .def_ro("channel_type",   &Participant::channel_type)
            .def_ro("role",           &Participant::role)
            .def_ro("kind",           &Participant::kind)
            .def_ro("shm_name",       &Participant::shm_name)
            .def_ro("topic_name",     &Participant::topic_name)
            .def_ro("node_name",      &Participant::node_name)
            .def("__repr__", [](Participant const& p)
            {
                char const* role_name = "?";
                switch (p.role)
                {
                    case registry::Publisher:  role_name = "Publisher";  break;
                    case registry::Subscriber: role_name = "Subscriber"; break;
                    case registry::Both:       role_name = "Both";       break;
                }
                return std::string{"Participant(topic='"} + p.topic_name +
                       "', node='" + p.node_name +
                       "', pid=" + std::to_string(p.pid) +
                       ", role=" + role_name + ")";
            });

        nb::class_<TopicSummary>(m, "TopicSummary")
            .def_ro("shm_name",        &TopicSummary::shm_name)
            .def_ro("topic_name",      &TopicSummary::topic_name)
            .def_ro("channel_type",    &TopicSummary::channel_type)
            .def_ro("kind",            &TopicSummary::kind)
            .def_ro("producers",       &TopicSummary::producers)
            .def_ro("consumers",       &TopicSummary::consumers)
            .def_ro("stall_producers", &TopicSummary::stall_producers)
            .def_ro("stall_consumers", &TopicSummary::stall_consumers)
            .def("__repr__", [](TopicSummary const& t)
            {
                return std::string{"TopicSummary(topic='"} + t.topic_name +
                       "', producers=" + std::to_string(t.producers.size()) +
                       ", consumers=" + std::to_string(t.consumers.size()) +
                       ", stalled=" +
                       std::to_string(t.stall_producers.size()
                                      + t.stall_consumers.size()) + ")";
            });

        nb::class_<Registry>(m, "Registry")
            .def_static("open_or_create", &Registry::open_or_create,
                        "namespace"_a, "capacity"_a = registry::DEFAULT_CAPACITY,
                        nb::rv_policy::move,
                        "Open the registry SHM for `namespace`, creating it if absent.")
            .def_static("try_open", &Registry::try_open, "namespace"_a,
                        nb::rv_policy::move,
                        "Open an existing registry; returns None if none exists.")
            .def_static("unlink", &Registry::unlink, "namespace"_a,
                        "Remove the registry SHM for `namespace` from the filesystem.")
            .def("snapshot", &Registry::snapshot,
                 "Copy all currently Active participant entries.  Does not "
                 "filter by process liveness.")
            .def("list_topics", &Registry::list_topics,
                 "Topic-centric view: groups participants by shm_name and "
                 "splits them into producer/consumer × alive/stall lanes.")
            .def("sweep_stale", &Registry::sweep_stale,
                 "Reclaim slots owned by processes that no longer exist.  "
                 "Returns the number of slots freed.")
            .def_prop_ro("name",     &Registry::name)
            .def_prop_ro("capacity", &Registry::capacity)
            .def("__repr__", [](Registry const& r)
            {
                return std::string{"Registry(name='"} + r.name() +
                       "', capacity=" + std::to_string(r.capacity()) + ")";
            });

        m.def("process_exists", &process_exists, "pid"_a,
              "Return True if a process with `pid` exists on this host.");
        m.def("current_pid", &current_pid,
              "Return the PID of the current process.");

        // SampleRef (the C++ byte-copy sample) is not bound directly —
        // try_receive() / receive() auto-convert it to `bytes` at the
        // Python boundary.  Users who want ring-position information
        // can use try_receive_view() / receive_view() which return
        // SampleView (bound below).

        // -------------------------------------------------------------------
        // SampleView — zero-copy, pins the slot.
        //
        // Supports the Python buffer protocol: `memoryview(view)` returns
        // a read-only memoryview pointing directly at shared memory (no
        // copy).  The memoryview pins the SampleView alive — so retaining
        // a memoryview beyond the SampleView's Python reference keeps the
        // slot pinned and the mmap valid until the memoryview is released.
        // That makes the zero-copy path lifetime-safe by construction.
        // -------------------------------------------------------------------

        nb::class_<Subscriber::SampleView>(m, "SampleView",
            nb::type_slots(sv_slots))
            // __len__ so `len(view)` works; ring_pos / valid as properties
            // (no-arg accessors, Pythonic).
            .def("__len__",
                [](Subscriber::SampleView const& v) -> std::size_t
                { return v.len(); })
            .def_prop_ro("ring_pos", &Subscriber::SampleView::ring_pos)
            .def_prop_ro("valid",    &Subscriber::SampleView::valid)
            .def("release",
                [](Subscriber::SampleView& v)
                {
                    // Move-assign a default-constructed view: the old
                    // state's release() fires via the move-assignment,
                    // dropping the pin.  Subsequent memoryview(view)
                    // calls fail with BufferError (see sv_getbuffer).
                    v = Subscriber::SampleView{};
                },
                "Release the slot pin early.  Idempotent; after this, any "
                "NEW memoryview(view) call raises BufferError.  Memoryviews "
                "obtained before .release() remain valid as pointers but "
                "should not be used (the pin is gone).")
            // Context-manager support: `with view:` releases the pin on
            // block exit.
            //
            // __enter__ returns self with reference_internal rv_policy so
            // nanobind resolves to the existing Python wrapper rather
            // than constructing a second one around the same C++ object
            // (which would double-release on exit).
            //
            // __exit__ uses nb::args to accept the three positional
            // arguments Python's `with` statement passes (exc_type,
            // exc_value, traceback) — explicit `(nb::object, nb::object,
            // nb::object)` triggers a dispatch error in nanobind's
            // multi-arg resolution (nb::args sidesteps it).
            .def("__enter__",
                [](Subscriber::SampleView& v) -> Subscriber::SampleView&
                { return v; },
                nb::rv_policy::reference_internal)
            .def("__exit__",
                [](Subscriber::SampleView& v, nb::args /*exc_info*/)
                { v = Subscriber::SampleView{}; })
            .def("__repr__", [](Subscriber::SampleView const& v)
            {
                return std::string{"SampleView(len="} + std::to_string(v.len()) +
                       ", valid=" + (v.valid() ? "True" : "False") + ")";
            });

        // -------------------------------------------------------------------
        // AllocatedSlot — handle returned by Publisher.allocate().
        //
        // Supports the writable buffer protocol: `memoryview(slot)` or
        // `numpy.asarray(slot)` gets you a zero-copy writable view of the
        // reserved shared-memory slot.  Fill it in place, then call
        // `slot.publish()` to commit.  After publish, any NEW
        // memoryview(slot) call raises BufferError.
        //
        // keep_alive<1, 2>: keep the Publisher (arg 2) alive while this
        // slot (self, arg 1) is alive — the slot points into the
        // Publisher's mmap and must not outlive it.
        // -------------------------------------------------------------------

        nb::class_<PyAllocatedSlot>(m, "AllocatedSlot",
            nb::type_slots(as_slots))
            .def("publish",
                [](PyAllocatedSlot& s) -> std::size_t
                {
                    if (s.published)
                    {
                        throw nb::value_error(
                            "AllocatedSlot.publish() called more than once");
                    }
                    s.published = true;
                    return s.publisher->publish();
                },
                "Commit the reserved slot.  Returns the number of rings "
                "the sample was delivered to.  After this call, any NEW "
                "memoryview(slot) fails with BufferError.")
            .def("__len__",
                [](PyAllocatedSlot const& s) -> std::size_t { return s.len; })
            .def_prop_ro("published",
                [](PyAllocatedSlot const& s) -> bool { return s.published; })
            .def("__repr__", [](PyAllocatedSlot const& s)
            {
                return std::string{"AllocatedSlot(len="} + std::to_string(s.len) +
                       ", published=" + (s.published ? "True" : "False") + ")";
            });

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
                [](Publisher& p, nb::bytes const& data) -> std::size_t
                {
                    int32_t rc = p.send(data.c_str(), data.size());
                    if (rc >= 0)
                    {
                        return static_cast<std::size_t>(rc);
                    }
                    // C++ returns negative errno-style codes; translate to
                    // Python exceptions so callers don't silently drop
                    // messages by ignoring a "falsy" negative return.
                    int err = -rc;
                    if (err == EMSGSIZE)
                    {
                        throw nb::value_error(
                            "message too large: exceeds max_payload_size");
                    }
                    if (err == EAGAIN)
                    {
                        PyErr_SetString(PyExc_BlockingIOError,
                            "slot pool exhausted; try again after "
                            "subscribers drain");
                        throw nb::python_error();
                    }
                    // Any other negative rc: generic OSError with errno.
                    PyErr_SetFromErrno(PyExc_OSError);
                    throw nb::python_error();
                },
                "data"_a,
                "Copy `data` into a slot and publish (atomic convenience).  "
                "Returns the number of bytes written.  Raises ValueError if "
                "the message exceeds max_payload_size, BlockingIOError if "
                "the slot pool is exhausted, OSError on other failures.")
            .def("allocate",
                [](Publisher& p, std::size_t len) -> std::optional<PyAllocatedSlot>
                {
                    void* ptr = p.allocate(len);
                    if (ptr == nullptr)
                    {
                        return std::nullopt;
                    }
                    return PyAllocatedSlot{p, ptr, len};
                },
                "len"_a,
                // keep_alive<0, 1>: the returned AllocatedSlot (arg 0)
                // must pin the Publisher (arg 1 = self).  Memoryviews
                // obtained from the slot in turn pin the AllocatedSlot
                // (via Py_buffer::obj), so the full chain is
                // memoryview → AllocatedSlot → Publisher → SharedRegion.
                nb::keep_alive<0, 1>(),
                "Reserve a slot of `len` bytes and return an AllocatedSlot.  "
                "Use memoryview(slot) or numpy.asarray(slot) to fill it "
                "in place (zero-copy), then call slot.publish().  Returns "
                "None if the pool is exhausted.")
            .def_prop_ro("dropped", &Publisher::dropped,
                "Per-ring delivery drops (CAS contention or pool exhaustion).")
            .def("__repr__", [](Publisher const& p)
            {
                return std::string{"Publisher(dropped="} +
                       std::to_string(p.dropped()) + ")";
            });

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
                [](Subscriber& s, nanoseconds timeout) -> nb::object
                {
                    std::optional<Subscriber::SampleRef> sample;
                    {
                        nb::gil_scoped_release release;
                        sample = s.receive(timeout);
                    }
                    if (not sample.has_value())
                    {
                        return nb::none();
                    }
                    return nb::bytes(
                        reinterpret_cast<char const*>(sample->data()),
                        sample->len());
                },
                "timeout"_a,
                "Blocking receive with timeout (timedelta).  Releases the GIL while "
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
                [](Subscriber& s, nanoseconds timeout)
                    -> std::optional<Subscriber::SampleView>
                {
                    // Scope the GIL release tightly around the blocking
                    // wait, matching receive() above.  The C++ return value
                    // is pure C++ (no Python state), so strictly speaking
                    // the GIL only needs to be released for the futex
                    // wait itself — but keeping the scope explicit avoids
                    // any future-footgun if the Python-conversion path
                    // ever touches CPython state before reacquisition.
                    std::optional<Subscriber::SampleView> result;
                    {
                        nb::gil_scoped_release release;
                        result = s.receive_view(timeout);
                    }
                    return result;
                },
                "timeout"_a,
                nb::rv_policy::move,
                nb::keep_alive<0, 1>(),
                "Blocking zero-copy receive (timedelta timeout).  Releases the GIL.  Returns a "
                "SampleView or None on timeout.")
            .def_prop_ro("lost",           &Subscriber::lost,
                "Messages the subscriber's ring overflowed past — the "
                "publisher evicted them before this subscriber drained.")
            .def_prop_ro("drain_timeouts", &Subscriber::drain_timeouts,
                "Count of times the subscriber gave up waiting for an "
                "in-flight publisher during teardown.")
            .def("__repr__", [](Subscriber const& s)
            {
                return std::string{"Subscriber(lost="} + std::to_string(s.lost()) +
                       ", drain_timeouts=" + std::to_string(s.drain_timeouts()) + ")";
            })
            .def("__iter__", [](Subscriber& s) -> Subscriber& { return s; },
                 nb::rv_policy::reference)
            .def("__next__", [](Subscriber& s) -> nb::bytes
            {
                auto sample = s.try_receive();
                if (!sample.has_value())
                {
                    throw nb::stop_iteration();
                }
                return nb::bytes(reinterpret_cast<char const*>(sample->data()), sample->len());
            });

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
                nb::rv_policy::reference_internal)
            .def("__repr__", [](BroadcastHandle const& /*h*/)
            {
                return std::string{"BroadcastHandle(pub=Publisher, sub=Subscriber)"};
            });

        // -------------------------------------------------------------------
        // Node
        // -------------------------------------------------------------------

        // Node-returned Publisher/Subscriber/BroadcastHandle all point
        // into SharedRegion objects stored inside the Node itself.
        // keep_alive<0, 1>: the return value (0) pins the Node (1 = self).
        nb::class_<Node>(m, "Node")
            .def(nb::init<std::string const&, std::string const&>(),
                 "name"_a, "namespace"_a = std::string{"kickmsg"})
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
            .def_prop_ro("name",      &Node::name)
            .def_prop_ro("namespace", &Node::kmsg_namespace)
            .def("__repr__", [](Node const& n)
            {
                return std::string{"Node(name='"} + n.name() +
                       "', namespace='" + n.kmsg_namespace() + "')";
            });
    }
}
