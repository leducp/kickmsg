#include "common.h"
#include "kickmsg/version.h"

#include <argparse/argparse.hpp>

// Forward declarations
bool run_treiber_stress();
bool run_subscriber_churn();
bool run_gc_recovery();
bool run_fairness_test();
void run_all_mpmc(TestRunner& runner);
bool run_pool_exhaustion();
bool run_live_repair();
bool run_single_slot_ring();
bool run_subscriber_saturation();

int main(int argc, char** argv)
{
    argparse::ArgumentParser program("kickmsg_stress_test");
    program.add_description(
        "Lock-free shared-memory stress suite. Thread counts scale to the host "
        "CPU; --oversub tunes how hard it contends.");
    program.add_argument("--oversub")
        .help("total contention threads as a percentage of CPU cores "
              "(150 = ~1.5x cores, 50 = light, 400 = heavy)")
        .metavar("PCT")
        .scan<'i', int>()
        .default_value(static_cast<int>(g_oversub_pct));

    try
    {
        program.parse_args(argc, argv);
    }
    catch (std::exception const& e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }

    int pct = program.get<int>("--oversub");
    if (pct > 0)
    {
        g_oversub_pct = static_cast<uint16_t>(std::min(pct, 65535));
    }

    std::printf("=== Kickmsg Lock-Free Stress Tests ===\n");
    // Build stamp + resolved contention so a run is self-describing:
    // __DATE__/__TIME__ is this harness TU's compile time; the ABI version
    // confirms the layout; oversub/cores show the contention actually used.
    std::printf("kickmsg %s | shm ABI v%u | harness built %s %s\n",
                KICKMSG_VERSION_STRING,
                static_cast<unsigned>(kickmsg::VERSION),
                __DATE__, __TIME__);
    std::printf("contention: %u%% of %u cores -> %u threads/side\n\n",
                static_cast<unsigned>(g_oversub_pct),
                std::thread::hardware_concurrency(),
                static_cast<unsigned>(contention_count()));

    TestRunner runner;

    runner.run("treiber_stress",       run_treiber_stress);
    runner.run("subscriber_churn",     run_subscriber_churn);
    runner.run("gc_recovery",          run_gc_recovery);
    runner.run("fairness",             run_fairness_test);
    run_all_mpmc(runner);
    runner.run("pool_exhaustion",      run_pool_exhaustion);
    runner.run("live_repair",          run_live_repair);
    runner.run("single_slot_ring",     run_single_slot_ring);
    runner.run("subscriber_saturation", run_subscriber_saturation);

    return runner.summary();
}
