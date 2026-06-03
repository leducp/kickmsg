#include "common.h"

bool run_fairness_test()
{
    g_all_publishers_done = false;

    // Subscriber count scales to the host (max_subscribers tracks it, so the
    // ring count scales too): bounded on a low-core CI box, oversubscribed on
    // a big one. See contention_count().
    int const          NUM_SUBS  = static_cast<int>(contention_count());
    uint32_t const     NUM_MSGS  = 100000 / TSAN_SCALE;

    std::printf("--- Fairness test: 1 pub x %u msgs, %d subs (ring=256, pool=512) ---\n",
                NUM_MSGS, NUM_SUBS);

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = NUM_SUBS;
    cfg.sub_ring_capacity = 256;
    cfg.pool_size         = 512;
    cfg.max_payload_size  = sizeof(Payload);

    char const* shm_name = "/kickmsg_fairness_test";
    kickmsg::SharedMemory::unlink(shm_name);
    auto region = kickmsg::SharedRegion::create(
        shm_name, kickmsg::channel::PubSub, cfg, "fairness");

    g_subscribers_ready    = 0;
    g_subscribers_expected = NUM_SUBS;

    std::vector<SubResult> results(NUM_SUBS);
    std::vector<std::thread> sub_threads;

    for (int i = 0; i < NUM_SUBS; ++i)
    {
        sub_threads.emplace_back([&region, i, &results]()
        {
            results[static_cast<std::size_t>(i)] =
                subscriber_thread_copy(region, i, 1, NUM_MSGS);
        });
    }

    std::thread pub_thread(publisher_thread, std::ref(region), 0, NUM_MSGS);
    pub_thread.join();
    g_all_publishers_done.store(true, std::memory_order_release);

    for (auto& t : sub_threads)
    {
        t.join();
    }

    bool ok = true;
    uint64_t min_recv = UINT64_MAX;
    uint64_t max_recv = 0;

    for (auto const& r : results)
    {
        min_recv = std::min(min_recv, r.received);
        max_recv = std::max(max_recv, r.received);

        if (r.corrupted > 0 or r.bad_pub_id > 0 or r.reordered > 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: corrupt=%" PRIu64 " bad_pid=%"
                         PRIu64 " reorder=%" PRIu64 "\n",
                         r.sub_id, r.corrupted, r.bad_pub_id, r.reordered);
            ok = false;
        }

        // Exact conservation: the readiness barrier means every ring is Live
        // before the first send, and subscribers drain to completion.
        uint64_t accounted = r.received + r.lost + r.corrupted + r.bad_pub_id + r.reordered;
        if (accounted != NUM_MSGS)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: received+lost+corrupt+bad_pid+reorder (%" PRIu64
                         ") != total_sent (%u)!\n",
                         r.sub_id, accounted, NUM_MSGS);
            ok = false;
        }
    }

    std::printf("  Received range: [%" PRIu64 ", %" PRIu64 "] (spread: %" PRIu64 ")\n",
                min_recv, max_recv, max_recv - min_recv);

    if (min_recv == 0)
    {
        std::fprintf(stderr, "  [FAIL] at least one subscriber received 0 messages\n");
        ok = false;
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
