param(
    [string]$QtRoot = "$PSScriptRoot\..\.tools\Qt\6.5.3\msvc2019_64",
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [string]$Version = '1.0.1'
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path "$PSScriptRoot\..").Path
$QtRoot = (Resolve-Path $QtRoot).Path
$out = Join-Path $projectRoot "dist\DianXu-$Version-windows-x64"
if (Test-Path $out) { throw "Output already exists. Use a fresh version or preserve and rename $out before packaging." }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item -LiteralPath "$BuildDir\LianDianQi.exe" -Destination $out
$env:PATH = "$QtRoot\bin;$env:PATH"
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime --skip-plugin-types 'generic,imageformats,iconengines,networkinformation,tls,styles' --dir $out "$out\LianDianQi.exe"
if ($LASTEXITCODE) { throw 'Qt deployment failed.' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$runtime = Get-ChildItem -LiteralPath "$vs\VC\Redist\MSVC" -Directory | Where-Object { $_.Name -match '^14\.' } | Sort-Object Name -Descending | Select-Object -First 1
$crt = Get-ChildItem -LiteralPath "$($runtime.FullName)\x64" -Directory -Filter '*.CRT' | Select-Object -First 1
if (!$crt) { throw 'Visual C++ redistribution files not found.' }
Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $out
Copy-Item -LiteralPath "$projectRoot\LICENSE","$projectRoot\README.md" -Destination $out
Copy-Item -LiteralPath "$projectRoot\examples" -Destination $out -Recurse
New-Item -ItemType Directory -Path "$out\scripts" | Out-Null
Copy-Item -LiteralPath "$projectRoot\scripts\register-floatingball.ps1","$projectRoot\scripts\floatingball-start.ps1","$projectRoot\scripts\floatingball-stop.ps1","$projectRoot\scripts\unregister-floatingball.ps1" -Destination "$out\scripts"
Copy-Item -LiteralPath "$projectRoot\docs\qt-licenses" -Destination "$out\licenses" -Recurse
Copy-Item -LiteralPath "$projectRoot\docs\USAGE.md" -Destination $out
Copy-Item -LiteralPath "$projectRoot\docs" -Destination "$out\docs" -Recurse
@('[Paths]', 'Plugins=.') | Set-Content -LiteralPath "$out\qt.conf" -Encoding ascii
$zip = "$out.zip"
Compress-Archive -Path $out -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content -LiteralPath "$zip.sha256" -Encoding ascii
Write-Host "Package: $zip"
Write-Host "SHA256: $hash"
