#include <gtest/gtest.h>

#include "kickmsg/Region.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

#include <atomic>
#include <thread>

#include "kickmsg/os/Process.h"

#ifndef _WIN32
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <unistd.h>
#else
    #include <malloc.h>   // _aligned_malloc / _aligned_free
#endif

using namespace std::chrono;

namespace
{
    // Windows aligned allocations must be released with _aligned_free.
    void* aligned_buffer_alloc(std::size_t align, std::size_t size)
    {
#if defined(_WIN32)
        return _aligned_malloc(size, align);
#else
        void* p = nullptr;
        if (::posix_memalign(&p, align, size) != 0)
        {
            return nullptr;
        }
        return p;
#endif
    }

    void aligned_buffer_free(void* p)
    {
#if defined(_WIN32)
        _aligned_free(p);
#else
        ::free(p);
#endif
    }
}

class RegionTest : public ::testing::Test
{
public:
    static constexpr char const* SHM_NAME = "/kickmsg_test_region";

    void SetUp() override
    {
        kickmsg::SharedMemory::unlink(SHM_NAME);
    }

    void TearDown() override
    {
        kickmsg::SharedMemory::unlink(SHM_NAME);
    }

    kickmsg::channel::Config default_cfg()
    {
        kickmsg::channel::Config cfg;
        cfg.max_subscribers   = 4;
        cfg.sub_ring_capacity = 8;
        cfg.pool_size         = 16;
        cfg.max_payload_size  = 64;
        return cfg;
    }
};

TEST_F(RegionTest, CreateAndValidateHeader)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg, "test");
    auto* header   = region.header();

    EXPECT_EQ(header->magic, kickmsg::MAGIC);
    EXPECT_EQ(header->version, kickmsg::VERSION);
    EXPECT_EQ(header->channel_type, kickmsg::channel::PubSub);
    EXPECT_EQ(header->max_subs, cfg.max_subscribers);
    EXPECT_EQ(header->sub_ring_capacity, cfg.sub_ring_capacity);
    EXPECT_EQ(header->sub_ring_mask, cfg.sub_ring_capacity - 1);
    EXPECT_EQ(header->pool_size, cfg.pool_size);
    EXPECT_EQ(header->slot_data_size, cfg.max_payload_size);
    EXPECT_EQ(header->creator_name_len, 4u);

    std::string creator(kickmsg::header_creator_name(header), header->creator_name_len);
    EXPECT_EQ(creator, "test");
}

TEST_F(RegionTest, OpenExistingRegion)
{
    auto cfg = default_cfg();
    auto r1  = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg, "orig");
    auto r2  = kickmsg::SharedRegion::open(SHM_NAME);

    EXPECT_EQ(r2.header()->magic, kickmsg::MAGIC);
    EXPECT_EQ(r2.header()->pool_size, cfg.pool_size);
}

TEST_F(RegionTest, OpenNonexistentThrows)
{
    EXPECT_THROW(kickmsg::SharedRegion::open("/kickmsg_nonexistent_42"), std::runtime_error);
}

#ifndef _WIN32
TEST(SharedMemoryTest, TryOpenOnSizeZeroSegmentReturnsFalse)
{
    // An object created before ftruncate has size zero; opening must retry.
    char const* name = "/kickmsg_test_size0";
    ::shm_unlink(name);
    int fd = ::shm_open(name, O_RDWR | O_CREAT, 0666);
    ASSERT_GE(fd, 0);

    kickmsg::SharedMemory sm;
    EXPECT_FALSE(sm.try_open(name));

    ::close(fd);
    ::shm_unlink(name);
}
#endif

TEST_F(RegionTest, CreateOrOpenFirstCreates)
{
    auto cfg = default_cfg();
    auto r   = kickmsg::SharedRegion::create_or_open(
                   SHM_NAME, kickmsg::channel::Broadcast, cfg, "creator");

    EXPECT_EQ(r.header()->magic, kickmsg::MAGIC);
    EXPECT_EQ(r.header()->channel_type, kickmsg::channel::Broadcast);
}

TEST_F(RegionTest, CreateOrOpenSecondOpens)
{
    auto cfg = default_cfg();
    auto r1  = kickmsg::SharedRegion::create_or_open(
                   SHM_NAME, kickmsg::channel::Broadcast, cfg, "first");
    auto r2  = kickmsg::SharedRegion::create_or_open(
                   SHM_NAME, kickmsg::channel::Broadcast, cfg, "second");

    EXPECT_EQ(r2.header()->magic, kickmsg::MAGIC);
    EXPECT_EQ(r2.header()->pool_size, cfg.pool_size);
}

TEST_F(RegionTest, CreateOrOpenConfigMismatchThrows)
{
    auto cfg = default_cfg();
    // Keep the region alive so the mapping exists when create_or_open runs.
    // On Windows, named mappings are destroyed when the last handle closes.
    auto existing = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg, "node");

    auto bad_cfg = cfg;
    bad_cfg.max_payload_size = cfg.max_payload_size * 2;
    EXPECT_THROW(
        kickmsg::SharedRegion::create_or_open(
            SHM_NAME, kickmsg::channel::PubSub, bad_cfg, "other"),
        std::runtime_error);
}

TEST_F(RegionTest, CreateOrOpenValidatesGeometryOnOpenBranch)
{
    auto cfg = default_cfg();
    auto creator = kickmsg::SharedRegion::create_or_open(
        SHM_NAME, kickmsg::channel::PubSub, cfg, "creator");

    // Change an offset not covered by config_hash, then exercise the open branch.
    creator.header()->pool_offset = UINT64_MAX;

    EXPECT_THROW(
        kickmsg::SharedRegion::create_or_open(
            SHM_NAME, kickmsg::channel::PubSub, cfg, "opener"),
        std::runtime_error);
}

TEST_F(RegionTest, CreateRejectsOverlongCreatorName)
{
    std::string huge(70000, 'x');  // exceeds the uint16_t creator_name_len
    EXPECT_THROW(
        kickmsg::SharedRegion::create(
            SHM_NAME, kickmsg::channel::PubSub, default_cfg(), huge.c_str()),
        std::runtime_error);
}

TEST_F(RegionTest, RequiredSizeRejectsOverflowingConfig)
{
    auto cfg = default_cfg();
    cfg.pool_size = SIZE_MAX / 2;  // pool_size * slot_stride wraps total_size
    EXPECT_THROW(kickmsg::SharedRegion::required_size(cfg), std::runtime_error);
}

TEST_F(RegionTest, SchemaNameForcedNulTerminatedOnRead)
{
    auto region = kickmsg::SharedRegion::create(
        SHM_NAME, kickmsg::channel::PubSub, default_cfg());
    auto* h = region.header();

    // Corrupt: fill name with non-zero bytes and publish Set directly.
    std::memset(h->schema_data.name, 'A', sizeof(h->schema_data.name));
    h->schema_state.store(kickmsg::schema::Set, std::memory_order_release);

    auto got = region.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name[sizeof(got->name) - 1], '\0');
}

TEST_F(RegionTest, HeaderStoresCreatorMetadata)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg, "my_node");
    auto* header   = region.header();

    EXPECT_EQ(header->creator_pid, kickmsg::current_pid());
    EXPECT_GT(header->created_at_ns, 0u);
    EXPECT_NE(header->config_hash, 0u);
}

TEST_F(RegionTest, NonPowerOfTwoRingThrows)
{
    auto cfg = default_cfg();
    cfg.sub_ring_capacity = 7;
    EXPECT_THROW(
        kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg),
        std::runtime_error);
}

TEST_F(RegionTest, TreiberPopAllThenPushBack)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    auto* base  = region.base();
    auto* header   = region.header();

    std::vector<uint32_t> popped;
    for (uint32_t i = 0; i < cfg.pool_size; ++i)
    {
        uint32_t idx = kickmsg::treiber_pop(header->free_top, base, region.geometry());
        ASSERT_NE(idx, kickmsg::INVALID_SLOT) << "Pop failed at iteration " << i;
        popped.push_back(idx);
    }

    EXPECT_EQ(kickmsg::treiber_pop(header->free_top, base, region.geometry()), kickmsg::INVALID_SLOT);

    for (auto idx : popped)
    {
        auto* slot = kickmsg::slot_at(base, region.geometry(), idx);
        kickmsg::treiber_push(header->free_top, slot, idx);
    }

    uint32_t count = 0;
    uint32_t top = kickmsg::tagged_idx(header->free_top.load(std::memory_order_acquire));
    while (top != kickmsg::INVALID_SLOT)
    {
        auto* slot = kickmsg::slot_at(base, region.geometry(), top);
        top = slot->next_free;
        ++count;
    }
    EXPECT_EQ(count, static_cast<uint32_t>(cfg.pool_size));
}

TEST_F(RegionTest, CollectGarbageReclaimsOrphanedSlots)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 64;

    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);
    auto* header = region.header();

    auto count_free = [&]()
    {
        uint32_t count = 0;
        uint64_t top = header->free_top.load(std::memory_order_acquire);
        uint32_t idx = kickmsg::tagged_idx(top);
        while (idx != kickmsg::INVALID_SLOT)
        {
            auto* slot = kickmsg::slot_at(region.base(), region.geometry(), idx);
            idx = slot->next_free;
            ++count;
        }
        return count;
    };

    EXPECT_EQ(count_free(), 16u);

    for (int i = 0; i < 3; ++i)
    {
        uint32_t idx = kickmsg::treiber_pop(header->free_top, region.base(), region.geometry());
        ASSERT_NE(idx, kickmsg::INVALID_SLOT);
        auto* slot = kickmsg::slot_at(region.base(), region.geometry(), idx);
        slot->refcount.store(static_cast<uint32_t>(cfg.max_subscribers),
                             std::memory_order_release);
    }

    EXPECT_EQ(count_free(), 13u);

    std::size_t reclaimed = region.reclaim_orphaned_slots();
    EXPECT_EQ(reclaimed, 3u);
    EXPECT_EQ(count_free(), 16u);

    EXPECT_EQ(region.reclaim_orphaned_slots(), 0u);
}

TEST_F(RegionTest, RepairLockedEntryUnblocksPublishing)
{
    // Verify that after repair_locked_entries(), the repaired ring position
    // can be published over again when the ring wraps.

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;    // capacity = 4, so pos=4 wraps to idx=0
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    // Publish 1 message at pos=0, creating a committed entry with seq=1
    uint32_t val = 100;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    // Consume it so the subscriber is caught up
    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());

    // Simulate a crash at pos=1: manually lock the entry
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // Advance write_pos to simulate that a publisher claimed pos=1
    ring->write_pos.store(2, std::memory_order_release);
    auto& e1 = entries[1]; // pos=1 -> idx=1
    e1.sequence.store(kickmsg::seq_lock(1), std::memory_order_release);

    // Repair should fix the locked entry
    std::size_t repaired = region.repair_locked_entries();
    EXPECT_EQ(repaired, 1u);

    // The repaired entry carries the skip marker for pos = 1
    uint64_t seq = e1.sequence.load(std::memory_order_acquire);
    EXPECT_EQ(seq, kickmsg::seq_skip(1));

    // The entry has no slot claim; repair must leave it unchanged.
    EXPECT_EQ(kickmsg::meta_slot_biased(e1.meta.load(std::memory_order_acquire)), 0u);

    // Publish positions 2 through 5 to reuse both repaired entries.
    for (int i = 0; i < 4; ++i)
    {
        val = static_cast<uint32_t>(200 + i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0)
            << "Publishing failed at iteration " << i
            << " -- repaired entry likely blocked the ring";
    }

    // Subscriber should receive the new messages (some may be lost due to wrapping)
    int received = 0;
    while (auto s = sub.try_receive())
    {
        ++received;
    }
    EXPECT_GT(received, 0);
}

TEST_F(RegionTest, RepairStaleEntryFromCrashedPublisherBeforeCasLock)
{
    // Stage a publisher crash after claiming write_pos but before locking.
    // The entry retains a committed sequence from more than one wrap ago.

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;    // capacity = 4
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    // Fill the ring once: publish 4 messages (pos 0-3), consuming all.
    for (int i = 0; i < 4; ++i)
    {
        uint32_t val = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        auto s = sub.try_receive();
        ASSERT_TRUE(s.has_value());
    }

    // Entry at idx=0 now has seq=1 (committed for pos=0).
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // Leave idx 0 at seq 1 and advance write_pos to 12, two wraps ahead.
    ring->write_pos.store(12, std::memory_order_release);
    // The scan sees pos 8 at idx 0; seq 1 is more than one wrap behind seq 9.

    auto report = region.diagnose();
    EXPECT_GT(report.locked_entries, 0u)
        << "diagnose() should detect the stale entry (Case B)";

    std::size_t repaired = region.repair_locked_entries();
    EXPECT_GT(repaired, 0u)
        << "repair_locked_entries() should advance the stale entry";

    // After repair, the entry at idx=0 carries the skip marker for pos=8
    // (the slot in the scan window [12-4, 12) that maps to idx=0).
    uint64_t seq0 = entries[0].sequence.load(std::memory_order_acquire);
    EXPECT_EQ(seq0, kickmsg::seq_skip(8))
        << "Stale entry should be advanced to the pos=8 skip marker";

    // Publishing should now succeed past the repaired slot.
    for (int i = 0; i < 8; ++i)
    {
        uint32_t val = static_cast<uint32_t>(100 + i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0)
            << "Publishing failed at iteration " << i
            << " -- repaired entry may still be stuck";
    }
}

TEST_F(RegionTest, RepairLockedEntryAtPositionZero)
{
    // Edge case: crash at pos=0 where prev_seq was 0.

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);

    // Simulate crash at pos=0: lock the entry, advance write_pos
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto* entries = kickmsg::ring_entries(ring);
    ring->write_pos.store(1, std::memory_order_release);
    entries[0].sequence.store(kickmsg::seq_lock(0), std::memory_order_release);

    std::size_t repaired = region.repair_locked_entries();
    EXPECT_EQ(repaired, 1u);
    EXPECT_EQ(entries[0].sequence.load(std::memory_order_acquire),
              kickmsg::seq_skip(0));
    EXPECT_EQ(kickmsg::meta_slot_biased(entries[0].meta.load(std::memory_order_acquire)),
              0u);

    // Publishing should work: pos=1,2,3 use fresh indices, pos=4 wraps to idx=0
    // and expects prev_seq=1 -- matches the repaired value
    kickmsg::Publisher pub(region);
    for (int i = 0; i < 5; ++i)
    {
        uint32_t val = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0)
            << "Publishing failed at iteration " << i;
    }
}

TEST_F(RegionTest, DiagnoseHealthyReturnsZeros)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    for (int i = 0; i < 5; ++i)
    {
        uint32_t val = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    }

    auto report = region.diagnose();
    EXPECT_EQ(report.locked_entries, 0u);
    EXPECT_EQ(report.retired_rings, 0u);
}

TEST_F(RegionTest, DiagnoseDetectsLockedEntries)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    // Publish one normal message
    uint32_t val = 1;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    // Simulate a crashed publisher at pos=1: lock the entry
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto* entries = kickmsg::ring_entries(ring);
    ring->write_pos.store(2, std::memory_order_release);
    entries[1].sequence.store(kickmsg::seq_lock(1), std::memory_order_release);

    auto report = region.diagnose();
    EXPECT_EQ(report.locked_entries, 1u);
    EXPECT_EQ(report.retired_rings, 0u);

    // Repair and verify clean
    region.repair_locked_entries();
    report = region.diagnose();
    EXPECT_EQ(report.locked_entries, 0u);
}

TEST_F(RegionTest, DiagnoseDetectsStuckRings)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    // Simulate a stuck ring: Free with stale in_flight
    auto* ring = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    ring->state_flight.store(
        kickmsg::ring::make_packed(kickmsg::ring::Free, 1),
        std::memory_order_release);

    auto report = region.diagnose();
    EXPECT_EQ(report.locked_entries, 0u);
    EXPECT_EQ(report.retired_rings, 1u);

    // Reset retired rings and verify clean
    std::size_t reset = region.reset_retired_rings();
    EXPECT_EQ(reset, 1u);
    report = region.diagnose();
    EXPECT_EQ(report.retired_rings, 0u);

    // Subscriber can now join the recovered ring
    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    uint32_t val = 42;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());

    uint32_t got = 0;
    std::memcpy(&got, sample->data(), sizeof(got));
    EXPECT_EQ(got, 42u);
}

TEST_F(RegionTest, ResetRetiredRingsLeavesDrainingUntouched)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    // Ring 0: retired (Free | in_flight=1) -- should be reset
    auto* ring0 = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    ring0->state_flight.store(
        kickmsg::ring::make_packed(kickmsg::ring::Free, 1),
        std::memory_order_release);

    // Ring 1: draining (Draining | in_flight=1) -- must NOT be touched
    auto* ring1 = kickmsg::sub_ring_at(region.base(), region.geometry(), 1);
    ring1->state_flight.store(
        kickmsg::ring::make_packed(kickmsg::ring::Draining, 1),
        std::memory_order_release);

    auto report = region.diagnose();
    EXPECT_EQ(report.retired_rings, 1u);
    EXPECT_EQ(report.draining_rings, 1u);

    std::size_t reset = region.reset_retired_rings();
    EXPECT_EQ(reset, 1u);  // only the retired ring

    // Ring 0 was reset
    uint32_t packed0 = ring0->state_flight.load(std::memory_order_acquire);
    EXPECT_EQ(packed0, kickmsg::ring::make_packed(kickmsg::ring::Free));

    // Ring 1 is still Draining with in_flight preserved
    uint32_t packed1 = ring1->state_flight.load(std::memory_order_acquire);
    EXPECT_EQ(kickmsg::ring::get_state(packed1), kickmsg::ring::Draining);
    EXPECT_EQ(kickmsg::ring::get_in_flight(packed1), 1u);
}

TEST_F(RegionTest, ReclaimDeadRingsRecoversCrashedOwnerRing)
{
    auto cfg = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    // Ring 0: a subscriber crashed holding it Live (guaranteed-dead pid,
    // same sentinel the registry sweep test uses).
    auto* ring0 = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    ring0->owner_starttime.store(0, std::memory_order_relaxed);
    ring0->owner_pid.store(0x7fffffff, std::memory_order_release);
    ring0->state_flight.store(kickmsg::ring::make_packed(kickmsg::ring::Live),
                              std::memory_order_release);

    // Ring 1: a LIVE owner (this process) must never be reclaimed.
    auto* ring1 = kickmsg::sub_ring_at(region.base(), region.geometry(), 1);
    ring1->owner_starttime.store(
        kickmsg::process_starttime(kickmsg::current_pid()), std::memory_order_relaxed);
    ring1->owner_pid.store(kickmsg::current_pid(), std::memory_order_release);
    ring1->state_flight.store(kickmsg::ring::make_packed(kickmsg::ring::Live),
                              std::memory_order_release);

    EXPECT_EQ(region.diagnose().dead_rings, 1u);
    EXPECT_EQ(region.reclaim_dead_rings(), 1u);

    // Ring 0 reclaimed to Free with owner cleared.
    uint32_t p0 = ring0->state_flight.load(std::memory_order_acquire);
    EXPECT_EQ(kickmsg::ring::get_state(p0), kickmsg::ring::Free);
    EXPECT_EQ(ring0->owner_pid.load(std::memory_order_acquire), 0u);

    // Ring 1 (live owner) untouched.
    uint32_t p1 = ring1->state_flight.load(std::memory_order_acquire);
    EXPECT_EQ(kickmsg::ring::get_state(p1), kickmsg::ring::Live);

    // Idempotent.
    EXPECT_EQ(region.reclaim_dead_rings(), 0u);
    EXPECT_EQ(region.diagnose().dead_rings, 0u);
}

TEST_F(RegionTest, ReclaimDeadRingsPreservesInFlight)
{
    auto cfg = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    // Keep in_flight so a late publisher decrement cannot underflow state.
    auto* ring = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    ring->owner_pid.store(0x7fffffff, std::memory_order_release);
    ring->state_flight.store(kickmsg::ring::make_packed(kickmsg::ring::Live, 1),
                             std::memory_order_release);

    EXPECT_EQ(region.reclaim_dead_rings(), 1u);

    uint32_t p = ring->state_flight.load(std::memory_order_acquire);
    EXPECT_EQ(kickmsg::ring::get_state(p), kickmsg::ring::Free);
    EXPECT_EQ(kickmsg::ring::get_in_flight(p), 1u);  // preserved
}

TEST_F(RegionTest, CollectGarbageDoesNotReclaimLiveSlots)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 64;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    for (int i = 0; i < 4; ++i)
    {
        uint32_t val = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    }

    std::size_t reclaimed = region.reclaim_orphaned_slots();
    EXPECT_EQ(reclaimed, 0u);

    for (int i = 0; i < 4; ++i)
    {
        auto msg = sub.try_receive();
        ASSERT_TRUE(msg.has_value());
    }

    EXPECT_EQ(region.reclaim_orphaned_slots(), 0u);
}

// --- Payload schema descriptor (opt-in, off the hot path) -----------------

namespace
{
    kickmsg::SchemaInfo make_schema(char const* name, uint32_t version,
                                    uint8_t identity_fill, uint8_t layout_fill)
    {
        kickmsg::SchemaInfo s{};
        std::fill(s.identity.begin(), s.identity.end(), identity_fill);
        std::fill(s.layout.begin(),   s.layout.end(),   layout_fill);
        std::snprintf(s.name, sizeof(s.name), "%s", name);
        s.version       = version;
        s.identity_algo = 1;  // user-defined (e.g. sha256)
        s.layout_algo   = 2;  // user-defined (e.g. fletcher-512)
        s.flags         = 0;
        return s;
    }
}

TEST_F(RegionTest, SchemaLayoutIsFiveHundredTwelveBytes)
{
    // Binary ABI guard: this struct lives in shared memory.
    static_assert(sizeof(kickmsg::SchemaInfo) == 512,
                  "SchemaInfo must stay 512 bytes");
    EXPECT_EQ(sizeof(kickmsg::SchemaInfo), 512u);
}

TEST_F(RegionTest, SchemaUnsetByDefault)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    EXPECT_FALSE(region.schema().has_value());
}

TEST_F(RegionTest, SchemaBakedAtCreate)
{
    auto cfg = default_cfg();
    cfg.schema = make_schema("my/Pose", 3, 0xAB, 0xCD);

    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    auto got = region.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "my/Pose");
    EXPECT_EQ(got->version, 3u);
    EXPECT_EQ(got->identity[0],  0xAB);
    EXPECT_EQ(got->identity[63], 0xAB);
    EXPECT_EQ(got->layout[0],    0xCD);
    EXPECT_EQ(got->layout[63],   0xCD);
    EXPECT_EQ(got->identity_algo, 1u);
    EXPECT_EQ(got->layout_algo,   2u);

    // Reserved bytes must be zero so future readers can distinguish
    // "field not set by legacy writer" from "field set to some value".
    for (uint8_t byte : got->reserved)
    {
        EXPECT_EQ(byte, 0u);
    }
}

TEST_F(RegionTest, SchemaClaimOnUnsetRegion)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    EXPECT_FALSE(region.schema().has_value());

    auto info = make_schema("my/Twist", 1, 0x11, 0x22);
    EXPECT_TRUE(region.try_claim_schema(info));

    auto got = region.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "my/Twist");
    EXPECT_EQ(got->version, 1u);
    EXPECT_EQ(got->identity[0], 0x11);
    EXPECT_EQ(got->layout[0],   0x22);
}

TEST_F(RegionTest, SchemaClaimRejectsSecondClaimant)
{
    auto cfg = default_cfg();
    cfg.schema = make_schema("my/Pose", 1, 0xAA, 0xBB);

    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    // Second process tries to claim a *different* schema -- library just
    // reports "not the claimant", it never throws.  User picks the policy.
    auto other = make_schema("other/Pose", 2, 0x00, 0x00);
    EXPECT_FALSE(region.try_claim_schema(other));

    // Original schema is preserved.
    auto got = region.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "my/Pose");
    EXPECT_EQ(got->version, 1u);
    EXPECT_EQ(got->identity[0], 0xAA);
}

TEST_F(RegionTest, SchemaReadersAcrossRegionHandles)
{
    // Mimic the cross-process flow: one handle claims, a second handle
    // opens the same region and must observe the claim.
    auto cfg = default_cfg();
    auto r1  = kickmsg::SharedRegion::create(
                   SHM_NAME, kickmsg::channel::PubSub, cfg);

    EXPECT_TRUE(r1.try_claim_schema(make_schema("shared/Type", 7, 0x55, 0x66)));

    auto r2 = kickmsg::SharedRegion::open(SHM_NAME);
    auto got = r2.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "shared/Type");
    EXPECT_EQ(got->version, 7u);
}

TEST_F(RegionTest, SchemaConcurrentClaimsOneWins)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    constexpr int N = 8;
    std::atomic<int>          winner_count{0};
    std::vector<std::thread>  threads;
    std::atomic<bool>         start{false};

    for (int i = 0; i < N; ++i)
    {
        threads.emplace_back([&, i]()
        {
            while (not start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            auto info = make_schema("racer", static_cast<uint32_t>(i),
                                    static_cast<uint8_t>(i),
                                    static_cast<uint8_t>(i));
            if (region.try_claim_schema(info))
            {
                winner_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(winner_count.load(), 1)
        << "Exactly one try_claim_schema() must report success";

    // All other racers observe the winner's schema through schema().
    auto got = region.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "racer");
}

TEST_F(RegionTest, SchemaReaderDuringClaimingReturnsNullopt)
{
    // Stage Claiming directly so the reader cannot race past this state.
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    auto* h = region.header();

    // Simulate a claim that reached Claiming but hasn't stored Set yet.
    h->schema_state.store(kickmsg::schema::Claiming,
                          std::memory_order_release);

    EXPECT_FALSE(region.schema().has_value());

    // A follow-up Set makes the payload visible.
    auto info = make_schema("done/Type", 1, 0x01, 0x02);
    std::memcpy(&h->schema_data, &info, sizeof(info));
    h->schema_state.store(kickmsg::schema::Set, std::memory_order_release);

    auto got = region.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "done/Type");
}

TEST_F(RegionTest, SchemaResetRecoversWedgedClaimingState)
{
    // Stage a claimant that stopped before publishing Set.
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    auto* h = region.header();

    // Wedge the slot in Claiming.
    h->schema_state.store(kickmsg::schema::Claiming,
                          std::memory_order_release);

    // A fresh claim returns false (wedged, not its fault).
    auto pending = make_schema("retry/Type", 1, 0x00, 0x00);
    EXPECT_FALSE(region.try_claim_schema(pending));

    // Operator confirms the original claimant is gone, resets the slot.
    EXPECT_TRUE(region.reset_schema_claim());
    // Second call is a no-op -- state is already Unset.
    EXPECT_FALSE(region.reset_schema_claim());

    // Subsequent claim now succeeds.
    auto recovered = make_schema("recovered/Type", 2, 0xEE, 0xFF);
    EXPECT_TRUE(region.try_claim_schema(recovered));

    auto got = region.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "recovered/Type");
    EXPECT_EQ(got->version, 2u);
}

TEST_F(RegionTest, SchemaResetIsNoOpWhenNotClaiming)
{
    // reset_schema_claim must leave Unset and Set states untouched.
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    // Unset: no-op.
    EXPECT_FALSE(region.reset_schema_claim());
    EXPECT_FALSE(region.schema().has_value());

    // Set: must not wipe a valid schema.
    ASSERT_TRUE(region.try_claim_schema(
                    make_schema("kept/Type", 1, 0xAB, 0xCD)));
    EXPECT_FALSE(region.reset_schema_claim());
    auto got = region.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "kept/Type");
}

TEST_F(RegionTest, SchemaCreateOrOpenIgnoresOpenerSchemaWhenCreatorHadNone)
{
    auto cfg = default_cfg();
    // Note: cfg.schema intentionally left empty.
    auto existing = kickmsg::SharedRegion::create(
                        SHM_NAME, kickmsg::channel::PubSub, cfg, "creator");
    ASSERT_FALSE(existing.schema().has_value());

    auto opener_cfg = default_cfg();
    opener_cfg.schema = make_schema("opener/Type", 1, 0x11, 0x22);

    auto opened = kickmsg::SharedRegion::create_or_open(
                      SHM_NAME, kickmsg::channel::PubSub, opener_cfg, "opener");

    // Opener's cfg.schema was discarded -- slot is still Unset.
    EXPECT_FALSE(opened.schema().has_value());
}

TEST_F(RegionTest, SchemaCrossHandleObservesClaim)
{
    // Use independent mappings to check schema publication.
    auto cfg = default_cfg();
    auto r1  = kickmsg::SharedRegion::create(
                   SHM_NAME, kickmsg::channel::PubSub, cfg);

    auto r2 = kickmsg::SharedRegion::open(SHM_NAME);

    // Initially both see Unset.
    EXPECT_FALSE(r1.schema().has_value());
    EXPECT_FALSE(r2.schema().has_value());

    // r1 claims.  r2 must observe Set without any further fence on its
    // side -- acquire-load in schema() synchronizes with r1's release-store.
    ASSERT_TRUE(r1.try_claim_schema(make_schema("xhandle/Type", 5, 0x9A, 0xBC)));

    auto got_r2 = r2.schema();
    ASSERT_TRUE(got_r2.has_value());
    EXPECT_STREQ(got_r2->name, "xhandle/Type");
    EXPECT_EQ(got_r2->version, 5u);

    // Second claim via r2 fails (r1's claim stands).
    EXPECT_FALSE(r2.try_claim_schema(make_schema("other/Type", 0, 0, 0)));
}

TEST_F(RegionTest, SchemaResetViaSecondHandleAfterCrash)
{
    // Recover an abandoned claim through a second mapping.
    auto cfg = default_cfg();
    auto r1  = kickmsg::SharedRegion::create(
                   SHM_NAME, kickmsg::channel::PubSub, cfg);

    // Wedge via r1 (simulate crash: CAS Claiming but never reach Set).
    r1.header()->schema_state.store(kickmsg::schema::Claiming,
                                    std::memory_order_release);

    // Second handle (operator's repair tool) opens and recovers.
    auto r2 = kickmsg::SharedRegion::open(SHM_NAME);
    EXPECT_TRUE(r2.reset_schema_claim());

    // r1 sees the reset too (same underlying state).
    EXPECT_FALSE(r1.schema().has_value());

    // Fresh claim via either handle now succeeds.
    EXPECT_TRUE(r1.try_claim_schema(make_schema("after/Reset", 1, 0x42, 0x43)));
    auto got = r2.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "after/Reset");
}

TEST_F(RegionTest, DiagnoseReportsSchemaStuck)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);

    EXPECT_FALSE(region.diagnose().schema_stuck);

    region.header()->schema_state.store(kickmsg::schema::Claiming,
                                        std::memory_order_release);
    EXPECT_TRUE(region.diagnose().schema_stuck);

    // After reset, clean again.
    ASSERT_TRUE(region.reset_schema_claim());
    EXPECT_FALSE(region.diagnose().schema_stuck);

    // A successful claim (state = Set) does NOT register as stuck.
    ASSERT_TRUE(region.try_claim_schema(
                    make_schema("healthy/Type", 1, 0x01, 0x02)));
    EXPECT_FALSE(region.diagnose().schema_stuck);
}

TEST_F(RegionTest, SchemaDoesNotAffectConfigHash)
{
    auto cfg = default_cfg();
    cfg.schema = make_schema("creator/Type", 1, 0xAA, 0xBB);

    auto existing = kickmsg::SharedRegion::create(
                        SHM_NAME, kickmsg::channel::PubSub, cfg, "creator");

    auto other_cfg = default_cfg();
    other_cfg.schema = make_schema("opener/Type", 2, 0xCC, 0xDD);

    // Must succeed -- geometry matches, schema differs but is ignored on open.
    auto opened = kickmsg::SharedRegion::create_or_open(
                      SHM_NAME, kickmsg::channel::PubSub, other_cfg, "opener");

    // Opener observes the creator's schema, not its own -- library doesn't
    // overwrite or enforce anything.
    auto got = opened.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "creator/Type");
}

// stats() -- cross-process counter snapshot

TEST_F(RegionTest, StatsOnFreshRegionReportsZeros)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg, "stats");
    auto s = region.stats();

    EXPECT_EQ(s.rings.size(), cfg.max_subscribers);
    EXPECT_EQ(s.live_rings,   0u);
    EXPECT_EQ(s.total_writes, 0u);
    EXPECT_EQ(s.total_drops,  0u);
    EXPECT_EQ(s.total_losses, 0u);
    EXPECT_EQ(s.pool_size,    cfg.pool_size);
    // Fresh region: every slot is on the free stack.
    EXPECT_EQ(s.pool_free,    cfg.pool_size);

    for (auto const& r : s.rings)
    {
        EXPECT_EQ(r.state,         kickmsg::ring::Free);
        EXPECT_EQ(r.in_flight,     0u);
        EXPECT_EQ(r.write_pos,     0u);
        EXPECT_EQ(r.dropped_count, 0u);
        EXPECT_EQ(r.lost_count,    0u);
    }
}

TEST_F(RegionTest, StatsWritePosAdvancesWithPublishes)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg, "stats");

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    constexpr int N = 5;
    uint32_t payload = 0xC0FFEE;
    for (int i = 0; i < N; ++i)
    {
        ASSERT_GE(pub.send(&payload, sizeof(payload)), 0);
    }

    auto s = region.stats();
    EXPECT_EQ(s.live_rings,   1u);
    EXPECT_EQ(s.total_writes, static_cast<uint64_t>(N));

    // Exactly one ring should be Live and carry write_pos == N.
    std::size_t live_seen = 0;
    for (auto const& r : s.rings)
    {
        if (r.state == kickmsg::ring::Live)
        {
            ++live_seen;
            EXPECT_EQ(r.write_pos, static_cast<uint64_t>(N));
        }
    }
    EXPECT_EQ(live_seen, 1u);
}

TEST_F(RegionTest, StatsLostCountMatchesSubscriberLostOnOverflow)
{
    auto cfg    = default_cfg();  // sub_ring_capacity = 8
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg, "stats");

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    // Publish more than the ring can hold without draining -- forces the
    // subscriber's drain-ahead path to bump lost_count on its next read.
    uint32_t payload = 0;
    std::size_t const to_publish = cfg.sub_ring_capacity * 3;
    for (std::size_t i = 0; i < to_publish; ++i)
    {
        payload = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&payload, sizeof(payload)), 0);
    }

    // Drive the subscriber: the first try_receive hits the drain-ahead
    // branch and jumps read_pos forward, recording the skipped count.
    while (sub.try_receive()) { /* drain */ }

    EXPECT_GT(sub.lost(), 0u);

    auto s = region.stats();
    // Exactly one ring is Live -- its lost_count equals the subscriber's.
    uint64_t ring_lost = 0;
    for (auto const& r : s.rings)
    {
        ring_lost += r.lost_count;
    }
    EXPECT_EQ(ring_lost, sub.lost());
    EXPECT_EQ(s.total_losses, sub.lost());
}

TEST_F(RegionTest, StatsPoolFreeTracksAllocations)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg, "stats");

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    // Hold a slot mid-publish (allocate without publish).
    auto slot = pub.allocate();
    ASSERT_TRUE(slot.valid());

    auto s = region.stats();
    // One slot is popped from the free stack and not yet returned.
    EXPECT_EQ(s.pool_free, cfg.pool_size - 1);
}

// attach_create / attach_open -- caller-provided memory

class InjectedRegionTest : public ::testing::Test
{
public:
    kickmsg::channel::Config default_cfg()
    {
        kickmsg::channel::Config cfg;
        cfg.max_subscribers   = 2;
        cfg.sub_ring_capacity = 8;
        cfg.pool_size         = 16;
        cfg.max_payload_size  = 64;
        return cfg;
    }

    // Aligned heap buffer sized to fit a region with `cfg`.
    struct Buffer
    {
        std::unique_ptr<void, decltype(&aligned_buffer_free)> mem{nullptr, &aligned_buffer_free};
        std::size_t                                           size{0};
        void* get() { return mem.get(); }
    };

    Buffer make_buffer(kickmsg::channel::Config const& cfg, char const* creator = "")
    {
        Buffer b;
        b.size = kickmsg::SharedRegion::required_size(cfg, creator);
        void* raw = aligned_buffer_alloc(kickmsg::CACHE_LINE, b.size);
        EXPECT_NE(raw, nullptr);
        b.mem.reset(raw);
        return b;
    }
};

TEST_F(InjectedRegionTest, RequiredSizeMatchesShmBackedTotalSize)
{
    auto cfg = default_cfg();
    kickmsg::SharedMemory::unlink("/kickmsg_test_inject_size");
    auto shm = kickmsg::SharedRegion::create(
        "/kickmsg_test_inject_size", kickmsg::channel::PubSub, cfg, "x");
    EXPECT_EQ(kickmsg::SharedRegion::required_size(cfg, "x"),
              shm.header()->total_size);
    shm.unlink();
}

TEST_F(InjectedRegionTest, AttachCreateRoundtrip)
{
    auto cfg = default_cfg();
    auto buf = make_buffer(cfg, "inject");

    auto region = kickmsg::SharedRegion::attach_create(
        buf.get(), buf.size, kickmsg::channel::PubSub, cfg, "inject", "label");

    EXPECT_EQ(region.header()->magic, kickmsg::MAGIC);
    EXPECT_EQ(region.header()->version, kickmsg::VERSION);
    EXPECT_EQ(region.info().shm_name, "label");
    EXPECT_EQ(region.info().creator_name, "inject");

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    for (uint32_t i = 0; i < 5; ++i)
    {
        ASSERT_GE(pub.send(&i, sizeof(i)), 0);
    }

    int received = 0;
    while (auto s = sub.try_receive())
    {
        uint32_t got = 0;
        std::memcpy(&got, s->data(), sizeof(got));
        EXPECT_EQ(got, static_cast<uint32_t>(received));
        ++received;
    }
    EXPECT_EQ(received, 5);
}

TEST_F(InjectedRegionTest, AttachOpenSeesStampedRegion)
{
    auto cfg = default_cfg();
    auto buf = make_buffer(cfg, "creator");

    {
        auto creator = kickmsg::SharedRegion::attach_create(
            buf.get(), buf.size, kickmsg::channel::PubSub, cfg, "creator");

        kickmsg::Publisher pub(creator);
        uint32_t val = 0xC0FFEE;
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    }

    // A second handle attaches to the same buffer and validates.
    auto reader = kickmsg::SharedRegion::attach_open(buf.get(), buf.size, "ro");
    EXPECT_EQ(reader.info().shm_name, "ro");
    EXPECT_EQ(reader.info().creator_name, "creator");
    EXPECT_EQ(reader.header()->pool_size, cfg.pool_size);
}

TEST_F(InjectedRegionTest, AttachCreateRejectsMisalignedAddress)
{
    auto cfg  = default_cfg();
    auto buf  = make_buffer(cfg, "x");
    auto* bad = static_cast<char*>(buf.get()) + 1;  // off by one -- not aligned

    EXPECT_THROW(
        kickmsg::SharedRegion::attach_create(
            bad, buf.size - 1, kickmsg::channel::PubSub, cfg, "x"),
        std::runtime_error);
}

TEST_F(InjectedRegionTest, AttachCreateRejectsUndersizedBuffer)
{
    auto cfg = default_cfg();
    auto buf = make_buffer(cfg, "x");

    EXPECT_THROW(
        kickmsg::SharedRegion::attach_create(
            buf.get(), buf.size - 1, kickmsg::channel::PubSub, cfg, "x"),
        std::runtime_error);
}

TEST_F(InjectedRegionTest, AttachOpenRejectsZeroedBuffer)
{
    auto cfg = default_cfg();
    auto buf = make_buffer(cfg, "x");
    std::memset(buf.get(), 0, buf.size);

    EXPECT_THROW(
        kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
        std::runtime_error);
}

TEST_F(InjectedRegionTest, UnlinkOnInjectedRegionIsNoOp)
{
    auto cfg = default_cfg();
    auto buf = make_buffer(cfg, "x");
    auto region = kickmsg::SharedRegion::attach_create(
        buf.get(), buf.size, kickmsg::channel::PubSub, cfg, "x", "should-not-be-unlinked");

    // Must not call shm_unlink on the label, which would fail if it tried --
    // the label is not a path.  Just checks that the call returns cleanly.
    EXPECT_NO_THROW(region.unlink());
    // And the region remains usable after a no-op unlink.
    EXPECT_EQ(region.header()->magic, kickmsg::MAGIC);
}

TEST_F(InjectedRegionTest, AttachOpenRejectsBufferSmallerThanHeader)
{
    alignas(kickmsg::CACHE_LINE) std::byte tiny[kickmsg::CACHE_LINE]{};
    static_assert(sizeof(tiny) < sizeof(kickmsg::Header));

    EXPECT_THROW(
        kickmsg::SharedRegion::attach_open(tiny, sizeof(tiny)),
        std::runtime_error);
}

TEST_F(InjectedRegionTest, MoveLeavesSourceWithNullBase)
{
    auto cfg = default_cfg();
    auto buf = make_buffer(cfg, "x");
    auto src = kickmsg::SharedRegion::attach_create(
        buf.get(), buf.size, kickmsg::channel::PubSub, cfg, "x");

    void* live_base = src.base();
    ASSERT_NE(live_base, nullptr);

    auto dst = std::move(src);
    EXPECT_EQ(dst.base(), live_base);
    EXPECT_EQ(src.base(), nullptr);
}

TEST_F(InjectedRegionTest, MoveAssignLeavesSourceWithNullBase)
{
    auto cfg = default_cfg();
    auto buf = make_buffer(cfg, "x");
    auto src = kickmsg::SharedRegion::attach_create(
        buf.get(), buf.size, kickmsg::channel::PubSub, cfg, "x");

    void* live_base = src.base();
    ASSERT_NE(live_base, nullptr);

    kickmsg::SharedRegion dst;
    dst = std::move(src);
    EXPECT_EQ(dst.base(), live_base);
    EXPECT_EQ(src.base(), nullptr);
}

// Reject invalid shared-memory geometry before using it for pointer arithmetic.
class CorruptedHeaderTest : public InjectedRegionTest
{
public:
    // Make a valid stamped buffer the test can then deface.
    Buffer make_stamped(kickmsg::channel::Config const& cfg)
    {
        auto buf = make_buffer(cfg, "x");
        auto r = kickmsg::SharedRegion::attach_create(
            buf.get(), buf.size, kickmsg::channel::PubSub, cfg, "x");
        (void)r;  // RAII drops; the bytes in `buf` stay stamped
        return buf;
    }

    kickmsg::Header* header(Buffer& b) { return static_cast<kickmsg::Header*>(b.get()); }
};

TEST_F(CorruptedHeaderTest, RejectsZeroMaxSubs)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->max_subs = 0;
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsNonPowerOfTwoRingCapacity)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->sub_ring_capacity = 7;
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsInconsistentRingMask)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->sub_ring_mask = 3;  // capacity is 8, mask should be 7
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsCreatorNameLenOverflow)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->creator_name_len = UINT16_MAX;
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsPoolOffsetPastTotalSize)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->pool_offset = UINT64_MAX;
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsTotalSizeSmallerThanHeader)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->total_size = sizeof(kickmsg::Header) - 1;
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsRingsOverflowingPoolOffset)
{
    auto buf = make_stamped(default_cfg());
    // max_subs * sub_ring_stride must fit in [sub_rings_offset, pool_offset).
    header(buf)->max_subs = UINT64_MAX;
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsPoolOverflowingTotalSize)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->pool_size = UINT64_MAX;
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsTinySlotStride)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->slot_stride = 1;  // smaller than sizeof(SlotHeader)
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsTinyRingStride)
{
    auto buf = make_stamped(default_cfg());
    header(buf)->sub_ring_stride = 1;  // smaller than a SubRingHeader + entries
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, RejectsRingCapacityOverflowingRegion)
{
    auto buf = make_stamped(default_cfg());
    // Huge power-of-two capacity with a consistent mask: passes the
    // power-of-two and mask checks, must trip the pre-multiply overflow guard.
    header(buf)->sub_ring_capacity = uint64_t{1} << 60;
    header(buf)->sub_ring_mask     = (uint64_t{1} << 60) - 1;
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}

TEST_F(CorruptedHeaderTest, StampedBufferStillValidates)
{
    auto buf = make_stamped(default_cfg());
    // Sanity: an unmodified stamped buffer must pass.
    EXPECT_NO_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size));
}

TEST_F(CorruptedHeaderTest, RejectsCreatorNameLenPastTail)
{
    auto buf = make_stamped(default_cfg());
    // Within total_size but past the creator-name tail (would let info()
    // read into the subscriber rings / pool).
    header(buf)->creator_name_len =
        static_cast<uint16_t>(header(buf)->sub_rings_offset);
    EXPECT_THROW(kickmsg::SharedRegion::attach_open(buf.get(), buf.size),
                 std::runtime_error);
}


#include "kickmsg/os/Time.h"

TEST_F(RegionTest, RepairStealsProvenStaleLockAndResumedCommitFails)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;
    cfg.commit_timeout    = 1ms;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    for (int i = 0; i < 4; ++i)
    {
        uint32_t val = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        ASSERT_TRUE(sub.try_receive().has_value());
    }

    auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // Stalled holder at pos=4 (idx 0): claimed the position, locked the
    // entry, then was descheduled past commit_timeout.
    ring->write_pos.store(5, std::memory_order_release);
    uint64_t const claim_before = entries[0].meta.load(std::memory_order_acquire);
    uint64_t expected = 1;
    ASSERT_TRUE(entries[0].sequence.compare_exchange_strong(
        expected, kickmsg::seq_lock(4),
        std::memory_order_acquire, std::memory_order_relaxed));

    // The lock value is stable across the grace re-check -> repair steals.
    EXPECT_EQ(region.repair_locked_entries(), 1u);
    EXPECT_EQ(entries[0].sequence.load(std::memory_order_acquire),
              kickmsg::seq_skip(4));
    // Keep the predecessor's claim for the next publisher to release.
    EXPECT_EQ(entries[0].meta.load(std::memory_order_acquire), claim_before);

    // The resumed holder's commit CAS must fail.
    uint64_t lock_val = kickmsg::seq_lock(4);
    EXPECT_FALSE(entries[0].sequence.compare_exchange_strong(
        lock_val, 5u, std::memory_order_release, std::memory_order_relaxed));
    EXPECT_EQ(entries[0].sequence.load(std::memory_order_acquire),
              kickmsg::seq_skip(4));
    EXPECT_EQ(entries[0].meta.load(std::memory_order_acquire), claim_before);

    // Once the next wrap has taken the entry over, the stale holder's
    // take-over is refused too: its position is no longer the newest here.
    entries[0].meta.store(kickmsg::meta_pack(8, 3), std::memory_order_release);
    EXPECT_FALSE(kickmsg::meta_precedes(
        entries[0].meta.load(std::memory_order_acquire), 4));
}

TEST_F(RegionTest, RepairGraceSparesInFlightCommit)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;
    // The commit below must land inside the repairer's grace sleep.  A loaded CI runner
    // can stall a thread for tens of milliseconds, hence the 100x margin.
    cfg.commit_timeout    = 1s;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    for (int i = 0; i < 4; ++i)
    {
        uint32_t val = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        ASSERT_TRUE(sub.try_receive().has_value());
    }

    auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // Healthy holder mid-commit at pos=4.
    ring->write_pos.store(5, std::memory_order_release);
    uint64_t expected = 1;
    ASSERT_TRUE(entries[0].sequence.compare_exchange_strong(
        expected, kickmsg::seq_lock(4),
        std::memory_order_acquire, std::memory_order_relaxed));

    std::atomic<bool>        started{false};
    std::atomic<std::size_t> repaired{SIZE_MAX};
    std::thread repairer([&]
    {
        started.store(true, std::memory_order_release);
        repaired = region.repair_locked_entries();
    });

    // Commit during the grace period; repair must leave the changed entry alone.
    while (not started.load(std::memory_order_acquire))
    {
        kickmsg::yield();
    }
    kickmsg::sleep(10ms);
    entries[0].meta.store(kickmsg::meta_pack(4, kickmsg::INVALID_SLOT),
                          std::memory_order_relaxed);
    uint64_t lock_val = kickmsg::seq_lock(4);
    EXPECT_TRUE(entries[0].sequence.compare_exchange_strong(
        lock_val, 5u, std::memory_order_release, std::memory_order_relaxed));

    repairer.join();
    EXPECT_EQ(repaired.load(), 0u);
    EXPECT_EQ(entries[0].sequence.load(std::memory_order_acquire), 5u);
}

TEST_F(RegionTest, StealBacksOffWhenEntryChangesFirst)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // A repairer observed the lock, but the holder committed first.
    uint64_t const committed = kickmsg::meta_pack(4, 3);
    entries[0].meta.store(committed, std::memory_order_relaxed);
    entries[0].sequence.store(5, std::memory_order_release);

    EXPECT_FALSE(kickmsg::entry_steal_and_skip(entries[0], 4,
                                                kickmsg::seq_lock(4)));
    EXPECT_EQ(entries[0].sequence.load(std::memory_order_acquire), 5u);
    EXPECT_EQ(entries[0].meta.load(std::memory_order_acquire), committed);
}

// Handles must use their validated geometry after shared header changes.
TEST_F(RegionTest, HandlesUseAValidatedGeometrySnapshot)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    uint32_t first = 7;
    ASSERT_GE(pub.send(&first, sizeof(first)), 0);

    // Change every shared field used for pointer arithmetic.
    auto* header = region.header();
    header->pool_offset       = 1;
    header->slot_stride       = 1ULL << 40;
    header->sub_rings_offset  = 1;
    header->sub_ring_stride   = 1ULL << 40;
    header->pool_size         = UINT32_MAX;
    header->slot_data_size    = UINT64_MAX;
    header->sub_ring_capacity = 1ULL << 40;
    header->sub_ring_mask     = UINT64_MAX;
    header->max_subs          = UINT32_MAX;

    // Both handles keep working off their own copies.
    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());
    uint32_t got = 0;
    std::memcpy(&got, sample->data(), sizeof(got));
    EXPECT_EQ(got, 7u);

    uint32_t second = 9;
    ASSERT_GE(pub.send(&second, sizeof(second)), 0);
    auto view = sub.try_receive_view();
    ASSERT_TRUE(view.has_value());
    ASSERT_EQ(view->len(), sizeof(second));
    std::memcpy(&got, view->data(), sizeof(got));
    EXPECT_EQ(got, 9u);
}

TEST_F(RegionTest, VersionMismatchIsFatalAndImmediate)
{
    auto cfg = default_cfg();

    // Held open: a Windows mapping is destroyed with its last handle, and an old
    // region only exists because an old process still maps it.
    auto old_build = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    old_build.header()->version = kickmsg::VERSION - 1;

    EXPECT_THROW(kickmsg::SharedRegion::open(SHM_NAME), kickmsg::VersionMismatch);

    auto start = steady_clock::now();
    EXPECT_THROW(kickmsg::SharedRegion::create_or_open(SHM_NAME, kickmsg::channel::PubSub, cfg),
                 kickmsg::VersionMismatch);
    EXPECT_LT(steady_clock::now() - start, 500ms) << "create_or_open waited on a region that will never match";
}

TEST_F(RegionTest, RegionWalksUseTheValidatedGeometrySnapshot)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg, "my_node");
    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    uint32_t value = 7;
    ASSERT_GE(pub.send(&value, sizeof(value)), 0);

    // A peer rewrites every geometry field after validation.
    auto* header = region.header();
    header->pool_offset       = 1;
    header->slot_stride       = 1ULL << 40;
    header->sub_rings_offset  = 1;
    header->sub_ring_stride   = 1ULL << 40;
    header->pool_size         = UINT32_MAX;
    header->slot_data_size    = UINT64_MAX;
    header->sub_ring_capacity = 1ULL << 40;
    header->sub_ring_mask     = UINT64_MAX;
    header->max_subs          = UINT32_MAX;
    header->creator_name_len  = UINT16_MAX;

    auto report = region.diagnose();
    EXPECT_EQ(report.live_rings, 1u);
    EXPECT_EQ(report.locked_entries, 0u);

    auto stats = region.stats();
    EXPECT_EQ(stats.rings.size(), cfg.max_subscribers);
    EXPECT_EQ(stats.pool_size, cfg.pool_size);
    EXPECT_EQ(stats.total_writes, 1u);

    auto info = region.info();
    EXPECT_EQ(info.max_subs, cfg.max_subscribers);
    EXPECT_EQ(info.pool_size, cfg.pool_size);
    EXPECT_LE(info.creator_name.size(), region.geometry().sub_rings_offset - sizeof(kickmsg::Header));
    EXPECT_EQ(info.creator_name.rfind("my_node", 0), 0u);

    EXPECT_EQ(region.repair_locked_entries(), 0u);
    EXPECT_EQ(region.reset_retired_rings(), 0u);
    EXPECT_EQ(region.reclaim_dead_rings(), 0u);
    EXPECT_EQ(region.reclaim_orphaned_slots(), 0u);

    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());
    std::memcpy(&value, sample->data(), sizeof(value));
    EXPECT_EQ(value, 7u);
}

// Each claim owns one reference; a leaked extra reference must not pin the slot.
TEST_F(RegionTest, ReclaimOrphanedSlotsResetsOvercountedRefcounts)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    {
        kickmsg::Subscriber sub(region);
        kickmsg::Publisher  pub(region);

        uint32_t value = 1;
        ASSERT_GE(pub.send(&value, sizeof(value)), 0);

        auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), sub.ring_index());
        auto* entries = kickmsg::ring_entries(ring);
        uint32_t biased = kickmsg::meta_slot_biased(entries[0].meta.load(std::memory_order_acquire));
        ASSERT_NE(biased, 0u);
        auto* slot = kickmsg::slot_at(region.base(), region.geometry(), biased - 1);
        ASSERT_EQ(slot->refcount.load(std::memory_order_acquire), 1u);

        slot->refcount.store(5, std::memory_order_release);
        EXPECT_EQ(region.reclaim_orphaned_slots(), 0u) << "a claimed slot was freed";
        EXPECT_EQ(slot->refcount.load(std::memory_order_acquire), 1u);
    }
    EXPECT_EQ(region.stats().pool_free, cfg.pool_size) << "the leaked reference still pins the slot";
}

// A committed entry whose claim names another position must not be delivered.
TEST_F(RegionTest, ReceiveRejectsAClaimTaggedForAnotherPosition)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    uint32_t value = 1;
    ASSERT_GE(pub.send(&value, sizeof(value)), 0);

    auto*    ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), sub.ring_index());
    auto&    e       = kickmsg::ring_entries(ring)[0];
    uint64_t claimed = e.meta.load(std::memory_order_acquire);
    uint32_t slot    = kickmsg::meta_slot_biased(claimed) - 1;
    e.meta.store(kickmsg::meta_pack(4, slot), std::memory_order_release);

    EXPECT_FALSE(sub.try_receive_view().has_value());
    EXPECT_EQ(sub.lost(), 1u);

    e.meta.store(claimed, std::memory_order_release);
}

TEST_F(RegionTest, CorruptFreeStackHeadFailsAllocationInsteadOfFaulting)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Publisher pub(region);

    region.header()->free_top.store(kickmsg::tagged_pack(0, 0xfffffffe),
                                    std::memory_order_release);

    auto slot = pub.allocate();
    EXPECT_FALSE(slot.valid());

    uint32_t value = 1;
    EXPECT_EQ(pub.send(&value, sizeof(value)), -EAGAIN);
}

// Resume a stalled writer after a newer writer commits to the same entry.
TEST_F(RegionTest, StalledPublisherCannotOverwriteANewerEntry)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 1;   // every position lands on the same entry
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;
    cfg.commit_timeout    = 1us;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber sub(region);

    auto* header       = region.header();
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto& entry   = kickmsg::ring_entries(ring)[0];

    // Publisher A claims pos 0 and locks the entry, then stalls before
    // taking the metadata over.
    uint32_t a_slot = kickmsg::treiber_pop(header->free_top, region.base(), region.geometry());
    ASSERT_NE(a_slot, kickmsg::INVALID_SLOT);
    auto*    a      = kickmsg::slot_at(region.base(), region.geometry(), a_slot);
    uint32_t stale  = 111;
    std::memcpy(kickmsg::slot_data(a), &stale, sizeof(stale));
    a->payload_len.store(sizeof(stale), std::memory_order_relaxed);
    a->refcount.store(1, std::memory_order_release);
    ring->state_flight.fetch_add(kickmsg::ring::IN_FLIGHT_ONE, std::memory_order_acq_rel);
    uint64_t pos      = ring->write_pos.fetch_add(1, std::memory_order_acq_rel);
    uint64_t expected = 0;
    ASSERT_TRUE(entry.sequence.compare_exchange_strong(
        expected, kickmsg::seq_lock(pos),
        std::memory_order_acquire, std::memory_order_relaxed));

    // A repairer steals the stalled lock, then publisher B commits the next
    // generation over the same entry.
    EXPECT_EQ(region.repair_locked_entries(), 1u);
    kickmsg::Publisher b(region);
    uint32_t fresh = 222;
    ASSERT_GE(b.send(&fresh, sizeof(fresh)), 0);

    // A resumes and runs the take-over from Publisher::publish().
    uint64_t const my_meta  = kickmsg::meta_pack(pos, a_slot);
    uint64_t       old_meta = entry.meta.load(std::memory_order_acquire);
    bool           taken    = false;
    while (kickmsg::meta_precedes(old_meta, pos))
    {
        if (entry.meta.compare_exchange_weak(old_meta, my_meta,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            taken = true;
            break;
        }
    }
    EXPECT_FALSE(taken);

    // B's message is delivered intact.
    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());
    uint32_t got = 0;
    std::memcpy(&got, sample->data(), sizeof(got));
    EXPECT_EQ(got, 222u);

    ring->state_flight.fetch_sub(kickmsg::ring::IN_FLIGHT_ONE, std::memory_order_release);
}

// Repair must retain the predecessor's claim until its reference is released.
TEST_F(RegionTest, StealKeepsThePredecessorReleasable)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 1;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;
    cfg.commit_timeout    = 1us;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    auto* ring = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    auto& entry = kickmsg::ring_entries(ring)[0];

    {
        kickmsg::Subscriber sub(region);
        kickmsg::Publisher  pub(region);
        uint32_t            value = 42;
        ASSERT_GE(pub.send(&value, sizeof(value)), 0);

        // The published slot is the entry's claim.
        uint32_t claimed = kickmsg::meta_slot_biased(
            entry.meta.load(std::memory_order_acquire));
        ASSERT_NE(claimed, 0u);

        // A publisher claims pos 1 and stalls before its take-over.
        ring->state_flight.fetch_add(kickmsg::ring::IN_FLIGHT_ONE,
                                     std::memory_order_acq_rel);
        uint64_t pos      = ring->write_pos.fetch_add(1, std::memory_order_acq_rel);
        uint64_t expected = 1;
        ASSERT_TRUE(entry.sequence.compare_exchange_strong(
            expected, kickmsg::seq_lock(pos),
            std::memory_order_acquire, std::memory_order_relaxed));

        EXPECT_EQ(region.repair_locked_entries(), 1u);

        // The steal left the claim intact, so the predecessor is still
        // reachable for whoever takes the entry over next.
        EXPECT_EQ(kickmsg::meta_slot_biased(entry.meta.load(std::memory_order_acquire)),
                  claimed);

        ring->state_flight.fetch_sub(kickmsg::ring::IN_FLIGHT_ONE,
                                     std::memory_order_release);
    }

    // Without a crash, all slots must return without orphan recovery.
    EXPECT_EQ(region.stats().pool_free, cfg.pool_size);
    EXPECT_EQ(region.reclaim_orphaned_slots(), 0u);
}
