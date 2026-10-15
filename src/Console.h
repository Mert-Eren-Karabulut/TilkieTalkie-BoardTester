#pragma once

// Serial console (docs/CONSOLE.md). Bench-only commands are compiled with -DTT_BENCH.
namespace Console
{
    // loop(): reads and runs at most one command line per pass.
    void service();

#ifdef TT_BENCH
    // setup(), right after battery.begin(): prints the result of a pending `sleeptest`.
    void reportSleepTestIfAny();
    // loop(), every phase: one `chargemon` line per second while it runs.
    void serviceChargeMonitor();
    // loop(), full-run phase: learning-cycle discharge floor (`learndis`).
    void serviceLearnFloor();
#endif
}
