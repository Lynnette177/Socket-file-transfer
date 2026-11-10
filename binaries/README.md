# 可执行文件

| 系统/架构 | 文件 | 验证状态 |
|---|---|---|
| macOS / Apple Silicon | [macos-arm64/sft](macos-arm64/sft) | 原生构建与集成测试通过 |
| Linux / x86_64 | [linux-x86_64/sft](linux-x86_64/sft) | 交叉构建完成，ELF完全静态链接；目标平台运行待验证 |
| Windows 10/11 / x86_64 | [windows-x86_64/sft.exe](windows-x86_64/sft.exe) | 交叉构建完成，依赖系统DLL；目标平台运行待验证 |

每个程序同时包含服务端和客户端功能，通过启动参数选择角色，不用分别下载两份程序。各平台程序不能互相替代，其他CPU架构需要用源码另行构建。

## 快速使用

在对应平台目录执行下列命令。macOS/Linux把`sft`写成`./sft`；Windows PowerShell写成`.\sft.exe`。

```text
sft keygen --key server.key --public server.pub
sft keygen --key client.key --public client.pub
sft fingerprint --public server.pub
sft fingerprint --public client.pub
sft server --key server.key --peer client.pub --root shared --host 127.0.0.1 --port 5005
sft client --key client.key --peer server.pub --host 127.0.0.1 --port 5005 --local local_files
```

这是同一主机进行演示的命令。分布式部署时，双方在自己的主机生成私钥，只交换`.pub`公钥文件，并通过可信渠道核对指纹；不要传送`.key`私钥。服务端跨主机接入需显式调整`--host`监听地址。

## 说明

- 认证使用固定Ed25519公钥，临时X25519协商密钥，TLS 1.3使用AES-256-GCM保护命令和文件。
- 同一会话可连续传输多个文件；重新连接必须完整握手，失败操作不会自动重放。
- `get -o`在本地已有同名文件时追加`.L`保存。中断文件从头重传，暂不支持断点续传。
- `SHA256SUMS`校验本目录文件，`build-manifest.json`记录程序与源码摘要及验证状态。
- 各平台目录保留OpenSSL及静态运行库的相关许可文本。
