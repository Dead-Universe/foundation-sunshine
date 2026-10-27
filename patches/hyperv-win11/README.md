# Sunshine 维护补丁

当前基线：`AlkaidLab/foundation-sunshine` 的 `880f1efed6bd0c18871a78d0f61185a7381cda5f`。
当前运行源码：`051349744dc98e91368e3a8f6562a4f037c4b241`。
补丁顺序及 SHA256 见 [series.json](series.json)。

| 顺序 | 补丁 | 保留原因 |
| --- | --- | --- |
| 1 | `0001-hyperv-vdd-capture-recovery.patch` | 合并原 1/2/3/4/6/7：采集选择、DXGI 错误处理、失败诊断、显示恢复、空闲唤醒与 Auto 输出恢复属于同一采集生命周期 |
| 2 | `0002-nvenc-delayed-completion.patch` | 编码完成事件迟到时有界等待，避免提前销毁仍有待完成帧的编码器 |
| 3 | `0003-hyperv-console-continuity.patch` | Core 固定在服务会话 0；控制台输入/显示辅助进程跟随会话；迁移刷新资源，新匿名控制台重建停止产帧的显示器 |
| 4 | `0004-offline-vbcable-installer.patch` | 安装器包含标准 VB-CABLE 官方完整包；已有驱动不重复安装，卸载 Sunshine 不删除共享声卡 |

2026-10-27 整理将服务端九份有效补丁合为四份，删除四份已撤回/失败的补丁文件。新的四补丁在虚拟机临时 Git 索引独立重放后，结果树仍严格等于 `c9cb8e9bb68faa49825274c6d686e83be27ca6d0`；真实索引、源码、运行二进制和配置均未更改，因此沿用相同源码既有的构建及有限媒体验收，不需要为文件重组重启服务。

有效修复没有被上游完整覆盖；本轮删除的是失败方案，合并的是同一功能的增量补丁。必要的诊断日志和回归测试一并保留。Moonlight 两项独立客户端修复另见 [moonlight/README.md](moonlight/README.md)。

后续更新先核对上游是否已包含修复，只重放仍必要的补丁。宿主机 `sunshine/` 保持干净上游快照；实际 Git 操作、编译和依赖全部在虚拟机 `D:\Tools`，网络下载使用当前宿主机网关的 `:7897` 代理。

核验命令：

```powershell
.\tools\Use-GuestPatchSeries.ps1 -Mode Verify
```

既有验证为 34 项 CTest 零失败，569 项聚合条目中 21 项按条件跳过；最新稳定版 Moonlight VPlus v6.5.3 硬解完成解锁、增强会话双向切换、两次停止重连及窗口/全屏切换。音频初始化与 Opus 输出正常，听感和长期稳定性未独立验收，未宣称满足 144fps 性能目标。

失败方案的原因和原提交保留在 Git 历史及项目调查记录中，不再保留可误应用的补丁副本。源码维护分支是 [maintenance/hyperv-win11-20261027](https://github.com/Dead-Universe/foundation-sunshine/tree/maintenance/hyperv-win11-20261027)。
