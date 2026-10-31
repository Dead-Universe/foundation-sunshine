# Moonlight 独立维护补丁

基于 [Moonlight-VPlus v6.5.3](https://github.com/qiin2333/moonlight-qt/releases/tag/v6.5.3)，上游 `5c3061568275170fcfa6e29856eac8f9739dda5e`，维护提交 `ae4e2e40a6aec2912245a75a4eb7b1399d64a6e1`，结果树 `68cd23ce9d23e2c6b929deaecb181815237cc2fb`。顺序和 SHA256 见 [series.json](series.json)。

| 补丁 | 用处 |
| --- | --- |
| 0001 | 保护已有转储确认的 HDR 清理、窗口操作与画面呈现竞态，保留上游 AMF 修复。 |
| 0002 | 保留稳定的 QML 导航上下文，避免退出或错误处理时访问已销毁的创建上下文。 |
| 0003 | 在剪贴板辅助程序中实现 Windows 文件和文件夹复制，延迟读取内容，并处理与文字、图片切换的衔接。 |

本轮客户端改动集中在文件剪贴板助手，主程序和解码器二进制保持既有 hdrfix.1；没有增加自动重连规避。两端都需要文件剪贴板补丁，仅更新一端无法启用新功能。默认独立目录为 `D:\Software\Moonlight-VPlus-Portable-x64-6.5.3-hdrfix.1-filecopy.1`，官方客户端保留。

连接 Desktop 后，在一端资源管理器复制，在另一端粘贴。包括中文子目录和空文件夹，只支持复制。连接前的旧剪贴板不自动导出，重连后重新复制。接收软件须支持 Windows 虚拟文件流。目标端尚有本地文件复制时，仍沿用上游保护行为，远端文字或图片可能被忽略。

构建仅在 win11 的 D:\Tools 中完成。双向原生 Shell 哈希检查和用户资源管理器实测通过，超过 4 GiB 的定位、旧流撤销及重连后复制通过。全补丁从干净上游独立重放得到相同结果树。图片的已知 Qt 位图兼容限制继续记录，不将它包装成已解决。

源码分支：[maintenance/moonlight-hdr-20261031](https://github.com/Dead-Universe/moonlight-qt/tree/maintenance/moonlight-hdr-20261031)。配对信息、私钥、用户设置、原始日志和转储不随补丁发布。
