#include "common.h"

bool run_pool_exhaustion()
{
    std::printf("--- Pool exhaustion: 8 pubs, pool=8, 4 slow subs ---\n");

    g_all_publishers_done = false;

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = sizeof(Payload);

    char const* shm_name = "/kickmsg_pool_exhaustion";
    kickmsg::SharedMemory::unlink(shm_name);
    auto region = kickmsg::SharedRegion::create(
        shm_name, kickmsg::channel::PubSub, cfg, "pool_exhaustion");

    constexpr int  NUM_PUBS = 8;
    constexpr int  NUM_SUBS = 4;
    uint32_t const NUM_MSGS = 10000 / TSAN_SCALE;

    g_subscribers_ready    = 0;
    g_subscribers_expected = NUM_SUBS;

    std::atomic<uint64_t> eagain_count{0};
    std::atomic<uint64_t> corruption_count{0};

    struct SlowSubStats
    {
        uint64_t received  = 0;
        uint64_t lost      = 0;
        uint64_t corrupted = 0;
    };
    std::vector<SlowSubStats> sub_stats(NUM_SUBS);

    // Publishers that track EAGAIN
    auto pub_worker = [&](int pub_id)
    {
        kickmsg::Publisher pub{region};

        wait_subscribers_ready();

        for (uint32_t i = 0; i < NUM_MSGS; ++i)
        {
            Payload msg;
            msg.magic    = Payload::MAGIC;
            msg.pub_id   = static_cast<uint32_t>(pub_id);
            msg.seq      = i;
            msg.checksum = compute_checksum(msg);

            int32_t rc;
            while ((rc = pub.send(&msg, sizeof(msg))) < 0)
            {
                if (rc != -EAGAIN)
                {
                    std::fprintf(stderr, "  [FATAL] publisher %d: send() returned %d\n",
                                 pub_id, rc);
                    std::abort();
                }
                eagain_count.fetch_add(1, std::memory_order_relaxed);
                kickmsg::yield();
            }
        }
    };

    // Slow subscribers: 1us sleep between receives. Slow but complete: the
    // loop still drains every remaining message after publishers finish.
    auto slow_sub = [&](int sub_id)
    {
        kickmsg::Subscriber sub{region};
        g_subscribers_ready.fetch_add(1, std::memory_order_release);

        auto& stats = sub_stats[static_cast<std::size_t>(sub_id)];
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

            // Deliberately slow consumer
            kickmsg::sleep(1us);

            if (sample->len() != sizeof(Payload))
            {
                std::fprintf(stderr, "  [FAIL] sub%d: wrong-length sample (%zu bytes)\n",
                             sub_id, sample->len());
                ++stats.corrupted;
                corruption_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            Payload msg;
            std::memcpy(&msg, sample->data(), sizeof(msg));
            if (msg.magic != Payload::MAGIC or msg.checksum != compute_checksum(msg))
            {
                std::fprintf(stderr, "  [FAIL] sub%d: corruption detected\n", sub_id);
                ++stats.corrupted;
                corruption_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            ++stats.received;
        }

        stats.lost = sub.lost();
    };

    std::vector<std::thread> sub_threads;
    for (int i = 0; i < NUM_SUBS; ++i)
    {
        sub_threads.emplace_back(slow_sub, i);
    }

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

    for (auto& t : sub_threads)
    {
        t.join();
    }

    std::printf("  EAGAIN count: %" PRIu64 "\n", eagain_count.load());

    bool ok = true;

    if (corruption_count.load() != 0)
    {
        std::fprintf(stderr, "  [FAIL] %" PRIu64 " corrupted/wrong-length samples!\n",
                     corruption_count.load());
        ok = false;
    }

    uint64_t const total_sent = static_cast<uint64_t>(NUM_PUBS) * NUM_MSGS;
    for (int i = 0; i < NUM_SUBS; ++i)
    {
        auto const& s = sub_stats[static_cast<std::size_t>(i)];
        std::printf("  sub%d: received=%" PRIu64 " lost=%" PRIu64 " corrupted=%" PRIu64 "\n",
                    i, s.received, s.lost, s.corrupted);

        if (s.received == 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: received 0 messages!\n", i);
            ok = false;
        }

        // Exact conservation: the readiness barrier means every ring is Live
        // before the first send, and the slow consumers still drain to
        // completion, so every ring position lands in exactly one bucket.
        uint64_t accounted = s.received + s.lost + s.corrupted;
        if (accounted != total_sent)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: received+lost+corrupted (%" PRIu64
                         ") != total_sent (%" PRIu64 ")!\n",
                         i, accounted, total_sent);
            ok = false;
        }
    }

    ok &= verify_gc_zero(region, cfg);
    ok &= verify_pool_free(region, cfg);
    ok &= verify_refcounts_zero(region, cfg);
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
