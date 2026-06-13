#include "scheduler.hpp"
#include <csignal>
#include <atomic>
#include <unistd.h>

static std::atomic<bool> g_run{true};
static void onSignal(int) { g_run.store(false, std::memory_order_relaxed); }

int main(void) {
    signal(SIGTERM, onSignal);
    signal(SIGINT,  onSignal);

    // 仅做调度。频率/GPU/电量等遥测与存活判定交给前端 App 自行读取 sysfs / pidof，
    // 守护进程不再写 status.json / daemon.alive。
    Schedule sched;

    while (g_run.load(std::memory_order_relaxed)) {
        pause();     // 纯等信号，主线程零周期唤醒
    }

    return 0;
}
