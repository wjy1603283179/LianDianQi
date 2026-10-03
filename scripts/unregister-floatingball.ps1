param([string]$FloatingBall = 'D:\Small_App\MangaDownloadTools\FloatingBall')
$ErrorActionPreference = 'Stop'
$config = Join-Path (Resolve-Path -LiteralPath $FloatingBall).Path '.settings\tools.json'
$data = Get-Content -LiteralPath $config -Raw -Encoding UTF8 | ConvertFrom-Json
Copy-Item -LiteralPath $config -Destination "$config.before-unregister-liandianqi-$(Get-Date -Format yyyyMMdd-HHmmss-fff).bak"
$data.tools = @($data.tools | Where-Object { $_.key -ne 'liandianqi' })
$temporary = "$config.liandianqi.tmp"
[IO.File]::WriteAllText($temporary, ($data | ConvertTo-Json -Depth 20), (New-Object Text.UTF8Encoding($false)))
Move-Item -LiteralPath $temporary -Destination $config -Force
Write-Host 'Removed only the liandianqi entry. The application and other tools were preserved.'
