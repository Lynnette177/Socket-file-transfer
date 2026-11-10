# Windows 一键本地测试

Release 压缩包已包含可执行文件；从源码运行时，先按 [Windows 图形客户端说明](../source/Windows图形客户端.md) 编译。Windows 构建默认将 `sft.exe` 与 `sft_gui.exe` 放在 `binaries/windows-x86_64`。双击 `start.cmd`，脚本优先使用该目录中的程序，也兼容旧构建目录。也可从 PowerShell 指定目录和端口：

```powershell
.\test\start.ps1 -BinDir 'C:\path\to\build\Release' -Port 5005
```

脚本首次运行时在 `test/runtime/credentials` 中创建两对凭据，之后复用；如果只存在某一对的私钥或公钥，脚本会停止并保留现有文件。服务端使用 `127.0.0.1`，图形客户端自动填写连接配置并连接；脚本会检查服务端是否完成客户端认证。关闭图形客户端后，脚本停止自己启动的服务端。若端口已被占用，启动将失败，可使用 `-Port` 指定其他端口。

## 命令行测试

先双击 `server.cmd` 启动命令行服务端，再双击 `client.cmd` 打开命令行客户端。客户端中输入 `help` 查看命令，输入 `exit` 退出；服务端可按 `Ctrl+C` 停止。这两个脚本使用与图形测试相同的凭据和文件目录，凭据不存在时会生成。

也可以在两个 PowerShell 终端分别执行，并使用相同的自定义端口：

```powershell
.\test\server.cmd -Port 5006
.\test\client.cmd -Port 5006
```

两个入口也支持 `-BinDir 'C:\path\to\programs'`。命令行客户端可单独运行并连接已启动的测试服务端；它不会自动启动或关闭服务端。图形测试和命令行测试若同时运行，应使用不同端口。

传输文件分别放在 `runtime/server-files` 和 `runtime/client-files`，服务端日志放在 `runtime/logs`。整个 `runtime/` 已被 Git 忽略，请勿将其中凭据提交到版本库。测试脚本不会自动编译程序；构建后如需再次测试，请确保使用的是最新版本的两个可执行文件。
