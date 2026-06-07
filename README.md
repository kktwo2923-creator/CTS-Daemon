# CoreTurboScheduler (CTS) 

> **一句话**：6+2 架构专用调度配置，省电 + 应用画像 + governor 自主调频。
>
> 参考 [yumi](https://github.com/imacte/yumi) 和 [MW_CpuTurboScheduler](https://github.com/MoWei-2077/MW_CpuTurboScheduler) 的设计思路。

针对芯片：**SM8850 / 骁龙 8 至尊版 / 8 Elite Gen 5（8e5）**
架构：6 × Pegasus 小核 + 2 × Phoenix-Prime 大核

---

## 快速上手

### 1. 文件位置
```
/sdcard/Android/CTS/
├── config.json      # 主配置
├── mode.txt         # 当前模式
└── log.txt          # 运行日志
```

### 2. 切换模式（立即生效）
```bash
echo "powersave"   > /sdcard/Android/CTS/mode.txt   # 最省电（关大核）
echo "balance"     > /sdcard/Android/CTS/mode.txt   # 日常用 ← 推荐
echo "performance" > /sdcard/Android/CTS/mode.txt   # 性能优先
echo "fast"        > /sdcard/Android/CTS/mode.txt   # 风驰（hmbird 调速器）
```

---

## CPU 频率映射（8 Elite Gen 5 / 6+2）

| 集群 | 核心数 | 类型 | 配置变量 | 频率范围 |
|------|--------|------|----------|----------|
| **c0** | 6 | Pegasus 小核 | `freq_min_c0` / `freq_max_c0` | 约 300MHz–3.62GHz |
| **c1** | 2 | Phoenix-Prime 大核 | `freq_min_c1` / `freq_max_c1` | 约 800MHz–4.6GHz |

> 实际档位以你机器的 `/sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies` 为准。

---

## v4.7 调频模型（无 Scenes，governor 自主）

### 设计理念

v4.5 之前用 Scenes（Standby / HeavyLoad / Touch / None）覆盖频率上下限，但实测发现：

1. **CPU 几乎不降频** —— scene 频繁切换重写 `scaling_max_freq`，governor 还没来得及自然回落就被改了
2. **配置复杂** —— 5 个 scene × 4 个簇 × min/max/gov = 60 个字段要管理
3. **响应过激** —— Touch 每秒触发多次，HeavyLoad 一旦触发就 boost，反而抖动费电

参考 [yumi](https://github.com/imacte/yumi) 和 [MW_CpuTurboScheduler](https://github.com/MoWei-2077/MW_CpuTurboScheduler)：

> **CTS 只设上下限和 governor 参数；具体频率响应交给 governor 自己。**

### 三个关键 governor 参数

调度器（walt / schedutil / hmbird）支持下面三类参数，写到 `gov_cN.params` 里：

| 参数 | 单位 | 含义 | 影响 |
|------|------|------|------|
| `up_rate_limit_us` | 微秒 | 升频最小间隔 | **小**=升频快（响应好但费电），**大**=升频慢 |
| `down_rate_limit_us` | 微秒 | 降频最小间隔 | **小**=降频快（省电），**大**=降频慢 |
| `hispeed_load` | 百分比 | 触发 hispeed 的负载阈值 | **小**=容易升频，**大**=不易升频 |

### v4.7 各模式默认参数（参考值）

| mode | up_rate (升频) | down_rate (降频) | hispeed_load | 取向 |
|------|----------|--------------|--------------|------|
| **powersave** | 20000 μs（慢） | 4000 μs（快） | 95% | **极度省电**，降频积极 |
| **balance** | 10000 μs | 4000 μs（快） | 90% | **降频积极**，升频中等 |
| **performance** | 5000 μs（快） | 8000 μs | 85% | 升频积极，降频适中 |
| **fast** | 2000 μs（极快） | 16000 μs（慢） | hmbird 内置 | 高频粘性，**风驰** |

**关键经验值**：
- `down_rate_limit_us` 在 powersave/balance/视频/阅读/音乐模式都设 ≤ 4000，**这是 CPU 下得来的核心**
- `up_rate_limit_us` 在游戏/性能模式设 ≤ 5000 才不卡
- `hispeed_load` 反向：90+ = 不容易升频（省电），80-85 = 容易升频（流畅）

### 如果还是觉得不降频

1. 看你内核当前 governor 实际支持的参数：
   ```bash
   ls /sys/devices/system/cpu/cpufreq/policy0/walt/    # walt
   ls /sys/devices/system/cpu/cpufreq/policy0/schedutil/  # schedutil
   ```
2. 验证写入是否生效：
   ```bash
   cat /sys/devices/system/cpu/cpufreq/policy0/walt/down_rate_limit_us
   ```
3. 实测当前频率：
   ```bash
   cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq
   ```

---

## 配置文件结构（v4.5 精简版）

```json
{
    "meta": { ... },           // 基本信息
    "Policy": { ... },         // CPU 集群索引
    "Function": { ... },       // 功能开关（已大幅精简）
    "package_blacklist": [...],// 黑名单 App
    "models": [ ... ]          // 各场景频率配置
}
```

### 1) meta

```json
"meta": {
    "name": "CTS-8Elite-Gen5",
    "version": "45",
    "author": "你的名字",
    "loglevel": "INFO",
    "soc": "SM8850",
    "arch": "6 Pegasus + 2 Phoenix-Prime"
}
```

### 2) Policy（CPU 集群索引）

```json
"Policy": { "c0": "0", "c1": "6", "c2": "-1", "c3": "-1" }
```

**怎么查？**
```bash
ls /sys/devices/system/cpu/cpufreq/
# 8 Elite Gen 5: policy0 + policy6 → c0=0, c1=6
# 普通 8 Gen 3:   policy0 + policy4 + policy7 → c0=0, c1=4, c2=7
```

### 3) Function（v4.5 精简到只剩 4 项）

```json
"Function": {
    "Cpuset":       { "enable": "true", ... },
    "LaunchBoost":  { "enable": "false" },
    "OfficialMode": { "enable": "false" },
    "Scheduler":    { "enable": "true", "sched_energy_aware": "true", ... }
}
```

> **v4.5 配置精简**：以下模块底层已默认开启，**配置里直接省略**：
> - `SceneDetect` — 场景识别默认开
> - `AppProfile` — 应用画像默认开
> - `GpuFreq` — GPU 频率守护默认开
> - `NetlinkScreen` — Netlink 屏幕监听默认开
>
> 想关掉或调参，再显式声明覆盖即可：
> ```json
> "Function": {
>     "SceneDetect": { "enable": "false" }   // 显式关闭
> }
> ```

### 4) package_blacklist

```json
"package_blacklist": [
    "com.android.systemui",
    "com.android.launcher*",   // 通配符
    "com.baidu.input*"
]
```

### 5) models（核心）

```json
{
    "model_name": "balance",
    "mode": "balance",                                // 对应 mode.txt
    "is_game": false,
    "packages": [],                                    // 空数组 = 基础模型，不参与 App 匹配
    "freq_min_c0": "400000",  "freq_max_c0": "2800000",
    "freq_min_c1": "1000000", "freq_max_c1": "3400000",
    "gpu_min_mhz": "0",       "gpu_max_mhz": "900",
    "CoreOnline": {
        "Core0": "1", "Core1": "1", "Core2": "1", "Core3": "1",
        "Core4": "1", "Core5": "1", "Core6": "1", "Core7": "0"
    },
    "gov_c0": {
        "governor": "walt",
        "params": {
            "hispeed_freq":      "1600000",
            "hispeed_load":      "90",
            "up_rate_limit_us":  "10000",
            "down_rate_limit_us": "4000"
        }
    },
    "gov_c1": {
        "governor": "walt",
        "params": {
            "hispeed_freq":      "2100000",
            "hispeed_load":      "90",
            "up_rate_limit_us":  "10000",
            "down_rate_limit_us": "4000"
        }
    }
}
```

> v4.7 移除了 `Scenes` 字段。如果你的旧 config 还有 `Scenes` 节点，CTS 会**忽略**它（不报错）。

### 风驰模式（fast）+ 游戏模型

`fast` 和 `game_heavy` 模型都用 `hmbird` 调速器，响应比 `walt` 更快：

```json
"gov_c0": {
    "governor": "hmbird",
    "params": {
        "up_rate_limit_us":  "2000",
        "down_rate_limit_us": "16000"
    }
}
```

游戏模型把 `down_rate_limit_us` 调大（16000μs）保持高频粘性，避免战斗中频率震荡。

---

## 常见问题

### Q: 游戏感觉性能不够？
- 直接把游戏包名放进 `game_heavy.packages`，AppProfile 会按 App 一次性把频率拉满
- 也可以新建一个自定义 model（参考 game_heavy），调大 `freq_min_c1`（拉高大核底线）

### Q: CPU 怎么都不降频？
v4.7 已修复。三个常见原因：
1. **基础模型没设 `down_rate_limit_us`** → governor 按内核默认（通常太大）→ CPU 粘高频
2. **AppProfile 退出未还原 SchedParam** → 游戏的 `down_rate_limit_us=16000` 一直生效
3. **scene 频繁切换重写 `scaling_max_freq`** → governor 还没来得及自然回落就被改了

v4.7 解决：
1. 所有模板模型都明确设 `down_rate_limit_us`（powersave/balance=4000，performance=8000）
2. `Release()` 现在会重新写基础 SchedParam（修复了游戏退出后 rate_limit 不还原的 bug）
3. 移除场景切换路径，CPU 频率上下限不再被频繁重写

### Q: 配置改了没生效？
1. 看日志：`cat /sdcard/Android/CTS/log.txt`
2. JSON 是否合法（推荐用 `python3 -m json.tool config.json` 验证）
3. `loglevel` 改 `DEBUG` 看更详细的写入路径
4. 验证 governor 参数实际生效：`cat /sys/devices/system/cpu/cpufreq/policy0/walt/down_rate_limit_us`

### Q: 写 `"true"` 还是 `true`？
- v4.4+ 两种都行，CTS 内部容忍。**推荐统一写 `"true"`**（key 和 value 都加引号），少记一个规则

### Q: 哪些 App 算游戏？
- `game_heavy.packages` 已内置 25 款主流大型手游（王者、和平、原神、星铁、绝区零、明日方舟等）
- 加新游戏直接往 `packages` 数组里加包名

---

## v4.7 更新摘要

### 🔥 重大改动：移除场景识别（Z 方案）
- **删除 Scenes 节点** — 配置不再需要写 `Standby`/`HeavyLoad`/`Touch`/`AmSwitch`/`None`
- **删除 4 个场景检测线程** — `sceneTickTask` / `screenStateTask` / `loadSamplingTask` / `touchDetectTask` 不再启动
- **频率响应交给 governor** — 用 `up_rate_limit_us` / `down_rate_limit_us` / `hispeed_load` 控制升降频速度
- 参考 [yumi](https://github.com/imacte/yumi) 和 [MW_CpuTurboScheduler](https://github.com/MoWei-2077/MW_CpuTurboScheduler)

### 🆕 配置增强
- 所有模板模型现在都明确设置 `up_rate_limit_us` 和 `down_rate_limit_us`
- powersave/balance/video/reading/music 的 `down_rate_limit_us` ≤ 4000μs，**这是 CPU 真正能降频的关键**
- 游戏/性能模式的 `up_rate_limit_us` ≤ 5000μs，保证响应度
- 风驰模式 fast 用 `down_rate_limit_us=16000` 维持高频粘性

### 🐛 关键 Bug 修复
**1. CPU 退出 AppProfile 后 SchedParam 不还原**
游戏的 `up_rate_limit_us=2000` / `down_rate_limit_us=16000` 在退出游戏后**残留**在 governor sysfs 里，
导致回桌面后 CPU 仍然按游戏的频率策略响应，"怎么都不降频"。
现在 `Release()` 会重新写基础 mode 的 SchedParam。

**2. 场景切换频繁导致 governor 来不及降频**
scene 每秒切换多次 → 重写 `scaling_max_freq` → governor 看到上限变化重新评估 →
还没等 down_rate_limit_us 触发就被新的 scene 改了。v4.7 移除这条路径。

### 🛠 简化
- 4 个场景检测线程移除，进程线程数减少 30%
- `applyAppProfile` 的频率回退从三级简化为二级（AppProfile → Performances）
- 旧的 SceneDetector / SceneCfg 代码保留但不启用，可后续回归

### 🐛 之前版本的修复（继续生效）
- v4.6: GPU 频率控制不生效（writeSysfsLocked 不 chmod、SM8850 sysfs 路径未覆盖）
- v4.5: 类卡顿 boost、8 Elite Gen 5 频率档
- v4.4: qlib::string `operator==` 越堆 UB、日志输出顺序乱
- v4.3: GpuFreq::enable 编译报错、popenShell 缺花括号
- v4.1: 配置全字符串值不被识别、currentMatch 多线程未同步、退出游戏大核不关

---

**协议**: GPL-3.0
