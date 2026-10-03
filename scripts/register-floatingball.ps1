param(
    [string]$FloatingBall = 'D:\Small_App\MangaDownloadTools\FloatingBall',
    [string]$Executable = "$PSScriptRoot\..\LianDianQi.exe"
)
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$FloatingBall = (Resolve-Path -LiteralPath $FloatingBall).Path
$config = Join-Path $FloatingBall '.settings\tools.json'
if (!(Test-Path -LiteralPath $config)) { throw "FloatingBall configuration not found: $config" }
$data = Get-Content -LiteralPath $config -Raw -Encoding UTF8 | ConvertFrom-Json
if (!$data.tools) { throw 'FloatingBall tools.json is missing its tools list.' }
$spec = [ordered]@{
    key = 'liandianqi'
    name = '点序 · 连点器'
    hint = 'C++ / Qt · 可视化脚本 · F6 启停任务 / F8 停止'
    category = '自动化'
    icon = '🖱'
    status = @{ type = 'process'; match = ('(?i)^\s*"?' + [regex]::Escape($Executable) + '"?(?:\s|$)'); openUrl = ([System.Uri]$Executable).AbsoluteUri }
    start = @{ cmd = @($Executable, '--show'); cwd = (Split-Path $Executable); wait = 10 }
    stop = @{ cmd = @($Executable, '--quit'); cwd = (Split-Path $Executable); wait = 10 }
}
$data.tools = @($data.tools | Where-Object { $_.key -ne 'liandianqi' }) + @($spec)
$backup = "$config.before-liandianqi-$(Get-Date -Format yyyyMMdd-HHmmss-fff).bak"
Copy-Item -LiteralPath $config -Destination $backup
$temporary = "$config.liandianqi.tmp"
[IO.File]::WriteAllText($temporary, ($data | ConvertTo-Json -Depth 20), (New-Object Text.UTF8Encoding($false)))
Move-Item -LiteralPath $temporary -Destination $config -Force
Write-Host "Registered 点序 · 连点器. FloatingBall will reload tools.json automatically."
Write-Host "Backup: $backup"
