
#include <map>
#include <thread>

#include <gtest/gtest.h>

#include "kickmsg/Blackboard.h"
#include "kickmsg/Hash.h"
#include "kickmsg/os/Process.h"

using namespace kickmsg;

namespace
{
    // Far above every platform's pid_max, so process_exists() reports false.
    constexpr uint64_t DEAD_PID = 0x7FFFFFFFull;

    struct Sample
    {
        uint32_t id;
        uint32_t state;
    };

    BlackboardEntry* entry(Blackboard& bb, uint32_t index)
    {
        auto* h = bb.header();
        return bb_entry_at(static_cast<void*>(h), h, index);
    }
}

class BlackboardTest : public ::testing::Test
{
protected:
    static constexpr char const* NS   = "bbtest";
    static constexpr char const* NAME = "board";

    void SetUp()    override { Blackboard::unlink(NS, NAME); }
    void TearDown() override { Blackboard::unlink(NS, NAME); }

    blackboard::Config small_cfg()
    {
        blackboard::Config cfg;
        cfg.capacity       = 8;
        cfg.max_value_size = 64;
        return cfg;
    }

    Blackboard open()
    {
        return Blackboard::open_or_create(NS, NAME, small_cfg());
    }

    /// Stamp a dead owner onto an entry so the takeover and sweep paths can
    /// be exercised without forking.  The tenancy bump is what makes the
    /// still-live Writer handle stale, standing in for the owning process
    /// having gone away without running its destructor.
    static void orphan(Blackboard& bb, char const* key)
    {
        for (uint32_t i = 0; i < bb.header()->capacity; ++i)
        {
            auto* e = entry(bb, i);
            if (e->state.load(std::memory_order_acquire) != blackboard::Active)
            {
                continue;
            }
            if (::strnlen(e->key, blackboard::KEY_MAX) != std::strlen(key)
                or std::memcmp(e->key, key, std::strlen(key)) != 0)
            {
                continue;
            }
            e->owner_starttime.store(1, std::memory_order_relaxed);
            e->owner_pid.store(DEAD_PID, std::memory_order_release);
            e->tenancy.fetch_add(1, std::memory_order_release);
            return;
        }
        FAIL() << "no active entry for key " << key;
    }
};

// ---- open / lifecycle ----------------------------------------------------

TEST_F(BlackboardTest, OpenOrCreateIsIdempotent)
{
    auto a = open();
    auto b = open();
    EXPECT_EQ(a.name(), b.name());
    EXPECT_EQ(a.capacity(), 8u);
    EXPECT_GE(a.max_value_size(), 64u);
}

TEST_F(BlackboardTest, TryOpenReturnsNulloptWhenAbsent)
{
    EXPECT_FALSE(Blackboard::try_open(NS, NAME).has_value());
}

TEST_F(BlackboardTest, TryOpenDoesNotCreateRegion)
{
    EXPECT_FALSE(Blackboard::try_open(NS, NAME).has_value());
    EXPECT_FALSE(Blackboard::try_open(NS, NAME).has_value());
}

TEST_F(BlackboardTest, MoveLeavesSourceWithNullHeader)
{
    auto a = open();
    auto b = std::move(a);
    EXPECT_EQ(a.header(), nullptr);
    EXPECT_EQ(b.capacity(), 8u);
}

// ---- the headline: a late reader sees the current value ------------------

TEST_F(BlackboardTest, LateReaderSeesCurrentValue)
{
    auto bb = open();
    auto w  = bb.declare("arm/state");
    ASSERT_TRUE(w.write(Sample{7, 3}));

    // The reader attaches only now and never waits for a second write.
    auto other  = open();
    auto reader = other.observe("arm/state");

    Sample got{};
    auto   out = reader.read(got);
    EXPECT_EQ(out.status, blackboard::Ok);
    EXPECT_EQ(out.len, sizeof(Sample));
    EXPECT_EQ(out.update_count, 1u);
    EXPECT_EQ(got.id, 7u);
    EXPECT_EQ(got.state, 3u);
}

TEST_F(BlackboardTest, ObserveBeforeDeclareResolvesLazily)
{
    auto bb     = open();
    auto reader = bb.observe("late/key");

    Sample got{};
    EXPECT_EQ(reader.read(got).status, blackboard::Missing);

    auto w = bb.declare("late/key");
    ASSERT_TRUE(w.write(Sample{1, 2}));

    // Same Reader object, no re-observe.
    auto out = reader.read(got);
    EXPECT_EQ(out.status, blackboard::Ok);
    EXPECT_EQ(got.id, 1u);
}

TEST_F(BlackboardTest, DeclaredButUnwrittenKeyReadsUnset)
{
    auto bb = open();
    auto w  = bb.declare("no/value");
    auto r  = bb.observe("no/value");

    Sample got{};
    auto   out = r.read(got);
    EXPECT_EQ(out.status, blackboard::Unset);
    EXPECT_EQ(out.len, 0u);
    EXPECT_EQ(out.update_count, 0u);
}

// ---- ownership -----------------------------------------------------------

TEST_F(BlackboardTest, DeclareTwiceFromLiveOwnerThrows)
{
    auto bb = open();
    auto w  = bb.declare("owned");
    EXPECT_THROW(bb.declare("owned"), std::runtime_error);
}

TEST_F(BlackboardTest, RedeclareAfterWriterDestructionSucceeds)
{
    auto bb = open();
    {
        auto w = bb.declare("cycle");
        ASSERT_TRUE(w.write(Sample{5, 5}));
    }
    auto w2 = bb.declare("cycle");
    EXPECT_TRUE(w2.valid());
}

TEST_F(BlackboardTest, ReleasedKeyKeepsItsValue)
{
    auto bb = open();
    auto r  = bb.observe("released");
    {
        auto w = bb.declare("released");
        ASSERT_TRUE(w.write(Sample{9, 1}));
    }

    Sample got{};
    auto   out = r.read(got);
    EXPECT_EQ(out.status, blackboard::Ok);
    EXPECT_EQ(got.id, 9u);
    EXPECT_FALSE(r.owner_alive());

    auto snap = bb.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].owner_pid, 0u);
    EXPECT_FALSE(snap[0].owner_alive);
}

TEST_F(BlackboardTest, TakeoverPreservesPriorValueUntilFirstWrite)
{
    auto bb = open();
    {
        auto w = bb.declare("arm/state");
        ASSERT_TRUE(w.write(Sample{11, 4}));
        orphan(bb, "arm/state");
    }

    auto w2 = bb.declare("arm/state");
    auto r  = bb.observe("arm/state");

    Sample got{};
    auto   out = r.read(got);
    EXPECT_EQ(out.status, blackboard::Ok);
    EXPECT_EQ(got.id, 11u);
    EXPECT_EQ(out.update_count, 1u);

    // The counter continues rather than rewinding.
    ASSERT_TRUE(w2.write(Sample{12, 5}));
    out = r.read(got);
    EXPECT_EQ(out.status, blackboard::Ok);
    EXPECT_EQ(got.id, 12u);
    EXPECT_EQ(out.update_count, 2u);
}

TEST_F(BlackboardTest, SweepStaleLeavesLiveOwnersAlone)
{
    auto bb = open();
    auto w  = bb.declare("live");
    ASSERT_TRUE(w.write(Sample{1, 1}));
    EXPECT_EQ(bb.sweep_stale(), 0u);
    EXPECT_EQ(bb.snapshot().size(), 1u);
}

TEST_F(BlackboardTest, SweepStaleFreesDeadOwnerAndLeavesUnownedAlone)
{
    auto bb = open();
    {
        auto dead = bb.declare("dead");
        ASSERT_TRUE(dead.write(Sample{1, 1}));
        orphan(bb, "dead");
    }
    {
        auto released = bb.declare("released");
        ASSERT_TRUE(released.write(Sample{2, 2}));
    }

    EXPECT_EQ(bb.sweep_stale(), 1u);
    auto snap = bb.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].key, "released");
}

// ---- freshness / diagnostics --------------------------------------------

TEST_F(BlackboardTest, UpdateCountIncrementsAndTimestampAdvances)
{
    auto bb = open();
    auto w  = bb.declare("tick");
    auto r  = bb.observe("tick");

    ASSERT_TRUE(w.write(Sample{1, 1}));
    Sample got{};
    auto   first = r.read(got);
    ASSERT_EQ(first.status, blackboard::Ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(w.write(Sample{2, 2}));
    auto second = r.read(got);
    ASSERT_EQ(second.status, blackboard::Ok);

    EXPECT_EQ(first.update_count, 1u);
    EXPECT_EQ(second.update_count, 2u);
    EXPECT_GT(second.updated_at_ns, first.updated_at_ns);
}

TEST_F(BlackboardTest, SnapshotReportsOwnerAndSkipsFreeSlots)
{
    auto bb = open();
    auto a  = bb.declare("a", "node_a");
    auto b  = bb.declare("b", "node_b");
    ASSERT_TRUE(a.write(Sample{1, 1}));

    auto snap = bb.snapshot();
    ASSERT_EQ(snap.size(), 2u);
    for (auto const& ks : snap)
    {
        EXPECT_EQ(ks.owner_pid, current_pid());
        EXPECT_TRUE(ks.owner_alive);
        if (ks.key == "a")
        {
            EXPECT_EQ(ks.owner_node, "node_a");
            EXPECT_EQ(ks.value_len, sizeof(Sample));
            EXPECT_EQ(ks.update_count, 1u);
        }
        else
        {
            EXPECT_EQ(ks.update_count, 0u);
        }
    }
}

// ---- change notification -------------------------------------------------

TEST_F(BlackboardTest, WaitWakesOnAnyChange)
{
    auto bb = open();
    auto a  = bb.declare("a");
    auto b  = bb.declare("b");

    uint64_t seq = bb.change_seq();
    std::thread writer([&]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        b.write(Sample{1, 1});
    });

    // Waiting on the board, not on key "a": any value change wakes us.
    EXPECT_TRUE(bb.wait(seq, std::chrono::seconds(5)));
    writer.join();
    EXPECT_NE(bb.change_seq(), seq);
}

TEST_F(BlackboardTest, WaitReturnsFalseOnTimeout)
{
    auto bb = open();
    auto w  = bb.declare("quiet");

    uint64_t seq   = bb.change_seq();
    auto     start = std::chrono::steady_clock::now();
    EXPECT_FALSE(bb.wait(seq, std::chrono::milliseconds(50)));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(45));
}

TEST_F(BlackboardTest, WaitReturnsImmediatelyWhenSeqAlreadyAdvanced)
{
    auto     bb    = open();
    auto     w     = bb.declare("k");
    uint64_t stale = bb.change_seq();
    ASSERT_TRUE(w.write(Sample{1, 1}));

    auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(bb.wait(stale, std::chrono::seconds(5)));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

// ---- bounds and hostile bytes -------------------------------------------

TEST_F(BlackboardTest, ValueTooLargeIsRejectedAndPreservesPrevious)
{
    auto bb = open();
    auto w  = bb.declare("k");
    auto r  = bb.observe("k");
    ASSERT_TRUE(w.write(Sample{3, 3}));

    std::vector<uint8_t> huge(4096, 0xAB);
    EXPECT_FALSE(w.write(huge.data(), huge.size()));

    Sample got{};
    auto   out = r.read(got);
    EXPECT_EQ(out.status, blackboard::Ok);
    EXPECT_EQ(out.update_count, 1u);
    EXPECT_EQ(got.id, 3u);
}

TEST_F(BlackboardTest, ReadIntoSmallBufferReportsTruncated)
{
    auto bb = open();
    auto w  = bb.declare("k");
    auto r  = bb.observe("k");

    std::vector<uint8_t> payload(32, 0x5A);
    ASSERT_TRUE(w.write(payload.data(), payload.size()));

    uint8_t guarded[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    auto    out = r.read(guarded, 4);
    EXPECT_EQ(out.status, blackboard::Truncated);
    EXPECT_EQ(out.len, 32u);
    for (auto byte : guarded)
    {
        EXPECT_EQ(byte, 0);
    }
}

TEST_F(BlackboardTest, EmptyOrOverlongKeyThrows)
{
    auto bb = open();
    EXPECT_THROW(bb.declare(""), std::invalid_argument);
    EXPECT_THROW(bb.observe(""), std::invalid_argument);

    std::string huge(blackboard::KEY_MAX + 10, 'x');
    EXPECT_THROW(bb.declare(huge.c_str()), std::invalid_argument);
    EXPECT_THROW(bb.observe(huge.c_str()), std::invalid_argument);
}

TEST_F(BlackboardTest, CapacityExhaustionThrows)
{
    blackboard::Config cfg;
    cfg.capacity       = 2;
    cfg.max_value_size = 32;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg);

    auto a = bb.declare("a");
    auto b = bb.declare("b");
    EXPECT_THROW(bb.declare("c"), std::runtime_error);
}

TEST_F(BlackboardTest, CorruptValueLenIsClamped)
{
    auto bb = open();
    auto w  = bb.declare("k");
    auto r  = bb.observe("k");
    ASSERT_TRUE(w.write(Sample{1, 1}));

    auto* h    = bb.header();
    auto* cell = bb_cell_at(static_cast<void*>(h), h, 0, 1);
    cell->value_len.store(0xFFFFFFFFu, std::memory_order_relaxed);

    std::vector<uint8_t> out;
    auto result = r.read(out);
    EXPECT_EQ(result.status, blackboard::Ok);
    EXPECT_LE(result.len, bb.max_value_size());
}

TEST_F(BlackboardTest, CorruptKeyBytesAreNotOverread)
{
    auto bb = open();
    auto w  = bb.declare("k");

    auto* e = entry(bb, 0);
    std::memset(e->key, 'x', sizeof(e->key));   // no NUL anywhere

    auto snap = bb.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].key.size(), blackboard::KEY_MAX);
}

TEST_F(BlackboardTest, RejectsVersionMismatch)
{
    auto bb = open();
    bb.header()->version = blackboard::VERSION + 1;
    EXPECT_THROW(Blackboard::try_open(NS, NAME), std::runtime_error);
    bb.header()->version = blackboard::VERSION;
}

TEST_F(BlackboardTest, RejectsCapacityOverflowingRegion)
{
    auto     bb    = open();
    uint32_t saved = bb.header()->capacity;
    bb.header()->capacity = blackboard::MAX_CAPACITY;
    EXPECT_THROW(Blackboard::try_open(NS, NAME), std::runtime_error);
    bb.header()->capacity = saved;
}

TEST_F(BlackboardTest, RejectsInvalidMaxValueSize)
{
    auto     bb    = open();
    uint64_t saved = bb.header()->max_value_size;
    bb.header()->max_value_size = blackboard::MAX_VALUE_SIZE + 1;
    EXPECT_THROW(Blackboard::try_open(NS, NAME), std::runtime_error);
    bb.header()->max_value_size = 0;
    EXPECT_THROW(Blackboard::try_open(NS, NAME), std::runtime_error);
    bb.header()->max_value_size = saved;
}

TEST_F(BlackboardTest, RejectsGeometryMismatchOnOpen)
{
    auto bb = open();
    blackboard::Config other;
    other.capacity       = 16;
    other.max_value_size = 64;
    EXPECT_THROW(Blackboard::open_or_create(NS, NAME, other), std::runtime_error);
}

TEST_F(BlackboardTest, RejectsIdentityMismatchOnOpen)
{
    auto cfg = small_cfg();
    cfg.identity = 0xAAAA;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg);

    cfg.identity = 0xBBBB;
    EXPECT_THROW(Blackboard::open_or_create(NS, NAME, cfg), std::runtime_error);
}

// ---- ABA -----------------------------------------------------------------

TEST_F(BlackboardTest, EntryRetenancyInvalidatesCachedReaderIndex)
{
    blackboard::Config cfg;
    cfg.capacity       = 1;
    cfg.max_value_size = 32;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg);

    auto reader = bb.observe("a");
    {
        auto w = bb.declare("a");
        ASSERT_TRUE(w.write(Sample{1, 1}));
        Sample got{};
        ASSERT_EQ(reader.read(got).status, blackboard::Ok);   // caches the index
        orphan(bb, "a");
    }
    ASSERT_EQ(bb.sweep_stale(), 1u);

    auto w2 = bb.declare("b");
    ASSERT_TRUE(w2.write(Sample{99, 99}));

    Sample got{};
    auto   out = reader.read(got);
    EXPECT_EQ(out.status, blackboard::Missing);
    EXPECT_NE(got.id, 99u);
}

TEST_F(BlackboardTest, FreshClaimDoesNotResurrectPreviousTenantValue)
{
    blackboard::Config cfg;
    cfg.capacity       = 1;
    cfg.max_value_size = 32;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg);

    {
        auto w = bb.declare("a");
        ASSERT_TRUE(w.write(Sample{42, 42}));
        orphan(bb, "a");
    }
    ASSERT_EQ(bb.sweep_stale(), 1u);

    auto   w2 = bb.declare("b");
    auto   r  = bb.observe("b");
    Sample got{};
    EXPECT_EQ(r.read(got).status, blackboard::Unset);
}

// ---- convenience overloads ----------------------------------------------

TEST_F(BlackboardTest, VectorReadResizesAndReusesCapacity)
{
    auto bb = open();
    auto w  = bb.declare("k");
    auto r  = bb.observe("k");

    std::vector<uint8_t> payload(48, 0x7E);
    ASSERT_TRUE(w.write(payload.data(), payload.size()));

    std::vector<uint8_t> out;
    auto result = r.read(out);
    ASSERT_EQ(result.status, blackboard::Ok);
    ASSERT_EQ(out.size(), 48u);
    EXPECT_EQ(out, payload);

    std::size_t cap_before = out.capacity();
    result = r.read(out);
    EXPECT_EQ(result.status, blackboard::Ok);
    EXPECT_EQ(out.capacity(), cap_before);
}

TEST_F(BlackboardTest, TypedReadRejectsAValueOfADifferentSize)
{
    // A short value would otherwise return Ok with the tail of `out` holding
    // whatever was on the caller's stack.
    auto bb = open();
    auto w  = bb.declare("k");
    auto r  = bb.observe("k");

    ASSERT_TRUE(w.write(uint32_t{0xABCD1234}));

    uint64_t wide = 0xFFFFFFFFFFFFFFFFull;
    auto     out  = r.read(wide);
    EXPECT_EQ(out.status, blackboard::SizeMismatch);
    EXPECT_EQ(out.len, 4u);
    EXPECT_EQ(wide, 0xFFFFFFFFFFFFFFFFull) << "out must not be touched";

    // The matching type still reads.
    uint32_t narrow = 0;
    EXPECT_EQ(r.read(narrow).status, blackboard::Ok);
    EXPECT_EQ(narrow, 0xABCD1234u);

    // And a value larger than T is still Truncated, not a partial fill.
    ASSERT_TRUE(w.write(std::vector<uint8_t>(32, 0x11).data(), 32));
    EXPECT_EQ(r.read(narrow).status, blackboard::Truncated);
    EXPECT_EQ(narrow, 0xABCD1234u);
}

TEST_F(BlackboardTest, ClosedBoardThrowsRatherThanDereferencingNull)
{
    Blackboard closed;
    EXPECT_FALSE(closed.valid());
    EXPECT_THROW(closed.capacity(), std::runtime_error);
    EXPECT_THROW(closed.max_value_size(), std::runtime_error);
    EXPECT_THROW(closed.change_seq(), std::runtime_error);
    EXPECT_THROW(closed.declare("k"), std::runtime_error);
    EXPECT_THROW(closed.observe("k"), std::runtime_error);
    EXPECT_THROW(closed.wait(0, std::chrono::milliseconds(1)), std::runtime_error);
    EXPECT_THROW(closed.snapshot(), std::runtime_error);
    EXPECT_THROW(closed.sweep_stale(), std::runtime_error);

    auto bb = open();
    ASSERT_TRUE(bb.valid());
    Blackboard moved = std::move(bb);
    EXPECT_TRUE(moved.valid());
    EXPECT_FALSE(bb.valid());
    EXPECT_THROW(bb.capacity(), std::runtime_error);
}

TEST_F(BlackboardTest, RawByteRoundTrip)
{
    auto bb = open();
    auto w  = bb.declare("k");
    auto r  = bb.observe("k");

    char const* text = "lifecycle=ACTIVE";
    ASSERT_TRUE(w.write(text, std::strlen(text)));

    char buf[64] = {};
    auto out = r.read(buf, sizeof(buf));
    ASSERT_EQ(out.status, blackboard::Ok);
    EXPECT_EQ(std::string(buf, out.len), text);
}

TEST_F(BlackboardTest, ConcurrentDeclareOfOneKeyYieldsOneOwner)
{
    // Two claimants can both miss a not-yet-existing key and then claim two
    // different Free entries for it.  Exactly one must survive.
    auto bb = open();

    std::atomic<int> winners{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.emplace_back([&]
        {
            while (not go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            try
            {
                auto w = bb.declare("contested");
                winners.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                w.release();
            }
            catch (std::runtime_error const&)
            {
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(winners.load(), 1);

    // And exactly one entry carries the key.
    int matches = 0;
    for (auto const& ks : bb.snapshot())
    {
        if (ks.key == "contested")
        {
            ++matches;
        }
    }
    EXPECT_EQ(matches, 1);
}

// ---- audit regressions ---------------------------------------------------

TEST_F(BlackboardTest, MaxValueSizeIsExactlyAsConfigured)
{
    // The cell stride is padded to a cache line; that padding must not be
    // handed out as payload capacity, or one peer writes more than a
    // correctly-sized reader can hold.
    blackboard::Config cfg;
    cfg.capacity       = 4;
    cfg.max_value_size = 128;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg);

    EXPECT_EQ(bb.max_value_size(), 128u);

    auto w = bb.declare("k");
    std::vector<uint8_t> exact(128, 0xEE);
    EXPECT_TRUE(w.write(exact.data(), exact.size()));

    std::vector<uint8_t> over(129, 0xEE);
    EXPECT_FALSE(w.write(over.data(), over.size()));
}

TEST_F(BlackboardTest, BoardAtMaxValueSizeCanBeReopened)
{
    // The stride derived from MAX_VALUE_SIZE must not exceed the opener's
    // bound: creating at the documented maximum then failing every open would
    // be a board that can only ever be written by its creator.
    blackboard::Config cfg;
    cfg.capacity       = 1;
    cfg.max_value_size = blackboard::MAX_VALUE_SIZE;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg);
    EXPECT_EQ(bb.max_value_size(), blackboard::MAX_VALUE_SIZE);

    auto reopened = Blackboard::try_open(NS, NAME);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(reopened->max_value_size(), blackboard::MAX_VALUE_SIZE);
}

// A hammer, not a proof: release and takeover are serialized by the board
// lock, so the interleaving this once caught is now unreachable by
// construction.  What it locks in is that a release racing a takeover always
// leaves a usable writer behind.
TEST_F(BlackboardTest, ConcurrentReleaseAndTakeoverYieldAWorkingWriter)
{
    for (int round = 0; round < 200; ++round)
    {
        Blackboard::unlink(NS, NAME);
        auto bb = open();

        auto              first = bb.declare("contended");
        std::atomic<bool> taken{false};
        std::atomic<bool> wrote{false};

        std::thread taker([&]
        {
            while (not taken.load(std::memory_order_acquire))
            {
                try
                {
                    auto w = bb.declare("contended");
                    taken.store(true, std::memory_order_release);
                    wrote.store(w.write(uint32_t{7}), std::memory_order_release);
                    w.release();
                    return;
                }
                catch (std::runtime_error const&)
                {
                    std::this_thread::yield();
                }
            }
        });

        first.release();
        taker.join();

        ASSERT_TRUE(taken.load()) << "round " << round;
        EXPECT_TRUE(wrote.load()) << "takeover produced a writer that cannot write"
                                  << " (round " << round << ")";
    }
}

TEST_F(BlackboardTest, DirectOpenDetectsSanitizedNameCollision)
{
    // "a:b" and "a b" both sanitize to one shm path.  A caller that supplies
    // no identity must still get the collision rejected.
    Blackboard::unlink(NS, "a:b");
    auto cfg = small_cfg();
    auto first = Blackboard::open_or_create(NS, "a:b", cfg);
    EXPECT_THROW(Blackboard::open_or_create(NS, "a b", cfg), std::runtime_error);
    Blackboard::unlink(NS, "a:b");
}

TEST_F(BlackboardTest, TryOpenReportsAnUninitializedRegionRatherThanAbsent)
{
    // A creator that died before publishing MAGIC leaves a region that exists
    // but is unusable.  Calling that "absent" points an operator at the wrong
    // problem.
    auto name = Blackboard::shm_name(NS, NAME);
    SharedMemory raw;
    raw.create(name, 64 * 1024);   // zeroed: magic never published
    EXPECT_THROW(Blackboard::try_open(NS, NAME), std::runtime_error);
}

TEST_F(BlackboardTest, FreedEntryDoesNotLeaveADeadPidForTheNextClaimant)
{
    // A Free entry that kept its dead owner's pid would be inherited by the
    // next claimant the moment it CASes to Claiming -- and a concurrent
    // sweeper would then reclaim that live claim using the corpse's identity.
    auto bb = open();
    {
        auto w = bb.declare("gone");
        ASSERT_TRUE(w.write(Sample{1, 1}));
        orphan(bb, "gone");
    }
    ASSERT_EQ(bb.sweep_stale(), 1u);

    auto* e = entry(bb, 0);
    EXPECT_EQ(e->state.load(std::memory_order_acquire), blackboard::Free);
    EXPECT_EQ(e->owner_pid.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(e->owner_starttime.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(e->key_hash.load(std::memory_order_relaxed), 0u);

    // A fresh claim must survive a sweep running against it.
    auto w2 = bb.declare("fresh");
    EXPECT_EQ(bb.sweep_stale(), 0u);
    EXPECT_TRUE(w2.write(Sample{2, 2}));
}

// ---- crash-point matrix --------------------------------------------------
//
// Every metadata transition runs under the board lock, so a process can die at
// any store between taking that lock and dropping it.  Each case fabricates an
// entry exactly as such a death would leave it, marks the board lock as held by
// that dead process, and asserts the board recovers when the next actor takes
// the lock.
//
// The postcondition is uniform and deliberately strict:
//
//   1. the entry settles in a terminal state -- never Claiming
//   2. the board lock is free again
//   3. an Active entry has a key (no phantom: unreadable and unsweepable)
//   4. a Free entry carries no identity for the next claimant to inherit
//   5. the board still accepts a new key
//   6. an unrelated key is untouched (no collateral damage)

namespace
{
    struct CrashPoint
    {
        char const* name;
        bool        lock_held;   ///< died holding the board lock
        void (*fabricate)(BlackboardEntry*, uint64_t key_hash);
    };

    void zero_meta(BlackboardEntry* e)
    {
        e->owner_pid.store(0, std::memory_order_relaxed);
        e->owner_starttime.store(0, std::memory_order_relaxed);
        e->key_hash.store(0, std::memory_order_relaxed);
    }

    void set_key(BlackboardEntry* e, uint64_t kh)
    {
        std::memset(e->key, 0, sizeof(e->key));
        std::memcpy(e->key, "victim", 6);
        e->key_hash.store(kh, std::memory_order_relaxed);
    }

    void dead_owner(BlackboardEntry* e)
    {
        e->owner_starttime.store(1, std::memory_order_relaxed);
        e->owner_pid.store(DEAD_PID, std::memory_order_relaxed);
    }

    void put(BlackboardEntry* e, uint32_t state)
    {
        e->state.store(state, std::memory_order_release);
    }

    constexpr CrashPoint CRASH_POINTS[] = {
        // --- claim: Free -> Claiming -> Active, all under the lock ---
        {"claim/lock-taken-nothing-done", true,
         [](BlackboardEntry* e, uint64_t) { zero_meta(e); std::memset(e->key, 0, sizeof(e->key));
                                            put(e, blackboard::Free); }},
        {"claim/state-claiming", true,
         [](BlackboardEntry* e, uint64_t) { zero_meta(e); std::memset(e->key, 0, sizeof(e->key));
                                            put(e, blackboard::Claiming); }},
        {"claim/key-written", true,
         [](BlackboardEntry* e, uint64_t) { zero_meta(e);
                                            std::memset(e->key, 0, sizeof(e->key));
                                            std::memcpy(e->key, "victim", 6);
                                            put(e, blackboard::Claiming); }},
        {"claim/fully-filled-not-committed", true,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); dead_owner(e); set_key(e, kh);
                                               put(e, blackboard::Claiming); }},
        {"claim/committed-lock-not-dropped", true,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); dead_owner(e); set_key(e, kh);
                                               put(e, blackboard::Active); }},

        // --- takeover: entry never leaves Active ---
        {"takeover/owner-node-half-written", true,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); dead_owner(e); set_key(e, kh);
                                               std::memset(e->owner_node, 'x',
                                                           sizeof(e->owner_node));
                                               put(e, blackboard::Active); }},
        {"takeover/new-owner-published", true,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); dead_owner(e); set_key(e, kh);
                                               put(e, blackboard::Active); }},

        // --- release ---
        {"release/owner-pid-cleared", true,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); set_key(e, kh);
                                               e->owner_starttime.store(
                                                   1, std::memory_order_relaxed);
                                               put(e, blackboard::Active); }},
        {"release/complete-lock-not-dropped", true,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); set_key(e, kh);
                                               put(e, blackboard::Active); }},

        // --- sweep freeing a dead owner ---
        {"sweepfree/identity-cleared-state-not", true,
         [](BlackboardEntry* e, uint64_t) { zero_meta(e); put(e, blackboard::Active); }},
        {"sweepfree/free-published-identity-not", true,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); dead_owner(e); set_key(e, kh);
                                               put(e, blackboard::Free); }},
        {"sweepfree/complete-lock-not-dropped", true,
         [](BlackboardEntry* e, uint64_t) { zero_meta(e); std::memset(e->key, 0, sizeof(e->key));
                                            put(e, blackboard::Free); }},

        // --- forged states, lock NOT held: a corrupt peer can write these and
        //     each would otherwise wedge a key or leak a slot forever ---
        {"corrupt/claiming-orphan", false,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); dead_owner(e); set_key(e, kh);
                                               put(e, blackboard::Claiming); }},
        {"corrupt/active-without-key", false,
         [](BlackboardEntry* e, uint64_t) { zero_meta(e); dead_owner(e);
                                            std::memset(e->key, 0, sizeof(e->key));
                                            put(e, blackboard::Active); }},
        {"corrupt/free-carrying-identity", false,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); dead_owner(e); set_key(e, kh);
                                               put(e, blackboard::Free); }},
        {"corrupt/active-dead-owner", false,
         [](BlackboardEntry* e, uint64_t kh) { zero_meta(e); dead_owner(e); set_key(e, kh);
                                               put(e, blackboard::Active); }},
    };
}

TEST_F(BlackboardTest, CrashPointMatrix)
{
    for (auto const& point : CRASH_POINTS)
    {
        Blackboard::unlink(NS, NAME);
        blackboard::Config cfg;
        cfg.capacity       = 4;
        cfg.max_value_size = 64;
        auto bb = Blackboard::open_or_create(NS, NAME, cfg);

        // Index 0 is the victim; index 1 is an untouched neighbour.
        auto victim = bb.declare("victim", "owner");
        ASSERT_TRUE(victim.write(Sample{1, 1})) << point.name;
        auto neighbour = bb.declare("neighbour", "owner");
        ASSERT_TRUE(neighbour.write(Sample{2, 2})) << point.name;

        auto* h = bb.header();
        auto* e = entry(bb, 0);
        point.fabricate(e, hash::fnv1a_64(std::string_view("victim")));
        if (point.lock_held)
        {
            // The board lock, left held by a process that is gone.
            h->lock_token.store((uint64_t{7} << 32) | DEAD_PID,
                                std::memory_order_release);
        }

        bb.sweep_stale();

        std::string ctx = std::string("crash point: ") + point.name;
        bool        ok  = true;

        uint32_t st = e->state.load(std::memory_order_acquire);
        ok = ok and (st == blackboard::Free or st == blackboard::Active);
        ok = ok and h->lock_token.load(std::memory_order_acquire) == 0;
        if (st == blackboard::Active)
        {
            ok = ok and e->key_hash.load(std::memory_order_relaxed) != 0;
            ok = ok and ::strnlen(e->key, blackboard::KEY_MAX) != 0;
        }
        if (st == blackboard::Free)
        {
            ok = ok and e->key_hash.load(std::memory_order_relaxed) == 0;
            ok = ok and e->owner_pid.load(std::memory_order_relaxed) == 0;
        }

        bool declarable = true;
        try
        {
            auto probe = bb.declare("probe");
            declarable = probe.write(Sample{3, 3});
        }
        catch (std::runtime_error const&)
        {
            declarable = false;
        }
        ok = ok and declarable;

        Sample got{};
        auto   out = bb.observe("neighbour").read(got);
        ok = ok and out.status == blackboard::Ok and got.id == 2u;

        EXPECT_TRUE(ok) << ctx
                        << " | state=" << st
                        << " lock=" << h->lock_token.load(std::memory_order_acquire)
                        << " key_hash=" << e->key_hash.load(std::memory_order_relaxed)
                        << " owner=" << e->owner_pid.load(std::memory_order_relaxed)
                        << " declarable=" << declarable;
    }
    std::printf("[ crash matrix ] %zu points\n",
                sizeof(CRASH_POINTS) / sizeof(CRASH_POINTS[0]));
}


// ---- lock failure paths --------------------------------------------------

namespace
{
    /// A token that reads as a LIVE holder, so it is never transferred away.
    /// Mirrors Blackboard.cc's encoding deliberately: a change there must fail
    /// here loudly, not degrade into a dead token that gets recovered.
    uint64_t live_lock_token()
    {
        uint64_t pid = current_pid();
        uint64_t fp  = hash::fnv1a_64(process_starttime(pid)) >> 32;
        if (fp == 0)
        {
            fp = 1;
        }
        return (fp << 32) | (pid & 0xFFFFFFFFull);
    }
}

TEST_F(BlackboardTest, SnapshotReportsBusyRatherThanReadingUnlocked)
{
    // A takeover rewrites owner_node under the lock with no seqlock over those
    // bytes.  Reading anyway once the budget expires is exactly the race the
    // lock exists to prevent, so it must report busy instead of returning rows.
    auto bb = open();
    auto w  = bb.declare("k", "owner");
    ASSERT_TRUE(w.write(Sample{1, 1}));

    auto* h = bb.header();
    h->lock_token.store(live_lock_token(), std::memory_order_release);

    EXPECT_THROW(bb.snapshot(), std::runtime_error);

    h->lock_token.store(0, std::memory_order_release);
    EXPECT_EQ(bb.snapshot().size(), 1u);
}

TEST_F(BlackboardTest, LockIsNeverLeftHeldOnAThrowingPath)
{
    // Every acquisition goes through BoardGuard, so an early exit -- a throw
    // from a rejected declare, a busy snapshot, or an allocation failure while
    // building rows -- cannot strand the board.  A leaked lock held by a LIVE
    // pid is never transferred, so it would block every process forever.
    auto bb = open();
    auto* h = bb.header();

    auto w = bb.declare("owned", "owner");
    ASSERT_TRUE(w.write(Sample{1, 1}));

    EXPECT_THROW(bb.declare("owned"), std::runtime_error);
    EXPECT_EQ(h->lock_token.load(std::memory_order_acquire), 0u);

    EXPECT_THROW(bb.declare(std::string(blackboard::KEY_MAX + 5, 'x').c_str()),
                 std::invalid_argument);
    EXPECT_EQ(h->lock_token.load(std::memory_order_acquire), 0u);

    h->lock_token.store(live_lock_token(), std::memory_order_release);
    EXPECT_THROW(bb.snapshot(), std::runtime_error);
    h->lock_token.store(0, std::memory_order_release);
    EXPECT_EQ(h->lock_token.load(std::memory_order_acquire), 0u);

    bb.snapshot();
    EXPECT_EQ(h->lock_token.load(std::memory_order_acquire), 0u);

    // And the board still works.
    auto w2 = bb.declare("after");
    EXPECT_TRUE(w2.write(Sample{2, 2}));
}

TEST_F(BlackboardTest, ReleaseWaitsRatherThanStrandingTheKey)
{
    // release() cannot report failure -- it runs from a destructor -- so
    // giving up on the lock would leave ownership recorded for a process that
    // is still alive, which no sweep can reclaim.  It must wait.
    auto  bb = open();
    auto* h  = bb.header();

    auto w = bb.declare("stuck", "owner");
    ASSERT_TRUE(w.write(Sample{1, 1}));

    // A peer takes the board and holds it across the release.
    h->lock_token.store(live_lock_token(), std::memory_order_release);
    std::thread unlocker([&]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        h->lock_token.store(0, std::memory_order_release);
    });

    auto start = std::chrono::steady_clock::now();
    w.release();
    auto waited = std::chrono::steady_clock::now() - start;
    unlocker.join();

    EXPECT_GE(waited, std::chrono::milliseconds(50))
        << "release returned without waiting for the board lock";

    // Ownership really was cleared, so the key is redeclarable.
    auto w2 = bb.declare("stuck");
    EXPECT_TRUE(w2.write(Sample{2, 2}));
}

// A hammer, not a proof: it still passes with the publish re-check removed,
// because the window between the publish load and the cell read is
// nanoseconds.  It locks in coherence in normal operation, nothing more.
TEST_F(BlackboardTest, SnapshotValueTupleIsCoherentUnderConcurrentWrites)
{
    // The board lock excludes metadata operations, not writers.  Two writes can
    // wrap back onto the cell a snapshot chose, pairing update_count k with the
    // length and timestamp of k+2.  The payload length is tied to the sequence
    // parity here, so an incoherent tuple is detectable.
    auto bb = open();
    auto w  = bb.declare("hot", "owner");

    std::atomic<bool> stop{false};
    std::thread writer([&]
    {
        std::vector<uint8_t> small(8, 0xA1);
        std::vector<uint8_t> large(40, 0xB2);
        for (uint64_t seq = 1; not stop.load(std::memory_order_acquire); ++seq)
        {
            if (seq % 2 == 1)
            {
                w.write(small.data(), small.size());
            }
            else
            {
                w.write(large.data(), large.size());
            }
        }
    });

    int checked = 0;
    for (int i = 0; i < 4000; ++i)
    {
        std::vector<blackboard::KeyStatus> snap;
        try
        {
            snap = bb.snapshot();
        }
        catch (std::runtime_error const&)
        {
            continue;
        }
        for (auto const& ks : snap)
        {
            if (ks.update_count == 0)
            {
                continue;
            }
            std::size_t expected = 40;
            if (ks.update_count % 2 == 1)
            {
                expected = 8;
            }
            EXPECT_EQ(ks.value_len, expected)
                << "update_count " << ks.update_count << " paired with length "
                << ks.value_len;
            ++checked;
        }
    }
    stop.store(true, std::memory_order_release);
    writer.join();
    EXPECT_GT(checked, 0);
}

TEST_F(BlackboardTest, ConcurrentDeclareAcrossEntriesYieldsOneOwnerPerKey)
{
    // Tracks PEAK simultaneous owners rather than a total: a legal sequential
    // takeover must not look like a violation, and a run where nobody won must
    // not pass vacuously.
    constexpr int KEYS    = 4;
    constexpr int THREADS = 12;

    for (int round = 0; round < 40; ++round)
    {
        Blackboard::unlink(NS, NAME);
        blackboard::Config cfg;
        cfg.capacity       = 32;
        cfg.max_value_size = 32;
        auto bb = Blackboard::open_or_create(NS, NAME, cfg);

        std::atomic<bool> go{false};
        std::atomic<int>  live[KEYS];
        std::atomic<int>  peak[KEYS];
        std::atomic<int>  wins[KEYS];
        std::atomic<int>  busy{0};
        for (int k = 0; k < KEYS; ++k)
        {
            live[k].store(0);
            peak[k].store(0);
            wins[k].store(0);
        }

        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t)
        {
            threads.emplace_back([&, t]
            {
                int         slot = t % KEYS;
                std::string key  = "k" + std::to_string(slot);
                while (not go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                try
                {
                    auto w = bb.declare(key.c_str());
                    wins[slot].fetch_add(1, std::memory_order_relaxed);

                    int now = live[slot].fetch_add(1, std::memory_order_acq_rel) + 1;
                    int seen = peak[slot].load(std::memory_order_relaxed);
                    while (now > seen
                           and not peak[slot].compare_exchange_weak(
                                   seen, now, std::memory_order_relaxed))
                    {
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    live[slot].fetch_sub(1, std::memory_order_acq_rel);

                    w.release();
                }
                catch (std::runtime_error const& e)
                {
                    // A live owner holding the key is the expected rejection;
                    // a busy board is a different outcome and must stay rare.
                    if (std::string(e.what()).find("busy") != std::string::npos)
                    {
                        busy.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads)
        {
            t.join();
        }

        for (int k = 0; k < KEYS; ++k)
        {
            EXPECT_LE(peak[k].load(), 1)
                << "key k" << k << " had " << peak[k].load()
                << " simultaneous owners (round " << round << ")";
            EXPECT_GE(wins[k].load(), 1)
                << "key k" << k << " was never granted (round " << round << ")";
        }
        EXPECT_EQ(busy.load(), 0) << "board lock contention surfaced as busy";

        std::map<std::string, int> seen;
        for (auto const& ks : bb.snapshot())
        {
            ++seen[ks.key];
        }
        for (auto const& [k, n] : seen)
        {
            EXPECT_EQ(n, 1) << "key '" << k << "' held by " << n
                            << " entries (round " << round << ")";
        }
    }
}
