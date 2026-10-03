param([string]$Executable = "$PSScriptRoot\..\LianDianQi.exe")
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
# This launches the user's interactive Qt window. The helper itself exits immediately.
Start-Process -FilePath $Executable -ArgumentList '--show' -WorkingDirectory (Split-Path $Executable) -WindowStyle Normal
