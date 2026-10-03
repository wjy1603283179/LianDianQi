param([string]$Executable = "$PSScriptRoot\..\LianDianQi.exe")
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$controller = Start-Process -FilePath $Executable -ArgumentList '--quit' -WindowStyle Hidden -Wait -PassThru
if ($controller.ExitCode) { throw "The shutdown request failed: $($controller.ExitCode)" }
