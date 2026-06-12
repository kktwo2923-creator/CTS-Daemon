#pragma once

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

    // 前台切换触发，返回 true 表示场景确实发生变化
    bool triggerAmSwitch() {
        std::lock_guard<std::mutex> lk(mtx_);
        Scene cur = current_.load();
        if (cur == Scene::Standby)   return false;
        if (cur == Scene::HeavyLoad) return false;
        return transitTo(Scene::AmSwitch);
    }

private:
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
