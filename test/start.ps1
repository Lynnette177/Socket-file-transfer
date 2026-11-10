[CmdletBinding()]
param(
    [string]$BinDir = '',
    [ValidateRange(1, 65535)][int]$Port = 5005,
    [ValidateSet('gui', 'server', 'client')][string]$Mode = 'gui'
)
$ErrorActionPreference = 'Stop'
$testDir = $PSScriptRoot
$repoDir = Split-Path -Parent $testDir
$runtime = Join-Path $testDir 'runtime'
$keys = Join-Path $runtime 'credentials'
$shared = Join-Path $runtime 'server-files'
$local = Join-Path $runtime 'client-files'
$logs = Join-Path $runtime 'logs'
$serverProcess = $null
$guiProcess = $null

try {
    $candidates = if ($BinDir) { @($BinDir) } else {
        @((Join-Path $repoDir 'binaries\windows-x86_64'), (Join-Path $repoDir 'build\Release'), (Join-Path $repoDir 'build\Debug'),
          (Join-Path $repoDir 'build'), (Join-Path $repoDir 'source\build\Release'),
          (Join-Path $repoDir 'source\build\Debug'), (Join-Path $repoDir 'source\build'))
    }
    $selected = $candidates | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_ 'sft.exe') -PathType Leaf) -and
        ($Mode -ne 'gui' -or (Test-Path -LiteralPath (Join-Path $_ 'sft_gui.exe') -PathType Leaf))
    } | Select-Object -First 1
    if (-not $selected) {
        throw '找不到所需的可执行文件。请先构建项目，或使用 -BinDir 指定程序所在目录；图形模式需要 sft.exe 和 sft_gui.exe。'
    }
    $selected = (Resolve-Path -LiteralPath $selected).Path
    $sft = Join-Path $selected 'sft.exe'
    $gui = Join-Path $selected 'sft_gui.exe'
    foreach ($dir in @($keys, $shared, $local, $logs)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    foreach ($identity in @('server', 'client')) {
        $private = Join-Path $keys "$identity.key"
        $public = Join-Path $keys "$identity.pub"
        $hasPrivate = Test-Path -LiteralPath $private -PathType Leaf
        $hasPublic = Test-Path -LiteralPath $public -PathType Leaf
        if ($hasPrivate -xor $hasPublic) {
            throw "$identity 凭据不完整：私钥和公钥必须同时存在。为避免覆盖现有文件，脚本已停止。"
        }
        if (-not $hasPrivate) {
            Write-Host "Generating $identity identity..."
            & $sft keygen --key $private --public $public
            if ($LASTEXITCODE -ne 0) { throw "生成 $identity 凭据失败（退出码 $LASTEXITCODE）。" }
        } else {
            Write-Host "Using existing $identity identity."
        }
    }
    if ($Mode -eq 'server') {
        Write-Host "Starting command-line server on 127.0.0.1:$Port. Shared directory: $shared"
        & $sft server --host '127.0.0.1' --port "$Port" --key (Join-Path $keys 'server.key') `
            --peer (Join-Path $keys 'client.pub') --root $shared
        exit $LASTEXITCODE
    }
    if ($Mode -eq 'client') {
        Write-Host "Connecting command-line client to 127.0.0.1:$Port. Local directory: $local"
        & $sft client --host '127.0.0.1' --port "$Port" --key (Join-Path $keys 'client.key') `
            --peer (Join-Path $keys 'server.pub') --local $local
        exit $LASTEXITCODE
    }
    # Windows paths cannot contain quotes; enclosing each argument keeps spaces intact.
    function Q([string]$s) { return '"' + $s.TrimEnd('\') + '"' }
    $serverOut = Join-Path $logs 'server.out.log'
    $serverErr = Join-Path $logs 'server.err.log'
    $serverArgs = @('server', '--host', '127.0.0.1', '--port', "$Port", '--key', (Q (Join-Path $keys 'server.key')),
        '--peer', (Q (Join-Path $keys 'client.pub')), '--root', (Q $shared))
    $serverProcess = Start-Process -FilePath $sft -ArgumentList $serverArgs -WorkingDirectory $selected -WindowStyle Hidden `
        -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $ready = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $serverProcess.Refresh()
        if ($serverProcess.HasExited) { throw "服务端启动失败。请查看 $serverErr" }
        if ((Test-Path -LiteralPath $serverOut) -and ((Get-Content -LiteralPath $serverOut -Raw -ErrorAction SilentlyContinue) -match 'Server Started:')) { $ready = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) { throw "服务端启动超时。请查看 $serverOut 和 $serverErr" }
    $guiArgs = @('--host', '127.0.0.1', '--port', "$Port", '--key', (Q (Join-Path $keys 'client.key')),
        '--peer', (Q (Join-Path $keys 'server.pub')), '--local', (Q $local), '--connect')
    $guiProcess = Start-Process -FilePath $gui -ArgumentList $guiArgs -WorkingDirectory $selected -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $connected = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $serverProcess.Refresh()
        $guiProcess.Refresh()
        if ($serverProcess.HasExited) { throw "服务端已退出。请查看 $serverErr" }
        if ($guiProcess.HasExited) { throw '图形客户端已退出，未能建立连接。' }
        if ((Get-Content -LiteralPath $serverOut -Raw -ErrorAction SilentlyContinue) -match 'New Connection Established\. Client:') { $connected = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $connected) { throw "连接未在预期时间内完成。请查看图形客户端和 $serverErr" }
    Write-Host "Connected. Server files: $shared"
    Write-Host "Client files: $local"
    Write-Host 'Close the graphical client to stop the test server.'
    $guiProcess.WaitForExit()
} catch {
    Write-Error $_ -ErrorAction Continue
    if ($guiProcess) {
        $guiProcess.Refresh()
        if (-not $guiProcess.HasExited) { Stop-Process -Id $guiProcess.Id -ErrorAction SilentlyContinue }
    }
    exit 1
} finally {
    if ($serverProcess) {
        $serverProcess.Refresh()
        if (-not $serverProcess.HasExited) { Stop-Process -Id $serverProcess.Id -ErrorAction SilentlyContinue }
    }
}
