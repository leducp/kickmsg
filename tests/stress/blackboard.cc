/// @file blackboard.cc
/// @brief Blackboard contention scenario.
///
/// W writer threads each own one key and rewrite a large checksummed payload
/// flat out; R reader threads wake on the board-wide change counter and read
/// every key.  The oracle that matters is the checksum: the parity double
/// buffer must never hand a reader a half-written value, at any write rate.

#include "common.h"

#include "kickmsg/Blackboard.h"

namespace
{
    constexpr char const* NS   = "kickmsg_bbstress";
    constexpr char const* NAME = "stress";

    struct BbValue
    {
        static constexpr uint32_t MAGIC = 0xB1ACB0AD;
        uint32_t magic;
        uint32_t writer_id;
        uint32_t seq;
        uint32_t checksum;
        uint8_t  filler[1008];
    };

    uint32_t bb_checksum(BbValue const& v)
    {
        uint32_t sum = v.magic ^ v.writer_id ^ v.seq ^ 0xDEADBEEFu;
        for (std::size_t i = 0; i < sizeof(v.filler); ++i)
        {
            sum = (sum << 1) ^ (sum >> 31) ^ v.filler[i];
        }
        return sum;
    }

    void fill(BbValue& v, uint32_t writer_id, uint32_t seq)
    {
        v.magic     = BbValue::MAGIC;
        v.writer_id = writer_id;
        v.seq       = seq;
        for (std::size_t i = 0; i < sizeof(v.filler); ++i)
        {
            v.filler[i] = static_cast<uint8_t>(seq + writer_id + i);
        }
        v.checksum = bb_checksum(v);
    }

    std::string key_for(int writer_id)
    {
        return "writer/" + std::to_string(writer_id) + "/state";
    }

    std::atomic<bool>     g_writers_done{false};
    std::atomic<uint64_t> g_writes{0};
    std::atomic<uint64_t> g_torn{0};
    std::atomic<uint64_t> g_regressed{0};
    std::atomic<uint64_t> g_busy{0};
    std::atomic<uint64_t> g_reads{0};

    void writer_thread(Blackboard& bb, int writer_id, uint32_t count)
    {
        auto    w = bb.declare(key_for(writer_id).c_str(), "stress");
        BbValue value{};
        for (uint32_t seq = 1; seq <= count; ++seq)
        {
            fill(value, static_cast<uint32_t>(writer_id), seq);
            if (w.write(value))
            {
                g_writes.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Ownership is released by the caller, after the change_seq oracle.
        w.release();
    }

    void reader_thread(Blackboard& bb, int num_writers)
    {
        std::vector<Blackboard::Reader> readers;
        readers.reserve(static_cast<std::size_t>(num_writers));
        for (int i = 0; i < num_writers; ++i)
        {
            readers.push_back(bb.observe(key_for(i).c_str()));
        }
        std::vector<uint64_t> last_count(static_cast<std::size_t>(num_writers), 0);

        BbValue  got{};
        uint64_t seen = bb.change_seq();
        for (;;)
        {
            bool done = g_writers_done.load(std::memory_order_acquire);

            for (int i = 0; i < num_writers; ++i)
            {
                auto out = readers[static_cast<std::size_t>(i)].read(got);
                if (out.status == blackboard::Busy)
                {
                    g_busy.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (out.status == blackboard::Unset
                    or out.status == blackboard::Missing)
                {
                    continue;
                }
                g_reads.fetch_add(1, std::memory_order_relaxed);

                if (got.magic != BbValue::MAGIC or got.checksum != bb_checksum(got)
                    or got.writer_id != static_cast<uint32_t>(i))
                {
                    g_torn.fetch_add(1, std::memory_order_relaxed);
                }
                if (out.update_count < last_count[static_cast<std::size_t>(i)])
                {
                    g_regressed.fetch_add(1, std::memory_order_relaxed);
                }
                last_count[static_cast<std::size_t>(i)] = out.update_count;
            }

            if (done)
            {
                return;
            }
            bb.wait(seen, milliseconds{20});
            seen = bb.change_seq();
        }
    }
}

bool run_blackboard_stress()
{
    int num_writers = static_cast<int>(contention_count());
    int num_readers = static_cast<int>(contention_count());
    uint32_t writes_per_writer = 20000 / TSAN_SCALE;

    blackboard::Config cfg;
    cfg.capacity       = static_cast<uint32_t>(num_writers) + 4;
    cfg.max_value_size = sizeof(BbValue);

    Blackboard::unlink(NS, NAME);
    auto bb = Blackboard::open_or_create(NS, NAME, cfg);

    g_writers_done.store(false, std::memory_order_release);
    g_writes.store(0, std::memory_order_relaxed);
    g_torn.store(0, std::memory_order_relaxed);
    g_regressed.store(0, std::memory_order_relaxed);
    g_busy.store(0, std::memory_order_relaxed);
    g_reads.store(0, std::memory_order_relaxed);

    uint64_t seq_before = bb.change_seq();

    std::vector<std::thread> writers;
    for (int i = 0; i < num_writers; ++i)
    {
        writers.emplace_back(writer_thread, std::ref(bb), i, writes_per_writer);
    }
    // Readers attach late on purpose: every key already has a value by the
    // time they observe it, which is the whole point of the pattern.
    std::vector<std::thread> readers;
    for (int i = 0; i < num_readers; ++i)
    {
        readers.emplace_back(reader_thread, std::ref(bb), num_writers);
    }

    for (auto& t : writers)
    {
        t.join();
    }
    uint64_t seq_after   = bb.change_seq();
    auto     final_snap  = bb.snapshot();

    g_writers_done.store(true, std::memory_order_release);
    for (auto& t : readers)
    {
        t.join();
    }

    uint64_t writes = g_writes.load(std::memory_order_relaxed);
    uint64_t torn   = g_torn.load(std::memory_order_relaxed);
    uint64_t regr   = g_regressed.load(std::memory_order_relaxed);
    uint64_t busy   = g_busy.load(std::memory_order_relaxed);
    uint64_t reads  = g_reads.load(std::memory_order_relaxed);

    // Ownership transitions are events too: each writer contributes one
    // declare and one release on top of its writes.
    uint64_t expected_delta = writes + 2 * static_cast<uint64_t>(num_writers);

    std::printf("         %d writers x %u writes, %d readers, %" PRIu64 " reads\n",
                num_writers, writes_per_writer, num_readers, reads);
    std::printf("         torn=%" PRIu64 " regressed=%" PRIu64 " busy=%" PRIu64
                " (busy is a transient retry-budget miss, not corruption)\n",
                torn, regr, busy);

    bool ok = true;
    if (torn != 0)
    {
        std::printf("         FAIL: %" PRIu64 " torn values escaped the double buffer\n", torn);
        ok = false;
    }
    if (regr != 0)
    {
        std::printf("         FAIL: %" PRIu64 " update_count regressions\n", regr);
        ok = false;
    }
    if (seq_after - seq_before != expected_delta)
    {
        std::printf("         FAIL: change_seq delta %" PRIu64 ", expected %" PRIu64 "\n",
                    seq_after - seq_before, expected_delta);
        ok = false;
    }
    if (final_snap.size() != static_cast<std::size_t>(num_writers))
    {
        std::printf("         FAIL: snapshot has %zu keys, expected %d\n",
                    final_snap.size(), num_writers);
        ok = false;
    }
    if (bb.sweep_stale() != 0)
    {
        std::printf("         FAIL: sweep_stale freed live keys\n");
        ok = false;
    }

    Blackboard::unlink(NS, NAME);
    return ok;
}
