#pragma once

// ============================================================
//  SceneDetector — 场景状态（精简版）
//
//  移除了场景识别线程组（Standby/HeavyLoad/Touch 的检测与超时），
//  频率响应改由 governor 的 up/down_rate_limit_us / hispeed_load 处理。
//
//  这里只保留仍在运行路径上用到的部分：
//    - current()         —— appProfileTask 用来跳过熄屏态（保留 Standby 枚举）
//    - triggerAmSwitch() —— LaunchBoost 打开时，前台切换走 AmSwitch（release() 调用）
//    - name()            —— 日志用
//
//  其余 trigger*/feed*/tick* 检测逻辑随线程组一并删除（编译进来却从不执行）。
//  如需回归，可从 git 历史 恢复完整状态机。
// ============================================================

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstring>
#include "Config.hpp"

class SceneDetector {
public:
    enum class Scene {
        None = 0,
        Touch,
        AmSwitch,
        HeavyLoad,
        Standby,
        COUNT
    };

    static const char* name(Scene s) {
        switch (s) {
            case Scene::None:      return "None";
            case Scene::Touch:     return "Touch";
            case Scene::AmSwitch:  return "AmSwitch";
            case Scene::HeavyLoad: return "HeavyLoad";
            case Scene::Standby:   return "Standby";
            default:               return "Unknown";
        }
    }

    SceneDetector() {
        current_.store(Scene::None);
        sceneStart_ = std::chrono::steady_clock::now();
    }

    Scene current() const { return current_.load(); }

    // 前台应用切换（仅 LaunchBoost 开启时经 release() 触发）
    // 返回 true 表示场景确实切换了，调用方据此触发一次频率应用
    bool triggerAmSwitch() {
        std::lock_guard<std::mutex> lk(mtx_);
        Scene cur = current_.load();
        // Standby/HeavyLoad 不被 AmSwitch 覆盖
        if (cur == Scene::Standby)   return false;
        if (cur == Scene::HeavyLoad) return false;
        return transitTo(Scene::AmSwitch);
    }

private:
    // 必须在持锁状态下调用
    bool transitTo(Scene next) {
        Scene cur = current_.load();
        if (cur == next) return false;
        current_.store(next);
        sceneStart_ = std::chrono::steady_clock::now();
        return true;
    }

    std::atomic<Scene> current_;
    std::chrono::steady_clock::time_point sceneStart_;
    std::mutex mtx_;
};
