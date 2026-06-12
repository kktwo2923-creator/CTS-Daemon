#include "scheduler.hpp"
#include "StatusReporter.hpp"
#include "TrialGuard.hpp"
#include <csignal>
#include <atomic>
#include <cstdio>
#include <unistd.h>

static std::atomic<bool> g_run{true};
static void onSignal(int) { g_run.store(false, std::memory_order_relaxed); }

int main(void) {
    signal(SIGTERM, onSignal);
    signal(SIGINT,  onSignal);

#ifdef CTS_TRIAL
    if (!TrialGuard::allowed()) {
        fprintf(stderr, "\n!!! CTS 体验版已到期(24 小时),已停止运行。请使用正式版。\n\n");
        return 1;
    }
    {
        long left = TrialGuard::remainSeconds();
        fprintf(stderr, "[CTS 体验版] 剩余试用约 %ld 小时 %ld 分钟。\n",
                left / 3600, (left % 3600) / 60);
    }
#endif

    Schedule sched;

    StatusReporter reporter(2);
    reporter.start();

    while (g_run.load(std::memory_order_relaxed)) {
#ifdef CTS_TRIAL
        sleep(300);  // 与原 300×1s 检查周期一致；信号会提前打断 sleep
        if (!g_run.load(std::memory_order_relaxed)) break;
        if (!TrialGuard::allowed()) {
            fprintf(stderr, "\n!!! CTS 体验版已到期(24 小时),自动停止运行。\n\n");
            break;
        }
#else
        pause();     // 纯等信号，主线程零周期唤醒
#endif
    }

    reporter.stop();
    return 0;
}
