# CTS — v4.7 死代码清理

## 起因
v4.7 移除场景识别（方案 Z）后，相关代码只是\"不再启动\"，但仍然编译进二进制。
本次只做**删死代码**，不改任何运行逻辑：所有被删内容在当前版本都**没有任何调用方/执行路径**。

验证：用 `g++ -std=c++23 -fsyntax-only -Wall -Wextra -Wshadow` 通过，
改动的四个文件零报错、零警告（仅第三方 Json 库有既有 -Wshadow 警告，与本次无关）。

## 删除清单

### 1. scheduler.hpp（954 → 634 行）
删掉整组**永不启动**的场景识别线程及其私有辅助函数：
- `sceneTickTask` / `screenStateTask` / `loadSamplingTask` / `readProcStat`
- `touchDetectTask` / `openTouchDevices` / `netlinkScreenLoop`

连带删除：
- 成员 `bool netlinkScreenOk`（只在已删的 netlink 代码里赋值）
- 头文件 `<linux/input.h>` / `<linux/netlink.h>` / `<sys/socket.h>`（只被上述代码使用）

保留：`configTriggerTask` / `jsonTriggerTask` / `cpuSetTriggerTask` / `appProfileTask`
（这些是实际启动的线程）以及 `applyScene`/`applyWithProfile`/`Release` 等 AppProfile 路径。

### 2. SceneDetector.hpp（~230 → 77 行）
只保留运行路径仍用到的三个接口：
- `current()` —— appProfileTask 用来跳过熄屏态
- `triggerAmSwitch()` —— LaunchBoost 开启时经 release() 触发
- `name()` —— 日志用

删除只被已删线程调用的状态机方法：
`triggerScreen` / `triggerTouch` / `feedCpuLoad` / `feedBurstLoad` / `tickTimeout` / `parse`，
以及随之无用的 `lastHeavyExit_` / `heavyHits_` 成员。

### 3. Utils.hpp（568 → 217 行）
删掉零调用方的方法：
- `CachedWrite`（连同 `writeCache` / `cacheMutex` / `maxBucketSize`）
- `WriteFile(const char*, const char*)`（O_TRUNC 版，无人用）
- `mkdirRecursive`（只被上面的 WriteFile 调用）
- `InotifyMain` / `exec`
- `getPid` / `getTid` / `getActivity` / `readFrequencies` / `readString`
- `getScreenProperty`（连同 `<sys/system_properties.h>` 头）
- 整组温控读取：`getMaxCpuTemp` / `openZonePath` / `readTemp` / `checkSensorPath` / `thermalPath`

保留：三个 `FileWrite` 重载、`readInt` / `WriteInt` / `sleep_ms` /
`popenShell` / `popenRead` / `getTopApp` / `getTopAppCached`（均有实际调用方）。

### 4. Function.hpp
- 删成员 `std::atomic<int> latestThermalLevel{0}`（**从未有任何地方给它赋值**）
- `gpuFreqGuard` 里基于它的 `thermalActive` 分支恒为 false，已拿掉（逻辑等价）

### 5. CMakeLists.txt
去掉 Release 的 `-ffast-math`。本进程是纯整数 + sysfs IO 的守护进程，
不做浮点运算，该 flag 毫无收益，反而会改变 NaN/Inf 语义、可能引入 UB。

## 未改动（按\"最安全\"原则保留）
JsonConfig.hpp 中对 `SceneFreq` / `SceneCfg` 细项 / `NetlinkCfg` 的**解析**仍在。
它们把数据读进再也没人读的变量，运行时无害（仅配置加载时多花几毫秒），
但跨文件耦合最深、改动风险最高，故本轮保留。

如需进一步清理，可在下一轮删除：
- Config.hpp 的 `SceneFreq` / `NetlinkCfg` 命名空间、`SceneCfg` 除 `enable` 外的字段
- JsonConfig.hpp 的 `parseScenesNode` / `resetSceneFreq` 及其调用点、
  `SceneDetect` / `Netlink` 两个 readBool/readInt 解析块

## 回滚
所有删除内容均可从 git 历史 v4.6 完整恢复（场景识别状态机、温控读取等）。
