<#
.SYNOPSIS
  Remove a previously installed LevelTool plugin from a UE5 engine install.

.PARAMETER EnginePath
  Full path to the engine root. Defaults to the parent of this repository.

.EXAMPLE
  .\uninstall_from_engine.ps1
  .\uninstall_from_engine.ps1 -EnginePath "E:\WorkUE5"
#>

[CmdletBinding()]
param(
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $EnginePath) {
    $EnginePath = Split-Path -Parent $RepoRoot
    Write-Host "EnginePath not specified, auto-detected: $EnginePath" -ForegroundColor DarkGray
}

$DestPlugin = Join-Path $EnginePath 'Engine\Plugins\Marketplace\LevelTool'

if (-not (Test-Path $DestPlugin)) {
    Write-Host "Nothing to do; plugin not found at: $DestPlugin" -ForegroundColor Yellow
    return
}

Write-Host "Removing: $DestPlugin" -ForegroundColor Cyan
Remove-Item -Recurse -Force $DestPlugin
Write-Host "Done." -ForegroundColor Green
Write-Host ""
Write-Host "NOTE: Intermediate\ and Binaries\ that the plugin produced inside"
Write-Host "      target projects are not touched by this script. You can safely"
Write-Host "      delete <project>\Intermediate and <project>\Binaries next time"
Write-Host "      you open the project if you want a perfectly clean state."
