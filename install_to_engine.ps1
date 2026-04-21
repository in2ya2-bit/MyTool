<#
.SYNOPSIS
  Install the LevelTool plugin into a UE5 engine install, so it becomes
  available to every .uproject using that engine without touching any
  project file.

.DESCRIPTION
  Copies Plugins\LevelTool and the Python pipeline under files\ into
  <EngineRoot>\Engine\Plugins\Marketplace\LevelTool\. Because the uplugin
  is shipped with "EnabledByDefault": true, the plugin auto-enables in
  every project the first time its editor is opened — the target project's
  .uproject stays untouched and clean for Perforce.

.PARAMETER EnginePath
  Full path to the engine root (the folder containing Engine\, FeaturePacks\, ...).
  Defaults to the parent of this repository (repo-root\..) which matches
  the common case E:\WorkUE5\MyTool -> E:\WorkUE5.

.PARAMETER Force
  Overwrite the existing installed copy if one is present.

.EXAMPLE
  # Auto-detect engine (repo parent)
  .\install_to_engine.ps1

.EXAMPLE
  # Explicit engine path
  .\install_to_engine.ps1 -EnginePath "E:\WorkUE5" -Force
#>

[CmdletBinding()]
param(
    [string]$EnginePath,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $EnginePath) {
    $EnginePath = Split-Path -Parent $RepoRoot
    Write-Host "EnginePath not specified, auto-detected: $EnginePath" -ForegroundColor DarkGray
}

$EnginePluginsDir = Join-Path $EnginePath 'Engine\Plugins\Marketplace'
if (-not (Test-Path (Join-Path $EnginePath 'Engine'))) {
    throw "Not a UE5 engine root (no Engine\ folder): $EnginePath"
}

$SourcePlugin = Join-Path $RepoRoot 'Plugins\LevelTool'
$SourceFiles  = Join-Path $RepoRoot 'files'
$DestPlugin   = Join-Path $EnginePluginsDir 'LevelTool'
$DestFiles    = Join-Path $DestPlugin 'files'

if (-not (Test-Path $SourcePlugin)) { throw "Source plugin not found: $SourcePlugin" }
if (-not (Test-Path $SourceFiles )) { throw "Source files/ not found: $SourceFiles"  }

if (Test-Path $DestPlugin) {
    if (-not $Force) {
        throw "Destination already exists: $DestPlugin  (use -Force to overwrite)"
    }
    Write-Host "Removing existing install: $DestPlugin" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $DestPlugin
}

New-Item -ItemType Directory -Force -Path $EnginePluginsDir | Out-Null

Write-Host ""
Write-Host "Installing LevelTool..." -ForegroundColor Cyan
Write-Host "  Engine :  $EnginePath"
Write-Host "  Source :  $SourcePlugin"
Write-Host "  Target :  $DestPlugin"
Write-Host ""

# Copy plugin folder (Source, uplugin, optional Config, etc.)
Copy-Item -Recurse -Force -Path $SourcePlugin -Destination $EnginePluginsDir

# Place the Python pipeline INSIDE the plugin so ResolvePythonScriptDir()
# finds it automatically (no user setting needed).
Write-Host "  Copying Python pipeline into plugin..." -ForegroundColor DarkGray
Copy-Item -Recurse -Force -Path $SourceFiles -Destination $DestFiles

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  Installed to : $DestPlugin"
Write-Host "  Python dir   : $DestFiles"
Write-Host ""
Write-Host "Next step:" -ForegroundColor Cyan
Write-Host "  1. Close any running UE5 editor instance"
Write-Host "  2. Regenerate project files for any .uproject you want to use this in"
Write-Host "     (right-click the .uproject -> Generate Visual Studio project files)"
Write-Host "  3. Open the editor and build when prompted - the plugin compiles once"
Write-Host "     and is then usable from Tools -> Level Tool in every project"
Write-Host "     that opens with this engine install."
