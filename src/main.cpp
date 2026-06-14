#include "scheduler.hpp"
#include "License.hpp"
#include "Logger.hpp"
#include <csignal>
#include <atomic>
#include <cstdio>
#include <unistd.h>

static std::atomic<bool> g_run{true};
static void onSignal(int) { g_run.store(false, std::memory_order_relaxed); }

// 与授权服务器 LICENSE_SIGNING_SECRET 一致;用于离线校验 App 写入的 license.json 令牌。
static constexpr const char* kSigningSecret =
    "f7d8cf1f160cb8f1f8ee7894749cd39f57a237ac54f0f3dbde841a1334ad608d";

int main(void) {
    signal(SIGTERM, onSignal);
    signal(SIGINT,  onSignal);

    // ===== 授权校验(离线,首次运行读取 App 写入的 license.json) =====
    {
        Logger licLog;
        lic::Result r = lic::check(kSigningSecret);
        if (!r.ok) {
            licLog.Error("授权校验失败: %s", r.reason.c_str());
            fprintf(stderr, "[CTS] 授权校验失败: %s\n", r.reason.c_str());
            return 1;   // 未授权则拒绝运行
        }
        licLog.Info("授权校验通过 (卡密 %s)", r.licenseKey.c_str());
    }

    // 仅做调度。频率/GPU/电量等遥测与存活判定交给前端 App 自行读取 sysfs / pidof，
    // 守护进程不再写 status.json / daemon.alive。
    Schedule sched;

    while (g_run.load(std::memory_order_relaxed)) {
        pause();     // 纯等信号，主线程零周期唤醒
    }

    return 0;
}
