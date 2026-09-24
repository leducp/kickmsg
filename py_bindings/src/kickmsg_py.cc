/// @file kickmsg_py.cc
/// Python bindings using nanobind.
///
/// Exported buffers keep wrappers and mappings alive. They do not extend
/// reservation validity: stop using writable views before publish() or the
/// next allocate(). Do not use SampleView buffers after release().

#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

#include <nanobind/nanobind.h>
#include <nanobind/stl/chrono.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unordered_map.h>
#include <nanobind/stl/vector.h>

#include "kickmsg/Blackboard.h"
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
    // Copy Blackboard values into Python bytes; cells cannot be safely exported.
    struct PyReadOutcome
    {
        std::error_code ec;
        nb::bytes       data;
        uint64_t        updated_at_ns;
        uint64_t        update_count;
    };

    /// Build OSError with (errno, message); nanobind has no OSError helper.
    void raise_if(std::error_code ec, char const* what)
    {
        if (ec)
        {
            std::string const msg = std::string{what} + ": " + ec.message();
            PyObject*         args = Py_BuildValue("(is)", ec.value(), msg.c_str());
            PyErr_SetObject(PyExc_OSError, args);
            Py_XDECREF(args);
            throw nb::python_error();
        }
    }

}

namespace
{
    // Py_buffer::obj keeps the SampleView and its mapping alive.
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
        // CPython decrefs view->obj; shape and strides use the Py_buffer storage.
    }

    PyType_Slot sv_slots[] = {
        { Py_bf_getbuffer,     reinterpret_cast<void*>(sv_getbuffer)     },
        { Py_bf_releasebuffer, reinterpret_cast<void*>(sv_releasebuffer) },
        { 0, nullptr }
    };

    // Reject new buffer requests after publication or reservation replacement.
    int as_getbuffer(PyObject* self, Py_buffer* view, int /*flags*/) noexcept
    {
        auto* slot = nb::inst_ptr<kickmsg::AllocatedSlot>(nb::handle(self));

        if (slot->published())
        {
            PyErr_SetString(PyExc_BufferError,
                "AllocatedSlot has already been published; its buffer is "
                "no longer writable");
            view->obj = nullptr;
            return -1;
        }

        if (not slot->valid())
        {
            PyErr_SetString(PyExc_BufferError,
                "AllocatedSlot was superseded by a later Publisher.allocate(); "
                "its slot is back in the pool and writing through it would "
                "corrupt another reservation");
            view->obj = nullptr;
            return -1;
        }

        view->buf        = slot->data();
        view->obj        = self;
        Py_INCREF(self);
        view->len        = static_cast<Py_ssize_t>(slot->max_size());
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
    NB_MODULE(_native, m)
    {
        m.doc() = "Kickmsg — lock-free shared-memory IPC (native bindings)";

        // Enums & simple types

        // channel::None is exposed as NoChannel: `ChannelType.None` would be
        // a syntax error in Python.
        nb::enum_<channel::Type>(m, "ChannelType")
            .value("NoChannel", channel::None)
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
            .def_rw("identity", &channel::Config::identity,
                "Optional logical-identity fingerprint stamped into the "
                "region header at create time and verified at open when "
                "both sides are nonzero. Not part of the config hash.")
            .def("__repr__", [](channel::Config const& c)
            {
                return std::string{"Config(max_subscribers="} +
                       std::to_string(c.max_subscribers) +
                       ", pool_size=" + std::to_string(c.pool_size) +
                       ", max_payload_size=" + std::to_string(c.max_payload_size) + ")";
            });

        // SchemaInfo + schema submodule (Diff / diff)

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

        // hash submodule

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

        // HealthReport

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

        // RingStats / RegionStats

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
                    case ring::Free:       { state_name = "Free";       break; }
                    case ring::Live:       { state_name = "Live";       break; }
                    case ring::Draining:   { state_name = "Draining";   break; }
                    case ring::Reclaiming: { state_name = "Reclaiming"; break; }
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
            .def_ro("total_steals", &RegionStats::total_steals)
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
                       ", total_steals=" + std::to_string(s.total_steals) +
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

        // SharedRegion

        nb::class_<SharedRegion>(m, "SharedRegion")
            .def_static("create",
                [](char const* name, channel::Type type,
                   channel::Config const& cfg, std::string const& creator)
                { return SharedRegion::create(name, type, cfg, creator.c_str()); },
                "name"_a, "type"_a, "cfg"_a, "creator"_a = std::string{""},
                nb::rv_policy::move)
            .def_static("open", &SharedRegion::open, "name"_a,
                "expected_identity"_a = uint64_t{0},
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

        // Registry

        nb::enum_<registry::Role>(m, "Role")
            .value("Publisher",  registry::Publisher)
            .value("Subscriber", registry::Subscriber)
            .value("Both",       registry::Both);

        nb::enum_<registry::Kind>(m, "Kind")
            .value("Pubsub",     registry::Pubsub)
            .value("Broadcast",  registry::Broadcast)
            .value("Mailbox",    registry::Mailbox)
            .value("Blackboard", registry::Blackboard);

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
                    case registry::Publisher:  { role_name = "Publisher";  break; }
                    case registry::Subscriber: { role_name = "Subscriber"; break; }
                    case registry::Both:       { role_name = "Both";       break; }
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

        // Copy samples become bytes; SampleView also exposes ring position.

        // SampleView
        // Read-only exported buffers keep the wrapper alive. release() drops its pin.

        nb::class_<Subscriber::SampleView>(m, "SampleView",
            nb::type_slots(sv_slots))
            .def("__len__",
                [](Subscriber::SampleView const& v) -> std::size_t
                { return v.len(); })
            .def_prop_ro("ring_pos", &Subscriber::SampleView::ring_pos)
            .def_prop_ro("valid",    &Subscriber::SampleView::valid)
            .def("release",
                [](Subscriber::SampleView& v)
                {
                    // Release the pin and make future buffer requests fail.
                    v = Subscriber::SampleView{};
                },
                "Release the slot pin early.  Idempotent; after this, any "
                "NEW memoryview(view) call raises BufferError.  Memoryviews "
                "obtained before .release() remain valid as pointers but "
                "should not be used (the pin is gone).")
            // Return the existing wrapper from __enter__ to avoid a second owner.
            // nb::args accepts the three exception arguments passed to __exit__.
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

        // AllocatedSlot
        // The token rejects stale handles; existing writable buffers cannot be revoked.
        // Stop using them before publish() or another allocate().
        // keep_alive keeps the publisher mapped while the handle exists.

        nb::class_<AllocatedSlot>(m, "AllocatedSlot",
            nb::type_slots(as_slots))
            .def("publish",
                [](AllocatedSlot& s, std::size_t len) -> std::size_t
                {
                    if (s.published())
                    {
                        throw nb::value_error(
                            "AllocatedSlot.publish() called more than once");
                    }
                    if (not s.valid())
                    {
                        throw nb::value_error(
                            "AllocatedSlot was superseded by a later "
                            "Publisher.allocate(); publishing it would commit "
                            "the newer reservation");
                    }
                    if (len > s.max_size())
                    {
                        throw nb::value_error(
                            "publish(len) exceeds slot max_size");
                    }
                    return s.publish(len);
                },
                "len"_a,
                "Publish len bytes. Returns the number of rings delivered to. "
                "Further memoryview(slot) requests raise BufferError.")
            .def("__len__",
                [](AllocatedSlot const& s) -> std::size_t { return s.max_size(); })
            .def_prop_ro("max_size",
                [](AllocatedSlot const& s) -> std::size_t { return s.max_size(); })
            .def_prop_ro("published", &AllocatedSlot::published)
            .def_prop_ro("valid", &AllocatedSlot::valid,
                "False after publish() or another Publisher.allocate(). "
                "Existing buffers cannot be revoked; stop using them before "
                "either call.")
            .def("__repr__", [](AllocatedSlot const& s)
            {
                char const* published = "False";
                if (s.published())
                {
                    published = "True";
                }
                return std::string{"AllocatedSlot(max_size="} +
                       std::to_string(s.max_size()) +
                       ", published=" + published + ")";
            });

        // Publisher

        // keep_alive<1, 2> keeps the region mapped until the publisher is destroyed.
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
                    // Translate negative errno returns into Python exceptions.
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
                [](Publisher& p) -> std::optional<AllocatedSlot>
                {
                    auto slot = p.allocate();
                    if (not slot.valid())
                    {
                        return std::nullopt;
                    }
                    return slot;
                },
                // Keep the publisher alive while the reservation handle exists.
                nb::keep_alive<0, 1>(),
                "Reserve a slot sized to max_payload_size and return an "
                "AllocatedSlot.  Use memoryview(slot) or numpy.asarray(slot) "
                "to write up to slot.max_size bytes in place (zero-copy), "
                "then call slot.publish(n) with the actual number of bytes "
                "written.  Returns None if the pool is exhausted.  Supersedes "
                "any previous reservation: an earlier AllocatedSlot becomes "
                "invalid and refuses publish() and new memoryview() requests.  "
                "Buffers already exported from it cannot be revoked: writing "
                "through one after this call corrupts whichever reservation "
                "now holds the slot.")
            .def_prop_ro("dropped", &Publisher::dropped,
                "Per-ring delivery drops (CAS contention or pool exhaustion).")
            .def("__repr__", [](Publisher const& p)
            {
                return std::string{"Publisher(dropped="} +
                       std::to_string(p.dropped()) + ")";
            });

        // Subscriber

        // Keep the region alive while the subscriber exists.
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
            // Keep the subscriber and its mapping alive while the view exists.
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
                    // Reacquire the GIL before converting the C++ result to Python.
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

        // BroadcastHandle

        // Move-only fields are exposed by reference; the handle owns them.
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

        // Node

        // Returned handles keep the Node and its regions alive.

        nb::class_<blackboard::Config>(m, "BlackboardConfig")
            .def(nb::init<>())
            .def_rw("capacity",       &blackboard::Config::capacity)
            .def_rw("max_value_size", &blackboard::Config::max_value_size)
            .def_rw("identity",       &blackboard::Config::identity)
            .def("__repr__", [](blackboard::Config const& c)
            {
                return std::string{"BlackboardConfig(capacity="}
                     + std::to_string(c.capacity)
                     + ", max_value_size=" + std::to_string(c.max_value_size) + ")";
            });

        nb::class_<blackboard::KeyStatus>(m, "KeyStatus")
            .def_ro("key",           &blackboard::KeyStatus::key)
            .def_ro("value_len",     &blackboard::KeyStatus::value_len)
            .def_ro("updated_at_ns", &blackboard::KeyStatus::updated_at_ns)
            .def_ro("update_count",  &blackboard::KeyStatus::update_count)
            .def_ro("owner_pid",     &blackboard::KeyStatus::owner_pid)
            .def_ro("owner_node",    &blackboard::KeyStatus::owner_node)
            .def_ro("owner_alive",   &blackboard::KeyStatus::owner_alive)
            .def("__repr__", [](blackboard::KeyStatus const& k)
            {
                return std::string{"KeyStatus(key='"} + k.key
                     + "', updates=" + std::to_string(k.update_count)
                     + ", owner_pid=" + std::to_string(k.owner_pid) + ")";
            });

        nb::class_<PyReadOutcome>(m, "ReadOutcome")
            .def_prop_ro("errno", [](PyReadOutcome const& r)
                { return r.ec.value(); },
                "0 on success, otherwise the standard errno -- compare against "
                "the stdlib errno module (ENOENT: no such key, ENOMSG: declared "
                "but never written, EAGAIN: transient, retry).")
            .def_prop_ro("error", [](PyReadOutcome const& r) { return r.ec.message(); })
            .def_ro("data",          &PyReadOutcome::data)
            .def_ro("updated_at_ns", &PyReadOutcome::updated_at_ns)
            .def_ro("update_count",  &PyReadOutcome::update_count)
            .def("__len__", [](PyReadOutcome const& r) { return r.data.size(); })
            .def("__bool__", [](PyReadOutcome const& r) { return not r.ec; })
            .def("__repr__", [](PyReadOutcome const& r)
            {
                return std::string{"ReadOutcome(errno="}
                     + std::to_string(r.ec.value())
                     + ", len=" + std::to_string(r.data.size()) + ")";
            });

        nb::class_<Blackboard::Writer>(m, "BlackboardWriter")
            .def("write",
                [](Blackboard::Writer& w, nb::bytes const& data)
                { raise_if(w.write(data.c_str(), data.size()), "write"); },
                "data"_a,
                "Publish a new value.  Raises OSError -- EMSGSIZE if the value "
                "exceeds the board's max_value_size, ENOTRECOVERABLE if this "
                "writer no longer owns the key; the previous value is left "
                "untouched either way.")
            .def("release",
                [](Blackboard::Writer& w) { raise_if(w.release(), "release"); },
                 "Drop ownership now instead of at destruction.  The value, its "
                 "timestamp and its update count all survive.")
            .def_prop_ro("key",   &Blackboard::Writer::key)
            .def_prop_ro("valid", &Blackboard::Writer::valid)
            .def("__repr__", [](Blackboard::Writer const& w)
            {
                return std::string{"BlackboardWriter(key='"} + w.key() + "')";
            });

        nb::class_<Blackboard::Reader>(m, "BlackboardReader")
            .def("read",
                [](Blackboard::Reader const& r) -> PyReadOutcome
                {
                    std::vector<uint8_t> buf;
                    auto out = r.read(buf);
                    return PyReadOutcome{
                        out.ec,
                        nb::bytes(reinterpret_cast<char const*>(buf.data()), buf.size()),
                        out.updated_at_ns, out.update_count};
                },
                "Copy the current value.  Returns a ReadOutcome.  Values are "
                "always copied -- there is no memoryview form, because a writer "
                "may overwrite the cell mid-read.")
            .def("owner_alive", &Blackboard::Reader::owner_alive,
                 "Probe whether the key's owner process still exists.  Costs "
                 "one OS call -- not a hot-path call.")
            .def_prop_ro("key", &Blackboard::Reader::key)
            .def("__repr__", [](Blackboard::Reader const& r)
            {
                return std::string{"BlackboardReader(key='"} + r.key() + "')";
            });

        nb::class_<Blackboard>(m, "Blackboard")
            .def_static("open_or_create", &Blackboard::open_or_create,
                        "namespace"_a, "name"_a, "cfg"_a = blackboard::Config{},
                        "owner_name"_a = "",
                        nb::rv_policy::move,
                        "Open the blackboard SHM for (namespace, name), creating "
                        "it if absent.  `owner_name` labels every key this board "
                        "declares.")
            .def_static("try_open", &Blackboard::try_open, "namespace"_a, "name"_a,
                        nb::rv_policy::move,
                        "Open an existing blackboard; returns None if none exists.")
            .def_static("unlink", &Blackboard::unlink, "namespace"_a, "name"_a,
                        "Remove the blackboard SHM from the filesystem.")
            .def_static("shm_name", &Blackboard::shm_name, "namespace"_a, "name"_a)
            .def("declare",
                [](Blackboard& b, char const* key,
                   std::optional<std::string> const& owner_node) -> Blackboard::Writer
                {
                    if (owner_node.has_value())
                    {
                        return b.declare(key, owner_node->c_str());
                    }
                    return b.declare(key);
                },
                "key"_a, "owner_node"_a = nb::none(),
                nb::rv_policy::move, nb::keep_alive<0, 1>(),
                "Claim exclusive ownership of `key`.  `owner_node` defaults to "
                "the board's owner name.  Raises if a live process already owns "
                "the key or the board is at capacity.")
            .def("observe", &Blackboard::observe, "key"_a,
                 nb::rv_policy::move, nb::keep_alive<0, 1>(),
                 "Track `key` for O(1) reads.  Never creates it: a reader on a "
                 "key that does not exist yet reads ENOENT, and starts "
                 "succeeding as soon as a writer declares and writes it.")
            .def_prop_ro("change_seq", &Blackboard::change_seq)
            .def("wait",
                [](Blackboard& b, uint64_t last_seen, nanoseconds timeout)
                {
                    std::error_code ec;
                    {
                        nb::gil_scoped_release release;
                        ec = b.wait(last_seen, timeout);
                    }
                    // A timeout is an ordinary answer here, not a failure.
                    return not ec;
                },
                "last_seen"_a, "timeout"_a,
                "Block until change_seq differs from `last_seen`, or `timeout` "
                "(a timedelta) elapses.  Releases the GIL while blocked.  Pass "
                "the change_seq you last acted on -- that is what closes the "
                "lost-wakeup window.")
            .def("keys",
                [](Blackboard const& b, std::string const& prefix)
                {
                    std::vector<std::string> out;
                    {
                        nb::gil_scoped_release release;
                        out = b.keys(prefix);
                    }
                    return out;
                },
                "prefix"_a = std::string{},
                "Names of every active key, prefix-filtered.  Takes no board "
                "lock and probes no owner, unlike snapshot().")
            .def("read_all",
                [](Blackboard const& b, std::string const& prefix)
                {
                    std::unordered_map<std::string, std::vector<uint8_t>> raw;
                    {
                        nb::gil_scoped_release release;
                        raw = b.read_all(prefix);
                    }
                    std::unordered_map<std::string, nb::bytes> out;
                    out.reserve(raw.size());
                    for (auto const& [key, value] : raw)
                    {
                        out.emplace(key,
                            nb::bytes(reinterpret_cast<char const*>(value.data()), value.size()));
                    }
                    return out;
                },
                "prefix"_a = std::string{},
                "Every prefix-matching key that holds a value, as a dict of "
                "key -> bytes.  A key overtaken by its writer mid-read is "
                "dropped rather than returned torn.")
            .def("snapshot",
                [](Blackboard const& b)
                {
                    std::vector<blackboard::KeyStatus> out;
                    {
                        nb::gil_scoped_release release;
                        out = b.snapshot();
                    }
                    return out;
                },
                "Diagnostic copy of every active key.  Probes owner liveness "
                "(one OS call per key), so the GIL is released.")
            .def("sweep_stale",
                [](Blackboard& b)
                {
                    uint32_t freed = 0;
                    {
                        // One liveness probe per candidate key, so this can be
                        // thousands of OS calls on a large board.
                        nb::gil_scoped_release release;
                        freed = b.sweep_stale();
                    }
                    return freed;
                },
                "Reclaim crash residue: free keys whose owner process is "
                "provably dead (destroying their values) and recover entries "
                "left mid-operation by a process that died.  Safe under live "
                "traffic.  Releases the GIL.")
            .def_prop_ro("name",           &Blackboard::name)
            .def_prop_ro("capacity",       &Blackboard::capacity)
            .def_prop_ro("max_value_size", &Blackboard::max_value_size)
            .def("__repr__", [](Blackboard const& b)
            {
                return std::string{"Blackboard(name='"} + b.name()
                     + "', capacity=" + std::to_string(b.capacity()) + ")";
            });

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
            .def("advertise_or_join",
                [](Node& n, char const* topic, channel::Config const& cfg)
                { return n.advertise_or_join(topic, cfg); },
                "topic"_a, "cfg"_a,
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
            .def("open_mailbox",
                [](Node& n, char const* owner_node, char const* tag)
                { return n.open_mailbox(owner_node, tag); },
                "owner_node"_a, "tag"_a,
                nb::rv_policy::move, nb::keep_alive<0, 1>())
            .def("blackboard",
                [](Node& n, char const* name, blackboard::Config const& cfg)
                    -> Blackboard& { return n.blackboard(name, cfg); },
                "name"_a, "cfg"_a = blackboard::Config{},
                nb::rv_policy::reference_internal)
            .def("unlink_blackboard", &Node::unlink_blackboard, "name"_a)
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
