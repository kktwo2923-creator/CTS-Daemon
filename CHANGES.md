# CTS — GPU 频率控制修复

## 起因
用户报告："GPU 频率控制没有生效"。深入排查后定位为多个叠加问题。

## 修复的 Bug

### Bug A (最关键): `writeSysfs` 不会 chmod
CPU 路径用的 `utils.FileWrite` 在 `open()` 失败时会 `chmod 0666` 然后重试。
但 GPU 路径专用的 `writeSysfs` **没有这个 fallback** —— 直接 `return false`。

骁龙 8 Elite Gen 5（SM8850）等新 SoC 的 Adreno sysfs 默认权限通常是 `0444`/`0644`，
导致 root 进程也可能 EACCES。表现是：
- `tryGpuPath` 误判此路径"不可用"
- 链式跳过所有 4 个候选路径
- `gpuFreqControl` 输出"GPU频率设置可能未完全生效"
- 日志看起来一切正常，但 GPU 实际频率纹丝不动

**修法**：`writeSysfs` 也走 chmod 0666 fallback，与 `utils.FileWrite` 对齐。

### Bug B: `applyCurrentMode` / `applyBaseMode` 不应用 GPU
原版只有 `applyAppProfile` 调用 `gpuFreqControlCustom`。游戏退出后回桌面：
- AppProfile 不再匹配 → 走 `applyCurrentMode`/`applyBaseMode` 路径
- 这条路径**完全不碰 GPU**
- 必须等 `gpuFreqGuard` 下一轮（最长 15s）才把 GPU 还原到基础 mode 设定
- 期间 GPU 卡在游戏的高频上**继续耗电**

**修法**：新增 `applyBaseGpu()` helper，在 `applyBaseMode` 末尾调用，
确保 GPU 频率与 CPU 频率同步还原。去抖由 gpuFreqGuard 的回读纠偏兜底。

### Bug C: SM8850 / Adreno 8x 的新 sysfs 路径未覆盖
原版只尝试 4 个老路径：
```
/sys/kernel/gpu/gpu_max_clock                  (Mali/Exynos)
/sys/class/kgsl/kgsl-3d0/devfreq/max_freq      (旧 Adreno)
/sys/class/kgsl/kgsl-3d0/max_gpuclk            (旧 Adreno)
/sys/class/kgsl/kgsl-3d0/max_clock_mhz         (新一点 Adreno)
```

但新内核（SM8850 等）的标准路径是：
```
/sys/class/devfreq/3d00000.qcom,kgsl-3d0/max_freq
```

旧的 `kgsl/kgsl-3d0/devfreq/` 是软链，在新内核可能不存在。

**修法**：把新 devfreq 路径放到尝试列表**最前面**（按内核版本新→旧排序）。
同时把 `readKgslAvailableFreqs` 也扩展为多路径兜底。

### Bug D: 两个频率都为 0 时仍然每 3s 报告"未生效"
基础 mode 没配 GPU、AppProfile 也没配 GPU 时，
`gpuFreqGuard` 仍然每 15s 走一遍所有路径再失败一次，
日志看起来像 GPU 模块坏了。

**修法**：`mhzMin <= 0 && mhzMax <= 0` 时直接 sleep continue，跳过整轮。
这是合法状态（用户主动不配 GPU），不应该报警。

### Bug E (诊断改进): 三条路径全部失败时无明确告警
原版静默地写"GPU频率设置可能未完全生效"，但用户不知道是权限问题、驱动问题还是路径问题。

**修法**：`tryGpuPwrlevel` 也失败 → 显式 `logger.Warn` 提示用户检查
root/SELinux/驱动支持。`writeSysfs` 失败时记录 errno。

## 验证
NDK syntax-check 通过，无回归错误。

## 用户排查建议
如果 v4.6 升级后 GPU 频率仍然没生效，请：

1. **看日志**：`grep -i gpu /sdcard/Android/CTS/log.txt`
   - 看到 `GPU频率写入全部失败` → 权限/SELinux 问题，确认你是 root + Magisk 模块上下文
   - 看到 `GPU sysfs 打开失败` 配合 errno=13 → EACCES，selinux denial
   - 看到 `GPU频率已设置` 但 GPU 实际频率不变 → 写入了但被厂商驱动覆盖（联发科和某些 OEM 内核会这样）

2. **手动验证 sysfs 存在哪些路径**：
   ```bash
   ls /sys/class/devfreq/                       # 找 *kgsl-3d0
   ls /sys/class/kgsl/kgsl-3d0/                 # 看有没有 devfreq、max_gpuclk 等
   cat /sys/class/devfreq/*kgsl-3d0/available_frequencies  # 看实际档位
   ```

3. **临时手动测试**：以 root 执行
   ```bash
   echo 800000000 > /sys/class/devfreq/*kgsl-3d0/max_freq
   cat /sys/class/devfreq/*kgsl-3d0/cur_freq
   ```
   如果 cur_freq 跟随你 echo 的值变化 → CTS 路径里加上这个 sysfs。
   如果 echo 报 "Permission denied" → 你需要 magiskpolicy 允许写入。
