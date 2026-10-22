#requires -Version 7.2
# Makes the launcher's game files for one executable of an Uncharted: The Nathan Drake Collection dump with the
# Python reference scripts, as run.py does. The app makes the same files itself (platform/prepare_game.cpp); this
# script is for developers, to compare the two (pc-smoke.ps1 -Mode prepare writes the app's copy).
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Dump,[string]$Exe='eboot.bin',[string]$Out='',[string]$Python='python')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$repo=Split-Path $root -Parent
if(!(Test-Path -LiteralPath (Join-Path $Dump 'eboot.bin'))) { throw "$Dump does not contain eboot.bin; pass the game's app0 folder" }
if(!$Out) { $Out=Join-Path $root ('.local/prepared/'+($Exe -split '[.-]')[0]) }
New-Item -ItemType Directory -Force $Out | Out-Null
$marker=Join-Path $Out 'format.txt'
if(Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker }
foreach($step in @(@('prepare.py','--exe',$Exe),@('link_modules.py','--exe',$Exe),@('content_profile.py'))) {
    & $Python (Join-Path $repo "scripts/$($step[0])") $Dump --out $Out @($step | Select-Object -Skip 1)
    if($LASTEXITCODE) { throw "Game preparation failed: $($step[0])" }
}
$format=(Get-Content -LiteralPath (Join-Path $root 'prepared-format.txt') -TotalCount 1).Trim()
[IO.File]::WriteAllText($marker,"uncharted-uwp-prepared $format`n",[Text.Encoding]::ASCII)
Write-Output "Prepared game files (format $format) written to $Out"
