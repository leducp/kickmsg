#include "common.h"

#include "kickmsg/Hash.h"

// Large-payload torn-read hunt: 8 KB messages through a small pool (16) and
// tiny rings (8) so eviction constantly races readers. Every byte of every
// sample is validated against a deterministic pattern plus an FNV-1a checksum
// over the whole body; any mismatch is a torn read. The zero-copy reader
// validates the SampleView twice -- a second pass that fails after a clean
// first pass proves the slot was overwritten while pinned.

namespace
{
    constexpr std::size_t BIG_PAYLOAD_SIZE = 8192;

    struct BigHeader
    {
        static constexpr uint32_t MAGIC = 0xB16FEED5;
        uint32_t magic;
        uint32_t pub_id;
        uint32_t seq;
        uint32_t byte_count;
        uint64_t checksum;
    };

    constexpr std::size_t BIG_BODY_SIZE = BIG_PAYLOAD_SIZE - sizeof(BigHeader);

    constexpr int  NUM_PUBS = 4;
    constexpr int  NUM_SUBS = 2;

    inline uint8_t pattern_byte(uint32_t pub_id, uint32_t seq, std::size_t i)
    {
        return static_cast<uint8_t>(seq * 31u + pub_id * 131u + i);
    }

    struct BigSubStats
    {
        uint64_t received  = 0;
        uint64_t lost      = 0;
        uint64_t corrupted = 0;
        uint64_t torn_view = 0;
        uint64_t reordered = 0;
    };

    // Full validation of one sample: header sanity, every body byte against
    // the pattern, and the FNV-1a checksum over the whole body.
    bool validate_big(uint8_t const* data, std::size_t len, int sub_id, char const* pass_label)
    {
        if (len != BIG_PAYLOAD_SIZE)
        {
            std::fprintf(stderr, "  [FAIL] sub%d (%s): wrong-length sample (%zu bytes)\n",
                         sub_id, pass_label, len);
            return false;
        }

        BigHeader hdr;
        std::memcpy(&hdr, data, sizeof(hdr));

        if (hdr.magic != BigHeader::MAGIC
            or hdr.pub_id >= static_cast<uint32_t>(NUM_PUBS)
            or hdr.byte_count != BIG_BODY_SIZE)
        {
            std::fprintf(stderr, "  [FAIL] sub%d (%s): bad header (magic=%08x pub=%u bytes=%u)\n",
                         sub_id, pass_label, hdr.magic, hdr.pub_id, hdr.byte_count);
            return false;
        }

        uint8_t const* body = data + sizeof(BigHeader);
        for (std::size_t i = 0; i < BIG_BODY_SIZE; ++i)
        {
            if (body[i] != pattern_byte(hdr.pub_id, hdr.seq, i))
            {
                std::fprintf(stderr, "  [FAIL] sub%d (%s): torn body at byte %zu "
                             "(pub %u seq %u: got %02x, want %02x)\n",
                             sub_id, pass_label, i, hdr.pub_id, hdr.seq,
                             body[i], pattern_byte(hdr.pub_id, hdr.seq, i));
                return false;
            }
        }

        uint64_t sum = kickmsg::hash::fnv1a_64(body, BIG_BODY_SIZE);
        if (sum != hdr.checksum)
        {
            std::fprintf(stderr, "  [FAIL] sub%d (%s): checksum mismatch "
                         "(pub %u seq %u: got %016" PRIx64 ", want %016" PRIx64 ")\n",
                         sub_id, pass_label, hdr.pub_id, hdr.seq, sum, hdr.checksum);
            return false;
        }
        return true;
    }

    void check_reorder(uint8_t const* data, std::vector<uint32_t>& last_seq, BigSubStats& stats,
                       int sub_id)
    {
        BigHeader hdr;
        std::memcpy(&hdr, data, sizeof(hdr));
        auto& prev = last_seq[hdr.pub_id];
        if (prev != UINT32_MAX and hdr.seq <= prev)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: pub %u seq %u after seq %u (reorder)\n",
                         sub_id, hdr.pub_id, hdr.seq, prev);
            ++stats.reordered;
            return;
        }
        prev = hdr.seq;
        ++stats.received;
    }
}

bool run_big_payload()
{
    uint32_t const NUM_MSGS = 50000 / TSAN_SCALE;

    std::printf("--- Big payload: %d pubs x %u msgs of %zu B, pool=16, ring=8, "
                "1 copy + 1 zerocopy sub ---\n",
                NUM_PUBS, NUM_MSGS, BIG_PAYLOAD_SIZE);

    g_all_publishers_done  = false;
    g_subscribers_ready    = 0;
    g_subscribers_expected = NUM_SUBS;

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = NUM_SUBS;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = BIG_PAYLOAD_SIZE;

    char const* shm_name = "/kickmsg_big_payload";
    kickmsg::SharedMemory::unlink(shm_name);
    auto region = kickmsg::SharedRegion::create(
        shm_name, kickmsg::channel::PubSub, cfg, "big_payload");

    std::vector<BigSubStats> sub_stats(NUM_SUBS);

    auto pub_worker = [&](int pub_id)
    {
        kickmsg::Publisher pub{region};

        wait_subscribers_ready();

        std::vector<uint8_t> buf(BIG_PAYLOAD_SIZE);

        for (uint32_t i = 0; i < NUM_MSGS; ++i)
        {
            uint8_t* body = buf.data() + sizeof(BigHeader);
            for (std::size_t b = 0; b < BIG_BODY_SIZE; ++b)
            {
                body[b] = pattern_byte(static_cast<uint32_t>(pub_id), i, b);
            }

            BigHeader hdr;
            hdr.magic      = BigHeader::MAGIC;
            hdr.pub_id     = static_cast<uint32_t>(pub_id);
            hdr.seq        = i;
            hdr.byte_count = static_cast<uint32_t>(BIG_BODY_SIZE);
            hdr.checksum   = kickmsg::hash::fnv1a_64(body, BIG_BODY_SIZE);
            std::memcpy(buf.data(), &hdr, sizeof(hdr));

            int32_t rc;
            while ((rc = pub.send(buf.data(), buf.size())) < 0)
            {
                if (rc != -EAGAIN)
                {
                    std::fprintf(stderr, "  [FATAL] publisher %d: send() returned %d\n",
                                 pub_id, rc);
                    std::abort();
                }
                kickmsg::yield();
            }
        }
    };

    // Copy reader: try_receive path (memcpy out of the slot under the pin).
    auto copy_sub = [&](int sub_id)
    {
        kickmsg::Subscriber sub{region};
        g_subscribers_ready.fetch_add(1, std::memory_order_release);

        auto& stats = sub_stats[static_cast<std::size_t>(sub_id)];
        std::vector<uint32_t> last_seq(NUM_PUBS, UINT32_MAX);
        auto const timeout = milliseconds{500};

        while (true)
        {
            auto sample = sub.receive(timeout);
            if (not sample)
            {
                if (g_all_publishers_done)
                {
                    // try_receive can return null after exhausting its retry
                    // budget on a run of evicted entries; only "null with no
                    // lost() progress" proves the ring is empty.
                    uint64_t lost_before = sub.lost();
                    sample = sub.try_receive();
                    if (not sample)
                    {
                        if (sub.lost() == lost_before)
                        {
                            break;
                        }
                        continue;
                    }
                }
                else
                {
                    continue;
                }
            }

            auto const* data = static_cast<uint8_t const*>(sample->data());
            if (not validate_big(data, sample->len(), sub_id, "copy"))
            {
                ++stats.corrupted;
                continue;
            }
            check_reorder(data, last_seq, stats, sub_id);
        }

        stats.lost = sub.lost();
    };

    // Zero-copy reader: validates THROUGH the SampleView twice. The pin must
    // keep the slot immutable for the view's whole lifetime, so a second pass
    // failing after a clean first pass is a pin violation, not a torn commit.
    auto view_sub = [&](int sub_id)
    {
        kickmsg::Subscriber sub{region};
        g_subscribers_ready.fetch_add(1, std::memory_order_release);

        auto& stats = sub_stats[static_cast<std::size_t>(sub_id)];
        std::vector<uint32_t> last_seq(NUM_PUBS, UINT32_MAX);
        auto const timeout = milliseconds{500};

        while (true)
        {
            auto view = sub.receive_view(timeout);
            if (not view)
            {
                if (g_all_publishers_done)
                {
                    uint64_t lost_before = sub.lost();
                    view = sub.try_receive_view();
                    if (not view)
                    {
                        if (sub.lost() == lost_before)
                        {
                            break;
                        }
                        continue;
                    }
                }
                else
                {
                    continue;
                }
            }

            auto const* data = static_cast<uint8_t const*>(view->data());
            if (not validate_big(data, view->len(), sub_id, "view pass 1"))
            {
                ++stats.corrupted;
                continue;
            }
            if (not validate_big(data, view->len(), sub_id, "view pass 2"))
            {
                std::fprintf(stderr, "  [FAIL] sub%d: view changed between passes "
                             "(slot overwritten while pinned)\n", sub_id);
                ++stats.torn_view;
                continue;
            }
            check_reorder(data, last_seq, stats, sub_id);
        }

        stats.lost = sub.lost();
    };

    nanoseconds t0 = kickmsg::monotonic_ns();

    std::thread copy_thread(copy_sub, 0);
    std::thread view_thread(view_sub, 1);

    std::vector<std::thread> pub_threads;
    for (int i = 0; i < NUM_PUBS; ++i)
    {
        pub_threads.emplace_back(pub_worker, i);
    }

    for (auto& t : pub_threads)
    {
        t.join();
    }
    g_all_publishers_done.store(true, std::memory_order_release);

    copy_thread.join();
    view_thread.join();

    nanoseconds t1 = kickmsg::monotonic_ns();
    int64_t elapsed_ms = std::chrono::duration_cast<milliseconds>(t1 - t0).count();

    uint64_t const total_sent = static_cast<uint64_t>(NUM_PUBS) * NUM_MSGS;
    std::printf("  Elapsed: %" PRId64 " ms, total published: %" PRIu64 "\n",
                elapsed_ms, total_sent);

    bool ok = true;

    for (int i = 0; i < NUM_SUBS; ++i)
    {
        auto const& s = sub_stats[static_cast<std::size_t>(i)];
        std::printf("  sub%d: received=%" PRIu64 " lost=%" PRIu64 " corrupted=%" PRIu64
                    " torn_view=%" PRIu64 " reordered=%" PRIu64 "\n",
                    i, s.received, s.lost, s.corrupted, s.torn_view, s.reordered);

        if (s.corrupted > 0 or s.torn_view > 0 or s.reordered > 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: corrupted=%" PRIu64 " torn_view=%" PRIu64
                         " reordered=%" PRIu64 "\n",
                         i, s.corrupted, s.torn_view, s.reordered);
            ok = false;
        }
        if (s.received == 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: received 0 messages!\n", i);
            ok = false;
        }

        // Exact conservation: the readiness barrier means every ring is Live
        // before the first send, and both readers drain to completion.
        uint64_t accounted = s.received + s.lost + s.corrupted + s.torn_view + s.reordered;
        if (accounted != total_sent)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: received+lost+corrupted+torn+reorder (%" PRIu64
                         ") != total_sent (%" PRIu64 ")!\n",
                         i, accounted, total_sent);
            ok = false;
        }
    }

    ok &= verify_gc_zero(region, cfg);
    ok &= verify_refcounts_zero(region, cfg);
    ok &= verify_pool_free(region, cfg);
    ok &= verify_rings_inactive(region, cfg);

    region.unlink();

    if (ok)
    {
        std::printf("  [PASS]\n\n");
    }
    else
    {
        std::printf("  [FAIL]\n\n");
    }
    return ok;
}
