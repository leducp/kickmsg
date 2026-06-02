#include "common.h"
#include "kickmsg/version.h"

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

int main()
{
    std::printf("=== Kickmsg Lock-Free Stress Tests ===\n");
    // Build stamp: confirm which binary is running. __DATE__/__TIME__ is this
    // harness TU's compile time; shm ABI version confirms the layout in use.
    std::printf("kickmsg %s | shm ABI v%u | harness built %s %s\n\n",
                KICKMSG_VERSION_STRING,
                static_cast<unsigned>(kickmsg::VERSION),
                __DATE__, __TIME__);

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
