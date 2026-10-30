# Sunshine 维护补丁

当前上游基线：`AlkaidLab/foundation-sunshine` 的 `d549e16a3fa0887d26beb98eaa40785c1814b353`。
当前已编译并通过原生测试的源码：`5e832dd884695df3ca2d322cc25cabeeeaaf15c4`。
补丁顺序及 SHA256 见 [series.json](series.json)。

| 顺序 | 补丁 | 保留功能 |
| --- | --- | --- |
| 1 | `0001-hyperv-vdd-capture-recovery.patch` | VDD 采集选择、失败诊断、显示恢复、空闲唤醒与 Auto 输出恢复 |
| 2 | `0002-nvenc-delayed-completion.patch` | 编码完成事件迟到时有界等待，避免提前销毁未完成帧 |
| 3 | `0003-hyperv-console-continuity.patch` | 服务会话 0 Core、控制台输入/显示辅助进程和匿名会话迁移后的采集重建 |
| 4 | `0004-offline-vbcable-installer.patch` | 安装器包含标准 VB-CABLE 原包，保留已有和共享声卡 |

2026-10-27 先将原九补丁合为四份，随后清理内部残留：移除只写不读的输入错误字段、恒假内部编码器探测替代路径及最终失败清理后的两次无效赋值；保留实际 SendInput、正常采集回退和通用编码器验证覆盖接口。代码净减少 49 行，不计 NVENC 行尾格式恢复。NVENC 补丁恢复上游 CRLF，文件从 6833 字节降为 4263 字节；等待逻辑和回归测试不变。虚拟声卡补丁字节未变，Moonlight 两补丁保持原样。

本轮跟进上游四个新提交，在最新核对基线重新生成四份独立补丁；临时 Git 索引逐份重放后结果树严格等于 `5a6744e5e6bbd2b9c5109c2d758861d54f3912dc`。宿主机 `sunshine/` 只保存经过归档校验的干净上游快照；实际源码维护、构建和测试全部在 win11 的 `D:\Tools`，外网下载显式使用代理。

验证：原生构建成功；39 项 CTest 零失败；聚合测试 577 项，其中 21 项按条件跳过；服务策略 12 项通过。此为源码与原生回归验证，未新增媒体验收、长期稳定性或 144fps 性能结论。

当前运行服务仍使用此前验收的 `051349744dc98e91368e3a8f6562a4f037c4b241`，本轮未替换或重启服务。新源码测试结果不能替代新二进制的运行验收。

源码维护分支：[maintenance/hyperv-win11-20261027](https://github.com/Dead-Universe/foundation-sunshine/tree/maintenance/hyperv-win11-20261027)。Moonlight 独立补丁见 [moonlight/README.md](moonlight/README.md)。

2026-10-30: Keep the interactive display awake during sealed VDD streaming, release on stop, and preserve pre-probe wake. Native regressions pass; runtime idle and release checks are recorded separately.
