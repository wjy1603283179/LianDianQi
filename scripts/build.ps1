param(
    [string]$QtRoot = "$PSScriptRoot\..\.tools\Qt\6.5.3\msvc2019_64",
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path "$PSScriptRoot\..").Path
$QtRoot = (Resolve-Path $QtRoot).Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
# Import the toolchain environment; this command performs no filesystem mutation.
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$envBatch = Join-Path $BuildDir 'toolchain-env.cmd'
@("@echo off", "call `"$devcmd`" -no_logo -arch=x64 -host_arch=x64 >nul", 'if errorlevel 1 exit /b 1', 'set') | Set-Content -LiteralPath $envBatch -Encoding Default
$envLines = & cmd.exe /d /c $envBatch
if ($LASTEXITCODE) { throw 'Visual C++ environment initialization failed.' }
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$env:PATH = "$QtRoot\bin;$env:PATH"
$env:VSLANG = '1033'
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (!$cmake) { $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' } else { $cmake = $cmake.Source }
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$env:PATH = "$(Split-Path $ninja);$env:PATH"
& $cmake -S $projectRoot -B $BuildDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$QtRoot" '-DBUILD_TESTING=ON'
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
$buildLog = Join-Path $BuildDir 'build.log'
& $cmake --build $BuildDir --parallel 4 2>&1 | Out-File -LiteralPath $buildLog -Encoding utf8
$buildExit = $LASTEXITCODE
Get-Content -LiteralPath $buildLog | Select-String '^\[\d|warning C|error C|fatal error|FAILED|ninja:' | ForEach-Object { Write-Host $_.Line }
if ($buildExit) { throw "Build failed. See $buildLog" }
if (!$SkipTests) {
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    & $ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed.' }
}
Write-Host "Built: $BuildDir\LianDianQi.exe"
