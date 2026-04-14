#include <gtest/gtest.h>

#include "kickmsg/Region.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

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
    auto* hdr   = region.header();

    EXPECT_EQ(hdr->magic, kickmsg::MAGIC);
    EXPECT_EQ(hdr->version, kickmsg::VERSION);
    EXPECT_EQ(hdr->channel_type, kickmsg::channel::PubSub);
    EXPECT_EQ(hdr->max_subs, cfg.max_subscribers);
    EXPECT_EQ(hdr->sub_ring_capacity, cfg.sub_ring_capacity);
    EXPECT_EQ(hdr->sub_ring_mask, cfg.sub_ring_capacity - 1);
    EXPECT_EQ(hdr->pool_size, cfg.pool_size);
    EXPECT_EQ(hdr->slot_data_size, cfg.max_payload_size);
    EXPECT_EQ(hdr->creator_name_len, 4u);

    std::string creator(kickmsg::header_creator_name(hdr), hdr->creator_name_len);
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

TEST_F(RegionTest, HeaderStoresCreatorMetadata)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg, "my_node");
    auto* hdr   = region.header();

    EXPECT_EQ(hdr->creator_pid, static_cast<uint64_t>(getpid()));
    EXPECT_GT(hdr->created_at_ns, 0u);
    EXPECT_NE(hdr->config_hash, 0u);
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
    auto* hdr   = region.header();

    std::vector<uint32_t> popped;
    for (uint32_t i = 0; i < cfg.pool_size; ++i)
    {
        uint32_t idx = kickmsg::treiber_pop(hdr->free_top, base, hdr);
        ASSERT_NE(idx, kickmsg::INVALID_SLOT) << "Pop failed at iteration " << i;
        popped.push_back(idx);
    }

    EXPECT_EQ(kickmsg::treiber_pop(hdr->free_top, base, hdr), kickmsg::INVALID_SLOT);

    for (auto idx : popped)
    {
        auto* slot = kickmsg::slot_at(base, hdr, idx);
        kickmsg::treiber_push(hdr->free_top, slot, idx);
    }

    uint32_t count = 0;
    uint32_t top = kickmsg::tagged_idx(hdr->free_top.load(std::memory_order_acquire));
    while (top != kickmsg::INVALID_SLOT)
    {
        auto* slot = kickmsg::slot_at(base, hdr, top);
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
    auto* hdr = region.header();

    auto count_free = [&]()
    {
        uint32_t count = 0;
        uint64_t top = hdr->free_top.load(std::memory_order_acquire);
        uint32_t idx = kickmsg::tagged_idx(top);
        while (idx != kickmsg::INVALID_SLOT)
        {
            auto* slot = kickmsg::slot_at(region.base(), hdr, idx);
            idx = slot->next_free;
            ++count;
        }
        return count;
    };

    EXPECT_EQ(count_free(), 16u);

    for (int i = 0; i < 3; ++i)
    {
        uint32_t idx = kickmsg::treiber_pop(hdr->free_top, region.base(), hdr);
        ASSERT_NE(idx, kickmsg::INVALID_SLOT);
        auto* slot = kickmsg::slot_at(region.base(), hdr, idx);
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
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.header(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // Advance write_pos to simulate that a publisher claimed pos=1
    ring->write_pos.store(2, std::memory_order_release);
    auto& e1 = entries[1]; // pos=1 → idx=1
    e1.sequence.store(kickmsg::LOCKED_SEQUENCE, std::memory_order_release);

    // Repair should fix the locked entry
    std::size_t repaired = region.repair_locked_entries();
    EXPECT_EQ(repaired, 1u);

    // The repaired entry should have seq = pos + 1 = 2
    uint64_t seq = e1.sequence.load(std::memory_order_acquire);
    EXPECT_EQ(seq, 2u);

    // The repaired entry should have INVALID_SLOT
    uint32_t slot_idx = e1.slot_idx.load(std::memory_order_acquire);
    EXPECT_EQ(slot_idx, kickmsg::INVALID_SLOT);

    // Now publish enough to wrap around: pos 2, 3, 4, 5
    // pos=4 wraps to idx=0 and expects prev_seq=1 (pos 0's committed seq) — OK
    // pos=5 wraps to idx=1 and expects prev_seq=2 (the repaired seq) — this
    // would fail with the old code that stored prev_seq instead of pos+1
    for (int i = 0; i < 4; ++i)
    {
        val = static_cast<uint32_t>(200 + i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0)
            << "Publishing failed at iteration " << i
            << " — repaired entry likely blocked the ring";
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
    // Case B: publisher claimed write_pos (fetch_add) but crashed before
    // CAS-locking the entry.  The entry still has the committed sequence
    // from the previous wrap.  After more than one full wrap, the entry
    // is detectably stale (> 1 ring revolution behind) and
    // repair_locked_entries() should advance it.

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
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.header(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // Simulate: a publisher claimed pos=4 (fetch_add) targeting idx=0,
    // then crashed before the CAS lock.  The entry stays at seq=1.
    // Advance write_pos past pos=4 by TWO more full wraps so the entry
    // becomes > 1 wrap stale.
    // write_pos after the 4 real publishes is 4.  Set it to 4 + 2*cap = 12.
    ring->write_pos.store(12, std::memory_order_release);
    // Don't touch entries — they keep their old sequences.  Entry idx=0
    // has seq=1, but expected seq at pos=8 (the slot in the scan window)
    // is 9.  (pos=8 maps to idx=0 because 8 & 3 = 0.)  1 + 4 < 9 → stale.

    auto report = region.diagnose();
    EXPECT_GT(report.locked_entries, 0u)
        << "diagnose() should detect the stale entry (Case B)";

    std::size_t repaired = region.repair_locked_entries();
    EXPECT_GT(repaired, 0u)
        << "repair_locked_entries() should advance the stale entry";

    // After repair, the entry at idx=0 should have seq = expected.
    // The expected pos for idx=0 in the window [12-4, 12) = [8, 12) is pos=8.
    uint64_t seq0 = entries[0].sequence.load(std::memory_order_acquire);
    EXPECT_EQ(seq0, 9u)  // pos=8 → expected = 8 + 1 = 9
        << "Stale entry should be advanced to pos + 1";

    // Publishing should now succeed past the repaired slot.
    for (int i = 0; i < 8; ++i)
    {
        uint32_t val = static_cast<uint32_t>(100 + i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0)
            << "Publishing failed at iteration " << i
            << " — repaired entry may still be stuck";
    }
}

TEST_F(RegionTest, RepairLockedEntryAtPositionZero)
{
    // Edge case: crash at pos=0 where prev_seq was 0.
    // Old code stored prev_seq=0, new code stores pos+1=1.

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);

    // Simulate crash at pos=0: lock the entry, advance write_pos
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.header(), 0);
    auto* entries = kickmsg::ring_entries(ring);
    ring->write_pos.store(1, std::memory_order_release);
    entries[0].sequence.store(kickmsg::LOCKED_SEQUENCE, std::memory_order_release);

    std::size_t repaired = region.repair_locked_entries();
    EXPECT_EQ(repaired, 1u);
    EXPECT_EQ(entries[0].sequence.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(entries[0].slot_idx.load(std::memory_order_acquire), kickmsg::INVALID_SLOT);

    // Publishing should work: pos=1,2,3 use fresh indices, pos=4 wraps to idx=0
    // and expects prev_seq=1 — matches the repaired value
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
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.header(), 0);
    auto* entries = kickmsg::ring_entries(ring);
    ring->write_pos.store(2, std::memory_order_release);
    entries[1].sequence.store(kickmsg::LOCKED_SEQUENCE, std::memory_order_release);

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
    auto* ring = kickmsg::sub_ring_at(region.base(), region.header(), 0);
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

    // Ring 0: retired (Free | in_flight=1) — should be reset
    auto* ring0 = kickmsg::sub_ring_at(region.base(), region.header(), 0);
    ring0->state_flight.store(
        kickmsg::ring::make_packed(kickmsg::ring::Free, 1),
        std::memory_order_release);

    // Ring 1: draining (Draining | in_flight=1) — must NOT be touched
    auto* ring1 = kickmsg::sub_ring_at(region.base(), region.header(), 1);
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

    // Second process tries to claim a *different* schema — library just
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
    // Invariant: schema() must return nullopt unless state == Set.
    // We force the Claiming state directly (the real transition is too
    // brief to observe deterministically from another thread) and confirm
    // readers don't see torn payload bytes.
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
    // Crash scenario: a claimant CAS'd Unset → Claiming and died before
    // the release-store of Set.  Every try_claim_schema() caller will
    // observe Claiming and return false after bounded yields.
    // reset_schema_claim() is the operator-driven recovery.
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
    // Second call is a no-op — state is already Unset.
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
    // Separation of concerns, open-branch path: if the creator leaves
    // schema unset and a later opener passes cfg.schema, that schema is
    // silently ignored (use try_claim_schema to publish it instead).
    auto cfg = default_cfg();
    // Note: cfg.schema intentionally left empty.
    auto existing = kickmsg::SharedRegion::create(
                        SHM_NAME, kickmsg::channel::PubSub, cfg, "creator");
    ASSERT_FALSE(existing.schema().has_value());

    auto opener_cfg = default_cfg();
    opener_cfg.schema = make_schema("opener/Type", 1, 0x11, 0x22);

    auto opened = kickmsg::SharedRegion::create_or_open(
                      SHM_NAME, kickmsg::channel::PubSub, opener_cfg, "opener");

    // Opener's cfg.schema was discarded — slot is still Unset.
    EXPECT_FALSE(opened.schema().has_value());
}

TEST_F(RegionTest, SchemaCrossHandleObservesClaim)
{
    // Mirror the real cross-process flow: one SharedRegion handle claims,
    // a second SharedRegion handle opened against the same SHM observes
    // the claim.  This exercises the acquire-load in schema() across
    // independent mapping handles, not just within a single object.
    auto cfg = default_cfg();
    auto r1  = kickmsg::SharedRegion::create(
                   SHM_NAME, kickmsg::channel::PubSub, cfg);

    auto r2 = kickmsg::SharedRegion::open(SHM_NAME);

    // Initially both see Unset.
    EXPECT_FALSE(r1.schema().has_value());
    EXPECT_FALSE(r2.schema().has_value());

    // r1 claims.  r2 must observe Set without any further fence on its
    // side — acquire-load in schema() synchronizes with r1's release-store.
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
    // Mirror the cross-process crash-recovery flow: one handle wedges
    // (simulated claimant crashed mid-claim), a second handle opened
    // against the same SHM calls reset_schema_claim().
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
    // Wedged Claiming must surface via HealthReport alongside the other
    // crash-residue indicators so supervisors can detect it on a
    // routine health-check loop.
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
    // Separation of concerns: schema presence is orthogonal to channel
    // geometry, so create_or_open() from a different Config::schema must
    // NOT trip the config mismatch check.
    auto cfg = default_cfg();
    cfg.schema = make_schema("creator/Type", 1, 0xAA, 0xBB);

    auto existing = kickmsg::SharedRegion::create(
                        SHM_NAME, kickmsg::channel::PubSub, cfg, "creator");

    auto other_cfg = default_cfg();
    other_cfg.schema = make_schema("opener/Type", 2, 0xCC, 0xDD);

    // Must succeed — geometry matches, schema differs but is ignored on open.
    auto opened = kickmsg::SharedRegion::create_or_open(
                      SHM_NAME, kickmsg::channel::PubSub, other_cfg, "opener");

    // Opener observes the creator's schema, not its own — library doesn't
    // overwrite or enforce anything.
    auto got = opened.schema();
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "creator/Type");
}
