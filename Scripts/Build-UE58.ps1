# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ysk424 and CinderLink contributors

[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runUat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$plugin = Join-Path $repositoryRoot 'CinderLink.uplugin'
$package = Join-Path $repositoryRoot 'BuildArtifacts\CinderLink'

if (-not (Test-Path -LiteralPath $runUat -PathType Leaf)) {
    throw "RunUAT.bat was not found under: $EngineRoot"
}
if (-not (Test-Path -LiteralPath $plugin -PathType Leaf)) {
    throw "Plugin manifest was not found: $plugin"
}

& (Join-Path $PSScriptRoot 'Audit-Repository.ps1')

New-Item -ItemType Directory -Path (Split-Path -Parent $package) -Force | Out-Null
& $runUat BuildPlugin "-Plugin=$plugin" "-Package=$package" '-TargetPlatforms=Win64'
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Packaged plugin: $package" -ForegroundColor Green
