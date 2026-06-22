#include "common.h"

uint16_t g_oversub_pct = 150;

uint16_t contention_count()
{
    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0)
    {
        hw = 4;
    }
    uint32_t total    = (hw * g_oversub_pct + 99) / 100;  // ceil(hw * pct / 100)
    uint32_t per_side = std::max<uint32_t>(2, (total + 1) / 2);
    return static_cast<uint16_t>(std::min<uint32_t>(per_side, UINT16_MAX));
}

std::atomic<bool> g_all_publishers_done{false};

// Readiness barrier: subscribers signal after construction, publishers wait.
// Every ring is then Live for the whole run, making per-subscriber
// accounting exact.  Scenarios must reset both before spawning threads.
std::atomic<int> g_subscribers_ready{0};
int g_subscribers_expected{0};

void wait_subscribers_ready()
{
    while (g_subscribers_ready.load(std::memory_order_acquire) < g_subscribers_expected)
    {
        kickmsg::yield();
    }
}

// Messages a send_bounded() actually committed, and publishers that gave up.
// Conservation oracles use g_published (not the nominal count) because a
// publisher may stop early under sustained backpressure.  Reset per scenario.
std::atomic<uint64_t> g_published{0};
std::atomic<uint64_t> g_publisher_giveups{0};

// A tight pool can stay exhausted indefinitely: slots recycle only on
// eviction or teardown, never on consume, so once every publisher is blocked
// in allocate() nothing can free a slot.  A publisher must therefore bound
// its EAGAIN retry rather than spin forever.  Returns false if the pool
// stayed full past the deadline; the caller stops and books a giveup.
// 5 s of unbroken EAGAIN never happens on a healthy run (sends clear in
// microseconds) -- only a genuine deadlock reaches it.  Override via
// KICKMSG_SEND_GIVEUP_MS (the test suite uses a small value to exercise the
// giveup path deterministically).
milliseconds send_giveup_deadline()
{
    static milliseconds const deadline = []() -> milliseconds
    {
        char const* env = std::getenv("KICKMSG_SEND_GIVEUP_MS");
        if (env != nullptr and *env != '\0')
        {
            long v = std::atol(env);
            if (v > 0)
            {
                return milliseconds{v};
            }
        }
        return 5s;
    }();
    return deadline;
}

bool send_bounded(kickmsg::Publisher& pub, Payload const& msg, int pub_id)
{
    nanoseconds start = kickmsg::monotonic_ns();
    int32_t rc;
    while ((rc = pub.send(&msg, sizeof(msg))) < 0)
    {
        if (rc != -EAGAIN)
        {
            std::fprintf(stderr, "  [FATAL] publisher %d: send() returned %d\n", pub_id, rc);
            std::abort();
        }
        if (kickmsg::elapsed_time(start) >= send_giveup_deadline())
        {
            g_publisher_giveups.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        kickmsg::sleep(0ns);
    }
    g_published.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void publisher_thread(kickmsg::SharedRegion& region, int pub_id, uint32_t count)
{
    kickmsg::Publisher pub{region};

    wait_subscribers_ready();

    for (uint32_t i = 0; i < count; ++i)
    {
        Payload msg;
        msg.magic  = Payload::MAGIC;
        msg.pub_id = static_cast<uint32_t>(pub_id);
        msg.seq    = i;
        msg.checksum = compute_checksum(msg);

        if (not send_bounded(pub, msg, pub_id))
        {
            break;
        }
    }
}

void validate_payload(Payload const& msg, int num_pubs, uint64_t ring_pos,
                             std::vector<uint32_t>& last_seq,
                             std::vector<uint64_t>& last_pos,
                             SubResult& result,
                             MsgTrace* trace, std::size_t& trace_pos)
{
    if (msg.magic != Payload::MAGIC)
    {
        ++result.corrupted;
        return;
    }
    if (msg.checksum != compute_checksum(msg))
    {
        ++result.corrupted;
        return;
    }
    if (msg.pub_id >= static_cast<uint32_t>(num_pubs))
    {
        ++result.bad_pub_id;
        return;
    }

    trace[trace_pos % TRACE_SIZE] = {msg.pub_id, msg.seq, ring_pos};
    ++trace_pos;

    auto& prev_seq = last_seq[msg.pub_id];
    auto& prev_pos = last_pos[msg.pub_id];

    if (prev_seq != UINT32_MAX and msg.seq <= prev_seq)
    {
        auto delta = static_cast<int32_t>(prev_seq) - static_cast<int32_t>(msg.seq);
        std::fprintf(stderr, "  [REORDER] sub%d: pub %u seq %u @pos %" PRIu64
                     " after prev seq %u @pos %" PRIu64
                     " (delta=%d, lost=%" PRIu64 ", recv=%" PRIu64 ")\n",
                     result.sub_id, msg.pub_id, msg.seq, ring_pos,
                     prev_seq, prev_pos,
                     delta, result.lost, result.received);

        if (result.reordered == 0)
        {
            std::fprintf(stderr, "  Recent messages (oldest first):\n");
            std::size_t start = 0;
            if (trace_pos > TRACE_SIZE)
            {
                start = trace_pos - TRACE_SIZE;
            }
            for (std::size_t i = start; i < trace_pos; ++i)
            {
                auto& t = trace[i % TRACE_SIZE];
                char const* marker = "";
                if (t.pub_id == msg.pub_id)
                {
                    marker = " <--";
                }
                std::fprintf(stderr, "    [%zu] pub %u seq %u @pos %" PRIu64 "%s\n",
                             i, t.pub_id, t.seq, t.ring_pos, marker);
            }
        }

        ++result.reordered;
        return;
    }
    prev_seq = msg.seq;
    prev_pos = ring_pos;

    ++result.received;
}

SubResult subscriber_thread_copy(kickmsg::SharedRegion& region, int sub_id,
                                        int num_pubs, uint32_t /*msgs_per_pub*/)
{
    kickmsg::Subscriber sub{region};
    g_subscribers_ready.fetch_add(1, std::memory_order_release);

    SubResult result{};
    result.sub_id = sub_id;

    std::vector<uint32_t> last_seq(static_cast<std::size_t>(num_pubs), UINT32_MAX);
    std::vector<uint64_t> last_pos(static_cast<std::size_t>(num_pubs), UINT64_MAX);
    MsgTrace trace[TRACE_SIZE]{};
    std::size_t trace_pos = 0;

    auto const timeout = milliseconds{500};

    while (true)
    {
        auto sample = sub.receive(timeout);
        if (not sample)
        {
            if (g_all_publishers_done)
            {
                // Null can mean retry budget burned on evicted entries;
                // only null with no lost() progress proves ring-empty.
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

        if (sample->len() != sizeof(Payload))
        {
            ++result.corrupted;
            continue;
        }

        Payload msg;
        std::memcpy(&msg, sample->data(), sizeof(msg));
        validate_payload(msg, num_pubs, sample->ring_pos(),
                         last_seq, last_pos, result, trace, trace_pos);
    }

    result.lost = sub.lost();
    return result;
}

SubResult subscriber_thread_zerocopy(kickmsg::SharedRegion& region, int sub_id,
                                            int num_pubs, uint32_t /*msgs_per_pub*/)
{
    kickmsg::Subscriber sub{region};
    g_subscribers_ready.fetch_add(1, std::memory_order_release);

    SubResult result{};
    result.sub_id = sub_id;

    std::vector<uint32_t> last_seq(static_cast<std::size_t>(num_pubs), UINT32_MAX);
    std::vector<uint64_t> last_pos(static_cast<std::size_t>(num_pubs), UINT64_MAX);
    MsgTrace trace[TRACE_SIZE]{};
    std::size_t trace_pos = 0;

    auto const timeout = milliseconds{500};

    while (true)
    {
        auto view = sub.receive_view(timeout);
        if (not view)
        {
            if (g_all_publishers_done)
            {
                // Same caveat as the copy path.
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

        if (view->len() != sizeof(Payload))
        {
            ++result.corrupted;
            continue;
        }

        Payload msg;
        std::memcpy(&msg, view->data(), sizeof(msg));
        validate_payload(msg, num_pubs, view->ring_pos(),
                         last_seq, last_pos, result, trace, trace_pos);
    }

    result.lost = sub.lost();
    return result;
}

// No-crash oracle: GC work not explained by observed publisher drops means
// the normal path leaked and GC masked it -- fail.  Each stall-steal leaks
// at most one slot ref and books at least one drop, so reclaimed <= drops
// is the exact budget.
bool verify_gc_zero(kickmsg::SharedRegion& region, kickmsg::channel::Config const& cfg)
{
    bool ok = true;

    uint64_t dropped = 0;
    for (uint32_t i = 0; i < cfg.max_subscribers; ++i)
    {
        auto* ring = kickmsg::sub_ring_at(region.base(), region.header(), i);
        dropped += ring->dropped_count.load(std::memory_order_acquire);
    }

    std::size_t repaired = region.repair_locked_entries();
    if (repaired != 0)
    {
        std::fprintf(stderr, "  [FAIL] repair_locked_entries fixed %zu entries on a no-crash run\n",
                     repaired);
        ok = false;
    }

    std::size_t reclaimed = region.reclaim_orphaned_slots();
    if (reclaimed > dropped)
    {
        std::fprintf(stderr, "  [FAIL] reclaim_orphaned_slots recovered %zu slots on a no-crash run "
                     "(only %" PRIu64 " publisher drops can account for steal residue)\n",
                     reclaimed, dropped);
        ok = false;
    }
    else if (reclaimed != 0)
    {
        std::printf("  [NOTE] %zu slot(s) reclaimed, within the %" PRIu64
                    "-drop steal budget (stalled-publisher steal residue)\n",
                    reclaimed, dropped);
    }

    return ok;
}

bool verify_pool_free(kickmsg::SharedRegion& region, kickmsg::channel::Config const& cfg)
{
    auto* base = region.base();
    auto* hdr  = region.header();

    std::vector<bool> seen(cfg.pool_size, false);
    uint32_t count  = 0;
    uint32_t top    = kickmsg::tagged_idx(hdr->free_top.load(std::memory_order_acquire));

    while (top != kickmsg::INVALID_SLOT)
    {
        if (top >= cfg.pool_size)
        {
            std::fprintf(stderr, "  [FAIL] free stack contains out-of-range index %u\n", top);
            return false;
        }
        if (seen[top])
        {
            std::fprintf(stderr, "  [FAIL] free stack contains duplicate slot %u\n", top);
            return false;
        }
        seen[top] = true;
        ++count;

        auto* slot = kickmsg::slot_at(base, hdr, top);
        top = slot->next_free;
    }

    if (count != cfg.pool_size)
    {
        std::fprintf(stderr, "  [FAIL] free stack has %u slots, expected %zu (leak!)\n",
                     count, cfg.pool_size);
        return false;
    }
    return true;
}

bool verify_rings_inactive(kickmsg::SharedRegion& region, kickmsg::channel::Config const& cfg)
{
    auto* base = region.base();
    auto* hdr  = region.header();

    for (uint32_t i = 0; i < cfg.max_subscribers; ++i)
    {
        auto* ring = kickmsg::sub_ring_at(base, hdr, i);
        uint32_t packed = ring->state_flight.load(std::memory_order_acquire);
        if (kickmsg::ring::get_state(packed) != kickmsg::ring::Free)
        {
            std::fprintf(stderr, "  [FAIL] ring %u not Free after test\n", i);
            return false;
        }
        if (kickmsg::ring::get_in_flight(packed) != 0)
        {
            std::fprintf(stderr, "  [FAIL] ring %u has in_flight=%u after test\n",
                         i, kickmsg::ring::get_in_flight(packed));
            return false;
        }
    }
    return true;
}

bool verify_refcounts_zero(kickmsg::SharedRegion& region, kickmsg::channel::Config const& cfg)
{
    auto* base = region.base();
    auto* hdr  = region.header();

    for (uint32_t i = 0; i < cfg.pool_size; ++i)
    {
        auto* slot = kickmsg::slot_at(base, hdr, i);
        uint32_t rc = slot->refcount;
        if (rc != 0)
        {
            std::fprintf(stderr, "  [FAIL] slot %u has refcount %u (expected 0)\n", i, rc);
            return false;
        }
    }
    return true;
}
