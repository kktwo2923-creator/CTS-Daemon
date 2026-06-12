# OnePlus V Pro 更新日志

## 260612
- 适配 SM7675(骁龙 7+ Gen3)→ 7P3 配置
- fast(风驰)按机型修正:8E/8G3=scx,8E5/8G5/通用=hmbird
- 修复所有骁龙机型 GPU 最低频率被锁 222 → 可降到 160(6+2 架构通病,更省电)
- 修复簇拓扑错误:8G1(0/4/7)、8G2(0/3/5/7)、7+Gen3(0/3/7)、天玑9300/9400(0/4/7)此前误用 6+2/双簇模板,超大核或中核的频率/调速策略静默失效;各簇频率按 SoC 实际规格重写
- daemon 修复:写 GPU min/max 不再强制把 kgsl governor 钉成 performance(原会废掉 GPU DVFS,使 min 限频形同虚设)
- daemon 修复:GPU pwrlevel 兜底 max 方向写反(目标低于全部档位时误解除上限)、min/max 成功状态分别统计
- daemon 修复:CPU 频率改阻塞写并校验,写失败不再污染去重缓存导致重试被永久跳过
- daemon 修复:配置热重载清空旧簇映射(Policy),避免三簇→双簇切换时残留旧簇号
- 调度改为「跟随系统」:游戏/governor 留空交系统,移除游戏强制 cpuset 抢写
- 黑名单:Scene / MT管理器 / OnePlus V Pro(CTS)不接管
- App:CPU 面板「恢复默认」按钮、AGSL 着色器崩溃兜底
- 装机:签名不一致自动卸旧装新
- 新增 updateJson 在线更新
