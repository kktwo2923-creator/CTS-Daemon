# OnePlus V Pro 更新日志

## 260612
- 适配 SM7675(骁龙 7+ Gen3)→ 7P3 配置
- fast(风驰)按机型修正:8E/8G3=scx,8E5/8G5/通用=hmbird
- 修复所有骁龙机型 GPU 最低频率被锁 222 → 可降到 160(6+2 架构通病,更省电)
- 调度改为「跟随系统」:游戏/governor 留空交系统,移除游戏强制 cpuset 抢写
- 黑名单:Scene / MT管理器 / OnePlus V Pro(CTS)不接管
- App:CPU 面板「恢复默认」按钮、AGSL 着色器崩溃兜底
- 装机:签名不一致自动卸旧装新
- 新增 updateJson 在线更新
