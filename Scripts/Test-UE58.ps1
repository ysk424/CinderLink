# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ysk424 and CinderLink contributors

[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$artifactRoot = Join-Path $repositoryRoot 'BuildArtifacts'
$testHostRoot = Join-Path $artifactRoot 'TestHost'
$testProject = Join-Path $testHostRoot 'TestHost.uproject'
$reportRoot = Join-Path $artifactRoot 'TestReport'

if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
    throw "UnrealEditor-Cmd.exe was not found under: $EngineRoot"
}

& (Join-Path $PSScriptRoot 'Build-UE58.ps1') -EngineRoot $EngineRoot

New-Item -ItemType Directory -Path $testHostRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $testHostRoot 'Content') -Force | Out-Null

$projectDefinition = [ordered]@{
    FileVersion = 3
    EngineAssociation = '5.8'
    Category = ''
    Description = 'Generated host for CinderLink automation tests.'
    AdditionalPluginDirectories = @('..\CinderLink')
    Plugins = @(
        [ordered]@{
            Name = 'CinderLink'
            Enabled = $true
            SupportedTargetPlatforms = @('Win64')
        }
    )
}
$projectDefinition | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $testProject -Encoding utf8NoBOM

if (Test-Path -LiteralPath $reportRoot) {
    Remove-Item -LiteralPath $reportRoot -Recurse -Force
}

& $editor $testProject '-unattended' '-nop4' '-nosplash' '-NullRHI' `
    '-ExecCmds=Automation RunTests CinderLink;Quit' `
    '-TestExit=Automation Test Queue Empty' `
    "-ReportExportPath=$reportRoot"
$editorExitCode = $LASTEXITCODE

$indexPath = Join-Path $reportRoot 'index.json'
if (-not (Test-Path -LiteralPath $indexPath -PathType Leaf)) {
    throw "Unreal automation did not produce a report. Editor exit code: $editorExitCode"
}

$report = Get-Content -Raw -LiteralPath $indexPath | ConvertFrom-Json
$failed = @($report.tests | Where-Object { $_.state -ne 'Success' })
if ($editorExitCode -ne 0 -or $failed.Count -gt 0) {
    $failedNames = ($failed | ForEach-Object { $_.fullTestPath }) -join ', '
    throw "CinderLink automation failed. Editor exit code: $editorExitCode. Failed tests: $failedNames"
}

Write-Host "CinderLink automation passed: $($report.tests.Count) tests." -ForegroundColor Green
