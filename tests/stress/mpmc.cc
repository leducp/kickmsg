#include "common.h"

bool run_stress_test(TestConfig const& tc)
{
    char const* zc_label = "";
    if (tc.use_zerocopy)
    {
        zc_label = " (zero-copy)";
    }
    std::printf("--- Stress test%s: %d pubs x %u msgs, %d subs, pool=%zu, ring=%zu ---\n",
                zc_label,
                tc.num_publishers, tc.msgs_per_pub, tc.num_subscribers,
                tc.pool_size, tc.ring_capacity);

    g_all_publishers_done = false;
    g_subscribers_ready   = 0;
    g_subscribers_expected = tc.num_subscribers;

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = tc.max_subs;
    cfg.sub_ring_capacity = tc.ring_capacity;
    cfg.pool_size         = tc.pool_size;
    cfg.max_payload_size  = sizeof(Payload);

    char const* shm_name = "/kickmsg_stress_test";
    kickmsg::SharedMemory::unlink(shm_name);
    auto region = kickmsg::SharedRegion::create(
        shm_name, kickmsg::channel::PubSub, cfg, "stress_test");

    nanoseconds t0 = kickmsg::monotonic_ns();

    std::vector<std::thread> sub_threads;
    std::vector<SubResult> sub_results(static_cast<std::size_t>(tc.num_subscribers));

    for (int i = 0; i < tc.num_subscribers; ++i)
    {
        sub_threads.emplace_back([&region, i, &sub_results, &tc]()
        {
            auto fn = subscriber_thread_copy;
            if (tc.use_zerocopy)
            {
                fn = subscriber_thread_zerocopy;
            }
            sub_results[static_cast<std::size_t>(i)] =
                fn(region, i, tc.num_publishers, tc.msgs_per_pub);
        });
    }

    std::vector<std::thread> pub_threads;
    for (int i = 0; i < tc.num_publishers; ++i)
    {
        pub_threads.emplace_back(publisher_thread, std::ref(region), i, tc.msgs_per_pub);
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

    nanoseconds t1 = kickmsg::monotonic_ns();
    int64_t elapsed_ms = std::chrono::duration_cast<milliseconds>(t1 - t0).count();

    uint64_t total_sent = static_cast<uint64_t>(tc.num_publishers) * tc.msgs_per_pub;

    bool all_ok = true;

    char const* mode_label = "copy";
    if (tc.use_zerocopy)
    {
        mode_label = "zerocopy";
    }
    std::printf("  Config: %d pub, %d sub, %s\n",
                tc.num_publishers, tc.num_subscribers, mode_label);
    std::printf("  Elapsed: %" PRId64 " ms, total published: %" PRIu64 "\n",
                elapsed_ms, total_sent);
    std::printf("  %-6s %10s %10s %10s %10s %10s\n",
                "sub", "received", "lost", "corrupt", "bad_pid", "reorder");

    for (auto const& r : sub_results)
    {
        std::printf("  sub%-3d %10" PRIu64 " %10" PRIu64 " %10" PRIu64
                    " %10" PRIu64 " %10" PRIu64 "\n",
                    r.sub_id, r.received, r.lost, r.corrupted,
                    r.bad_pub_id, r.reordered);

        if (r.corrupted > 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: %" PRIu64 " corrupted messages!\n",
                         r.sub_id, r.corrupted);
            all_ok = false;
        }
        if (r.bad_pub_id > 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: %" PRIu64 " bad publisher IDs!\n",
                         r.sub_id, r.bad_pub_id);
            all_ok = false;
        }
        if (r.reordered > 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: %" PRIu64 " reordered messages!\n",
                         r.sub_id, r.reordered);
            all_ok = false;
        }
        if (r.received == 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: received 0 messages!\n", r.sub_id);
            all_ok = false;
        }
        // Exact conservation: the readiness barrier guarantees every ring is
        // Live before the first send, so each subscriber's ring sees every
        // message. Every ring position is consumed (received / corrupted /
        // bad_pub_id / reordered, exactly one bucket each) or counted in
        // lost; any other total means messages vanished or were duplicated.
        uint64_t accounted = r.received + r.lost + r.corrupted + r.bad_pub_id + r.reordered;
        if (accounted != total_sent)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: received+lost+corrupt+bad_pid+reorder (%" PRIu64
                         ") != total_sent (%" PRIu64 ")!\n",
                         r.sub_id, accounted, total_sent);
            all_ok = false;
        }
    }

    all_ok &= verify_gc_zero(region, cfg);
    all_ok &= verify_refcounts_zero(region, cfg);
    all_ok &= verify_pool_free(region, cfg);
    all_ok &= verify_rings_inactive(region, cfg);

    region.unlink();

    if (all_ok)
    {
        std::printf("  [PASS]\n\n");
    }
    else
    {
        std::printf("  [FAIL]\n\n");
    }
    return all_ok;
}

void run_all_mpmc(TestRunner& runner)
{
    // Copy-based receive tests
    {
        TestConfig tc;
        tc.num_publishers  = 2;
        tc.num_subscribers = 4;
        tc.msgs_per_pub    = 100000 / TSAN_SCALE;
        tc.pool_size       = 256;
        tc.ring_capacity   = 64;
        tc.max_subs        = 8;
        runner.run("mpmc 2p/4s", [&]{ return run_stress_test(tc); });
    }

    {
        int const n = contention_count();
        TestConfig tc;
        tc.num_publishers  = n;
        tc.num_subscribers = n;
        tc.msgs_per_pub    = 50000 / TSAN_SCALE;
        tc.pool_size       = 128;
        tc.ring_capacity   = 32;
        tc.max_subs        = static_cast<std::size_t>(n);
        runner.run("mpmc contended (pool 128)", [&]{ return run_stress_test(tc); });
    }

    {
        TestConfig tc;
        tc.num_publishers  = 1;
        tc.num_subscribers = 1;
        tc.msgs_per_pub    = 500000 / TSAN_SCALE;
        tc.pool_size       = 64;
        tc.ring_capacity   = 16;
        tc.max_subs        = 2;
        runner.run("mpmc 1p/1s", [&]{ return run_stress_test(tc); });
    }

    // High contention: small pool, heavy overflow
    {
        int const n = contention_count();
        TestConfig tc;
        tc.num_publishers  = n;
        tc.num_subscribers = n;
        tc.msgs_per_pub    = 20000 / TSAN_SCALE;
        tc.pool_size       = 32;
        tc.ring_capacity   = 8;
        tc.max_subs        = static_cast<std::size_t>(n);
        runner.run("mpmc contended (pool 32, tiny)", [&]{ return run_stress_test(tc); });
    }

    // Zero-copy receive tests -- exercises SampleView pin CAS,
    // refcount increment/decrement, and destructor release path
    {
        TestConfig tc;
        tc.num_publishers  = 2;
        tc.num_subscribers = 4;
        tc.msgs_per_pub    = 100000 / TSAN_SCALE;
        tc.pool_size       = 256;
        tc.ring_capacity   = 64;
        tc.max_subs        = 8;
        tc.use_zerocopy    = true;
        runner.run("mpmc 2p/4s zerocopy", [&]{ return run_stress_test(tc); });
    }

    {
        int const n = contention_count();
        TestConfig tc;
        tc.num_publishers  = n;
        tc.num_subscribers = n;
        tc.msgs_per_pub    = 50000 / TSAN_SCALE;
        tc.pool_size       = 128;
        tc.ring_capacity   = 32;
        tc.max_subs        = static_cast<std::size_t>(n);
        tc.use_zerocopy    = true;
        runner.run("mpmc contended (pool 128) zerocopy", [&]{ return run_stress_test(tc); });
    }

    // High contention zero-copy: small pool, heavy overflow
    {
        int const n = contention_count();
        TestConfig tc;
        tc.num_publishers  = n;
        tc.num_subscribers = n;
        tc.msgs_per_pub    = 20000 / TSAN_SCALE;
        tc.pool_size       = 32;
        tc.ring_capacity   = 8;
        tc.max_subs        = static_cast<std::size_t>(n);
        tc.use_zerocopy    = true;
        runner.run("mpmc contended (pool 32, tiny) zerocopy", [&]{ return run_stress_test(tc); });
    }
}
