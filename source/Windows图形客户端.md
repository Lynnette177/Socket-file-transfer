# Windows 原生图形客户端与连接保活

## Visual Studio 构建

安装 Visual Studio 的“使用 C++ 的桌面开发”工作负载、CMake 和 OpenSSL 3。可使用 vcpkg 安装 `openssl:x64-windows-static`，然后在 PowerShell 中运行（将 vcpkg 路径替换成实际路径）：

```powershell
C:/vcpkg/vcpkg.exe install openssl:x64-windows-static
cmake -S source -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DSFT_STATIC_OPENSSL=ON
cmake --build build --config Release --parallel
```

也可在 Visual Studio 中打开 `source` 文件夹并配置相同的 CMake/vcpkg 参数。`sft_gui` 和 `sft` 为两个构建目标，Windows 构建默认将两者输出至 `binaries/windows-x86_64`，Debug 和 Release 会覆盖该目录中的同名程序。可用 CMake 选项 `SFT_WINDOWS_OUTPUT_DIR` 指定其他输出目录。客户端采用 Win32 C++，不依赖 Web 或 GUI 第三方框架。

## 使用

先使用 `sft keygen` 生成双方密钥，通过可信渠道交换公钥，并正常启动服务端。运行 `sft_gui.exe`，填写服务器地址、端口、客户端私钥、服务端公钥和本地文件目录，然后点击“连接”。界面运行同目录的 `sft.exe client`，沿用既有的 TLS 身份认证及协议。日志中出现 `Fresh handshake complete` 后表示握手完成；密钥或连接错误会显示在日志中。

“刷新列表”将远程列表写入日志；上传通过文件选择器选取本地文件。下载、进入目录、新建目录和删除文件按钮使用“远程路径 / 命令”输入框的内容。也可输入 `help`、`pwd`、`cd ..`、`rm -r 目录`、`upload -o ...` 等命令后点击“执行命令”。同名文件遵循原有 CLI 行为。进度和最终确认显示在日志中。操作顺序执行；关闭窗口会中止运行中的客户端和传输。

## 保活和超时

TCP socket 显式启用 `SO_KEEPALIVE`，探测间隔由操作系统设置决定。认证完成后，服务端在等待下一条命令期间不再使用帧读取超时，因此健康连接可以空闲超过 `--timeout`；一旦接收到数据，握手、帧读取和写入仍受该超时限制，以避免不完整消息长期占用资源。TCP 保活用于检测失效连接，不会自动恢复断线；网络设备仍可能有自己的空闲连接策略。重连依旧需要新握手，失败操作不会自动重放。
