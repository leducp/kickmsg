#include <gtest/gtest.h>

#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

#include <cstring>

class PublisherTest : public ::testing::Test
{
public:
    static constexpr char const* SHM_NAME = "/kickmsg_test_publisher";

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

TEST_F(PublisherTest, SendReceiveSingleMessage)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    uint64_t payload = 0xDEADBEEFCAFEBABEULL;
    ASSERT_GE(pub.send(&payload, sizeof(payload)), 0);

    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->len(), sizeof(payload));

    uint64_t received = 0;
    std::memcpy(&received, sample->data(), sizeof(received));
    EXPECT_EQ(received, payload);
}

TEST_F(PublisherTest, AllocatePublishSeparately)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    auto* ptr = pub.allocate(sizeof(uint32_t));
    ASSERT_NE(ptr, nullptr);

    uint32_t val = 42;
    std::memcpy(ptr, &val, sizeof(val));
    std::size_t delivered = pub.publish();
    EXPECT_EQ(delivered, 1u);

    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());

    uint32_t got = 0;
    std::memcpy(&got, sample->data(), sizeof(got));
    EXPECT_EQ(got, 42u);
}

TEST_F(PublisherTest, AllocateTooLargeReturnsNull)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Publisher pub(region);

    EXPECT_EQ(pub.allocate(cfg.max_payload_size + 1), nullptr);
}

TEST_F(PublisherTest, SendReturnsEmsgsize)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Publisher pub(region);

    uint8_t buf[128]{};
    EXPECT_EQ(pub.send(buf, cfg.max_payload_size + 1), -EMSGSIZE);
}

TEST_F(PublisherTest, SendReturnsEagainOnPoolExhaustion)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 2;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    uint32_t val = 1;
    // Fill the pool (2 slots)
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    // Pool exhausted
    EXPECT_EQ(pub.send(&val, sizeof(val)), -EAGAIN);
}

TEST_F(PublisherTest, DroppedCounterZeroInNormalUse)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    for (int i = 0; i < 5; ++i)
    {
        ASSERT_GE(pub.send(&i, sizeof(i)), 0);
    }

    EXPECT_EQ(pub.dropped(), 0u);
}

TEST_F(PublisherTest, MultipleMessages)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    constexpr int N = 5;
    for (int i = 0; i < N; ++i)
    {
        ASSERT_GE(pub.send(&i, sizeof(i)), 0);
    }

    for (int i = 0; i < N; ++i)
    {
        auto sample = sub.try_receive();
        ASSERT_TRUE(sample.has_value()) << "Missing message at i=" << i;

        int got = 0;
        std::memcpy(&got, sample->data(), sizeof(got));
        EXPECT_EQ(got, i);
    }
}

TEST_F(PublisherTest, MultipleSubscribersEachReceive)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub1(region);
    kickmsg::Subscriber sub2(region);
    kickmsg::Publisher   pub(region);

    uint32_t val = 7;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    auto s1 = sub1.try_receive();
    auto s2 = sub2.try_receive();

    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s2.has_value());

    uint32_t r1 = 0, r2 = 0;
    std::memcpy(&r1, s1->data(), sizeof(r1));
    std::memcpy(&r2, s2->data(), sizeof(r2));

    EXPECT_EQ(r1, 7u);
    EXPECT_EQ(r2, 7u);
}

// ---------------------------------------------------------------------------
// Publisher self-repair: when a publisher hits a stuck entry (timeout +
// CAS lock failure), it heals the entry in place so the next publisher
// at that position succeeds without paying the timeout.
// ---------------------------------------------------------------------------

TEST_F(PublisherTest, SelfRepairCaseA_LockedSequence)
{
    // Simulate Case A: a publisher CAS-locked an entry (LOCKED_SEQUENCE)
    // then crashed before committing.  The next publisher that wraps to
    // this position should:
    //   1. time out in wait_and_capture_slot
    //   2. fail the CAS lock (entry is LOCKED_SEQUENCE, not prev_seq)
    //   3. self-repair the entry (advance seq to expected)
    //   4. drop delivery for this wrap
    // After that, the NEXT publisher at this position should succeed
    // without any timeout.

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;    // capacity = 4
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 8;
    cfg.commit_timeout    = std::chrono::microseconds{1000};  // 1 ms — fast test

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    // Publish 4 messages (pos 0-3), consume all.
    for (int i = 0; i < 4; ++i)
    {
        uint32_t val = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        auto s = sub.try_receive();
        ASSERT_TRUE(s.has_value());
    }

    // Poison entry at idx=0: simulate a publisher that locked pos=4 then
    // crashed.  write_pos is already 4 (from the 4 publishes above).
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.header(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // Advance write_pos past pos=4 so the ring has wrapped.
    ring->write_pos.store(5, std::memory_order_release);
    // Lock entry at idx=0 as if a publisher crashed mid-commit at pos=4.
    entries[0].sequence.store(kickmsg::LOCKED_SEQUENCE,
                              std::memory_order_release);

    // Next publish: pos=5 → idx=1 (clean entry, succeeds).
    uint32_t val = 100;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    // pos=6 → idx=2 (clean), pos=7 → idx=3 (clean).
    val = 101;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    val = 102;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    // pos=8 → idx=0 → POISONED.  Publisher times out + drops + self-repairs.
    auto before = std::chrono::steady_clock::now();
    val = 103;
    pub.send(&val, sizeof(val));  // may drop, but should NOT hang forever
    auto after = std::chrono::steady_clock::now();

    // Sanity: the timeout should have fired (took ~1 ms, not zero).
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          after - before).count();
    EXPECT_GE(elapsed_us, 500)
        << "Publisher should have waited ~1 ms for the stuck entry";

    // After self-repair, entry at idx=0 should be advanced.
    // The entry was stuck at LOCKED_SEQUENCE for pos=4.  The publisher at
    // pos=8 expects seq=5 and should have repaired it to 9 (pos=8 + 1).
    uint64_t seq0 = entries[0].sequence.load(std::memory_order_acquire);
    EXPECT_NE(seq0, kickmsg::LOCKED_SEQUENCE)
        << "Self-repair should have advanced the stuck entry";

    // Now the NEXT publish at idx=0 (pos=12) should succeed WITHOUT timeout.
    // First, fill positions 9, 10, 11 (idx 1, 2, 3).
    for (int i = 0; i < 3; ++i)
    {
        val = static_cast<uint32_t>(200 + i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    }

    // pos=12 → idx=0.  If self-repair worked, this should be fast.
    before = std::chrono::steady_clock::now();
    val = 203;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    after = std::chrono::steady_clock::now();

    elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     after - before).count();
    EXPECT_LT(elapsed_us, 500)
        << "After self-repair, the next publisher at this position should "
           "succeed instantly — not wait for another timeout";
}

TEST_F(PublisherTest, SelfRepairCaseB_StaleEntry)
{
    // Simulate Case B: a publisher claimed write_pos (fetch_add) but
    // crashed before CAS-locking the entry.  The entry still has the
    // old committed sequence from a previous wrap.  After > 1 wrap, the
    // publishing publisher should self-repair the stale entry.

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 8;
    cfg.commit_timeout    = std::chrono::microseconds{1000};

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    // Publish 4 messages (pos 0-3), consume all.
    for (int i = 0; i < 4; ++i)
    {
        uint32_t val = static_cast<uint32_t>(i);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        auto s = sub.try_receive();
        ASSERT_TRUE(s.has_value());
    }

    // Entry at idx=0 has seq=1 (committed for pos=0).
    auto* ring    = kickmsg::sub_ring_at(region.base(), region.header(), 0);
    auto* entries = kickmsg::ring_entries(ring);

    // Simulate: publisher claimed pos=4 (fetch_add) but crashed before
    // touching the entry.  Advance write_pos by TWO full wraps so the
    // entry at idx=0 is > 1 wrap stale.
    ring->write_pos.store(12, std::memory_order_release);

    // Entry idx=0 still has seq=1.  A publisher at pos=8 (idx=0) expects
    // seq=5.  seq=1 + cap=4 = 5 which is NOT < 5, so it needs to be
    // strictly more than one wrap.  At pos=12, expected=9.  1+4=5 < 9 → stale.

    // Publish at pos=12 → idx=0: should timeout + self-repair + drop.
    uint32_t val = 999;
    pub.send(&val, sizeof(val));  // drops at idx=0 but self-repairs

    // Verify the entry was repaired.
    uint64_t seq0 = entries[0].sequence.load(std::memory_order_acquire);
    EXPECT_GT(seq0, 1u)
        << "Self-repair should have advanced the stale entry past seq=1";

    // Next publisher at this position should succeed quickly.
    // Advance write_pos to 16 so pos=16 targets idx=0.
    // But we need to fill pos 13, 14, 15 first (idx 1, 2, 3).
    for (int i = 0; i < 3; ++i)
    {
        val = static_cast<uint32_t>(300 + i);
        pub.send(&val, sizeof(val));
    }

    auto before = std::chrono::steady_clock::now();
    val = 303;
    pub.send(&val, sizeof(val));  // pos=16 → idx=0, should be fast
    auto after = std::chrono::steady_clock::now();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          after - before).count();
    EXPECT_LT(elapsed_us, 500)
        << "After self-repair of stale entry, next publisher should succeed "
           "without timeout";
}
