param(
    [string]$QtRoot = "$PSScriptRoot\..\.tools\Qt\6.5.3\msvc2019_64",
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [switch]$SkipTests,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
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
# Probe the prefix directly: old CMake can misdecode localized MSVC output,
# leaving Ninja with no header dependencies and stale object layouts.
$probeDir = (Resolve-Path $BuildDir).Path
$probeHeader = Join-Path $probeDir 'include-prefix-probe.h'
$probeSource = Join-Path $probeDir 'include-prefix-probe.cpp'
$probeObject = Join-Path $probeDir 'include-prefix-probe.obj'
'// Dependency prefix probe.' | Set-Content -LiteralPath $probeHeader -Encoding ascii
'#include "include-prefix-probe.h"' | Set-Content -LiteralPath $probeSource -Encoding ascii
$probeOutput = & cl.exe /nologo /showIncludes /c "/Fo$probeObject" $probeSource 2>&1
if ($LASTEXITCODE) { throw 'Compiler dependency prefix probe failed.' }
$prefixLine = $probeOutput | ForEach-Object { $_.ToString() } | Where-Object { $_.EndsWith($probeHeader) } | Select-Object -First 1
if (!$prefixLine) { throw 'Could not determine the compiler dependency prefix.' }
$includePrefix = $prefixLine.Substring(0, $prefixLine.Length - $probeHeader.Length).TrimEnd() + ' '
$previousRules = Join-Path $probeDir 'CMakeFiles\rules.ninja'
if (Test-Path -LiteralPath $previousRules) {
    $previousPrefix = Get-Content -LiteralPath $previousRules -Encoding UTF8 | Where-Object { $_ -match '^msvc_deps_prefix = ' } | Select-Object -First 1
    if ($previousPrefix -and $previousPrefix.TrimEnd() -ne ('msvc_deps_prefix = ' + $includePrefix).TrimEnd()) { $Clean = $true }
}
& $cmake -S $projectRoot -B $BuildDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$QtRoot" '-DBUILD_TESTING=ON' "-DDIANXU_SHOWINCLUDES_PREFIX=$includePrefix"
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
$buildLog = Join-Path $BuildDir 'build.log'
$buildArguments = @('--build', $BuildDir, '--parallel', '4')
if ($Clean) { $buildArguments += '--clean-first' }
& $cmake @buildArguments 2>&1 | Out-File -LiteralPath $buildLog -Encoding utf8
$buildExit = $LASTEXITCODE
Get-Content -LiteralPath $buildLog | Select-String '^\[\d|warning C|error C|fatal error|FAILED|ninja:' | ForEach-Object { Write-Host $_.Line }
if ($buildExit) { throw "Build failed. See $buildLog" }
if (!$SkipTests) {
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    & $ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed.' }
}
Write-Host "Built: $BuildDir\LianDianQi.exe"
