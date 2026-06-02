#include "scheduler.hpp"
#include "StatusReporter.hpp"
#include <csignal>
#include <atomic>
#include <unistd.h>

// 优雅退出标志:收到 SIGTERM/SIGINT 后退出主循环,析构 StatusReporter(停采样线程)。
static std::atomic<bool> g_run{true};
static void onSignal(int) { g_run.store(false, std::memory_order_relaxed); }

int main(void) {
    // 注册信号:SIGTERM(模块/系统停止)、SIGINT(调试 Ctrl-C)
    signal(SIGTERM, onSignal);
    signal(SIGINT,  onSignal);

    Schedule sched;

    // 实时状态采集 + 心跳(双电芯默认 2;若实测功耗偏高一倍改成 1)
    StatusReporter reporter(2);
    reporter.start();

    // 主循环:等待退出信号(轻量轮询,1s 一次)
    while (g_run.load(std::memory_order_relaxed)) {
        sleep(1);
    }

    reporter.stop();
    return 0;
}
