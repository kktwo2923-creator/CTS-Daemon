#include "scheduler.hpp"
#include "StatusReporter.hpp"
#include <csignal>
#include <atomic>
#include <unistd.h>

static std::atomic<bool> g_run{true};
static void onSignal(int) { g_run.store(false, std::memory_order_relaxed); }

int main(void) {
    signal(SIGTERM, onSignal);
    signal(SIGINT,  onSignal);

    Schedule sched;

    StatusReporter reporter(2);
    reporter.start();

    while (g_run.load(std::memory_order_relaxed)) {
        pause();     // 纯等信号，主线程零周期唤醒
    }

    reporter.stop();
    return 0;
}
