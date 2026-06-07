#include "scheduler.hpp"
#include "StatusReporter.hpp"
#include "TrialGuard.hpp"
#include <csignal>
#include <atomic>
#include <cstdio>
#include <unistd.h>

// 优雅退出标志:收到 SIGTERM/SIGINT 后退出主循环,析构 StatusReporter(停采样线程)。
static std::atomic<bool> g_run{true};
static void onSignal(int) { g_run.store(false, std::memory_order_relaxed); }

int main(void) {
    // 注册信号:SIGTERM(模块/系统停止)、SIGINT(调试 Ctrl-C)
    signal(SIGTERM, onSignal);
    signal(SIGINT,  onSignal);

#ifdef CTS_TRIAL
    // 体验版:首次启动起算 24 小时,到期则启动失败退出(不进入主循环)。
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

    // 实时状态采集 + 心跳(双电芯默认 2;若实测功耗偏高一倍改成 1)
    StatusReporter reporter(2);
    reporter.start();

    // 主循环:等待退出信号(轻量轮询,1s 一次)
#ifdef CTS_TRIAL
    int trialTick = 0;
#endif
    while (g_run.load(std::memory_order_relaxed)) {
        sleep(1);
#ifdef CTS_TRIAL
        // 运行中每 5 分钟复查一次,跨过 24 小时即自动停止运行。
        if (++trialTick >= 300) {
            trialTick = 0;
            if (!TrialGuard::allowed()) {
                fprintf(stderr, "\n!!! CTS 体验版已到期(24 小时),自动停止运行。\n\n");
                break;
            }
        }
#endif
    }

    reporter.stop();
    return 0;
}
