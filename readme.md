# SecureFileTransfer：跨平台文件安全传输系统

使用 C++17 和 OpenSSL 实现的文件安全传输系统，支持 macOS、Linux、Windows。每个平台使用一份程序，通过 `server` / `client` 子命令切换角色，提供公钥认证、加密上传下载、目录操作、换钥与断线重连。

## 项目背景与版本历史

本仓库始于 2024 年的本科计算机网络课程实验，当时实现了 Windows 上的 Socket TCP 文件传输、命令交互和多客户端处理。本次软件与系统安全课程在这一基础上扩展了跨平台支持、双向身份认证、加密会话、可靠消息分帧和文件访问边界，并整理了源码、可执行文件和实验报告。

- [本科版本 1.0.0](https://github.com/Lynnette177/Socket-file-transfer/tree/1.0.0)：查看当时的源码和项目说明。
- [完整提交历史](https://github.com/Lynnette177/Socket-file-transfer/commits/main/)：保留最初提交至今的项目记录。
- [与本科版本的差异](https://github.com/Lynnette177/Socket-file-transfer/compare/1.0.0...main)：查看目录调整、代码实现及新增文档。

当前实现保留 `Server`、`ClientHandler`、`Client`、`ClientProcessor` 的类组织，以及 `ls/get/upload/rm/cd/mkdir` 命令交互。

## 项目目录

```text
Socket-file-transfer/
├── source/
│   ├── src/                 C++ 源码与头文件
│   ├── tests/               集成测试及大文件测试
│   ├── scripts/             Linux / Windows 交叉构建脚本
│   └── CMakeLists.txt
├── binaries/
│   ├── macos-arm64/sft
│   ├── linux-x86_64/sft
│   ├── windows-x86_64/sft.exe
│   ├── SHA256SUMS
│   └── build-manifest.json
├── evidence/                已有实验测试记录
├── .github/workflows/       三平台构建与测试配置
├── 实验报告.md
├── 实验报告.pdf
├── 协议详细文档.md
├── 协议详细文档.pdf
└── SHA256SUMS
```

## 文档入口

- [实验报告（Markdown）](实验报告.md) · [实验报告（PDF）](实验报告.pdf)
- [SFT/1 协议说明（Markdown）](协议详细文档.md) · [协议说明（PDF）](协议详细文档.pdf)
- [可执行文件及平台说明](binaries/README.md)
- [测试记录](evidence/)

## 认证与传输

双方本地预存对方的 Ed25519 公钥，用于验证身份。TLS 1.3 使用临时 X25519 协商秘密，通过 HKDF-SHA384 派生双向 AES-256-GCM 流量密钥；命令、元数据和文件内容均在加密通道中传输。

同一连接可以顺序传输多个文件。每发送约 256 MiB 自动触发 TLS KeyUpdate，也支持 `rekey` 主动换钥。`reconnect` 会销毁原连接状态并完成新的身份握手，禁用会话恢复和 0-RTT；失败的写操作不会自动重放。

应用层使用带长度的 SFT/1 帧，文件按 64 KiB 分块读写，接收端校验长度和 SHA-256 后提交临时文件。服务端限制共享根目录、链接访问、帧大小、单文件大小与连接数量，并支持只读模式。当前不支持断点续传，未完成文件需从头传输。

## 源码构建

依赖 CMake 3.20+、C++17 编译器、OpenSSL 3.x；运行测试还需要 Python 3 和 OpenSSL CLI。

macOS：

```sh
brew install cmake openssl@3
cmake -S source -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" -DSFT_STATIC_OPENSSL=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Linux（Debian / Ubuntu）：

```sh
sudo apt-get install build-essential cmake libssl-dev openssl python3
cmake -S source -B build -DCMAKE_BUILD_TYPE=Release -DSFT_STATIC_OPENSSL=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows（Visual Studio 2022 + vcpkg，在 PowerShell 中执行，将 vcpkg 路径改为实际安装目录）：

```powershell
C:/vcpkg/vcpkg.exe install openssl:x64-windows-static
cmake -S source -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DSFT_STATIC_OPENSSL=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Windows 测试需要 OpenSSL CLI；若未自动找到，可在配置时指定 `-DSFT_OPENSSL_EXECUTABLE="C:/实际路径/openssl.exe"`。仅构建程序可加 `-DBUILD_TESTING=OFF`。

## 本地运行

以下以 macOS 预编译程序为例，在仓库根目录执行：

```sh
SFT="$PWD/binaries/macos-arm64/sft"

"$SFT" keygen --key server.key --public server.pub
"$SFT" keygen --key client.key --public client.pub

# 终端一：运行服务端
"$SFT" server --key server.key --peer client.pub --root shared \
  --host 127.0.0.1 --port 5005 --timeout 600

# 终端二：重新进入仓库根目录并设置 SFT 后运行客户端
"$SFT" client --key client.key --peer server.pub --local local_files \
  --host 127.0.0.1 --port 5005 --timeout 600
```

在客户端输入 `help` 查看命令。多客户端可以分别生成身份，将各客户端 `.pub` 放入服务端信任目录，再以 `--trust-dir trusted_clients` 替代服务端的 `--peer`。私钥留在所属端，共享目录之外存放身份文件；公钥通过可信渠道交换和核对。

## 验证状态

| 平台 | 当前交付情况 |
|---|---|
| macOS arm64 | 原生 Release、集成测试和 ASan/UBSan 检查通过；270 MiB 同会话往返摘要一致 |
| Linux x86_64 | 已生成完全静态链接可执行文件；原生运行与跨系统互通待验证 |
| Windows x86_64 | 已生成可执行文件，依赖系统 DLL；原生运行与跨系统互通待验证 |

`evidence/` 保存实验阶段测试记录，其中路径和日期对应当时的本地测试环境。GitHub Actions 会在仓库推送后运行三平台原生构建与集成测试；已有二进制的交叉构建记录不等同于这些运行测试已经通过。

```sh
python3 source/tests/integration.py binaries/macos-arm64/sft
python3 source/tests/large_transfer.py binaries/macos-arm64/sft
```

`SHA256SUMS` 用于核对仓库内容，`binaries/SHA256SUMS` 校验预编译程序及配套文件；`build-manifest.json` 保留交付程序与源码摘要。各平台目录包含静态依赖的许可文本。
