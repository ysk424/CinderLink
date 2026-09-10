# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ysk424 and CinderLink contributors

[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$artifactRoot = Join-Path $repositoryRoot 'BuildArtifacts'
$testHostRoot = Join-Path $artifactRoot 'TestHost'
$testProject = Join-Path $testHostRoot 'TestHost.uproject'
$testPluginRoot = Join-Path $testHostRoot 'Plugins\CinderLink'
$reportRoot = Join-Path $artifactRoot ('TestReport-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmss') + '-' + [Guid]::NewGuid().ToString('N'))

if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
    throw "UnrealEditor-Cmd.exe was not found under: $EngineRoot"
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'Build-UE58.ps1') -EngineRoot $EngineRoot
}

New-Item -ItemType Directory -Path $testHostRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $testHostRoot 'Content') -Force | Out-Null
if (Test-Path -LiteralPath $testPluginRoot) {
    $resolvedHost = (Resolve-Path -LiteralPath $testHostRoot).Path.TrimEnd('\')
    $resolvedPlugin = (Resolve-Path -LiteralPath $testPluginRoot).Path.TrimEnd('\')
    if ($resolvedHost -ne [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'BuildArtifacts\TestHost')) -or
        $resolvedPlugin -ne (Join-Path $resolvedHost 'Plugins\CinderLink')) { throw 'Unexpected test plugin removal target.' }
    $links = @(Get-Item -LiteralPath $testHostRoot, (Split-Path -Parent $testPluginRoot), $testPluginRoot -Force;
        Get-ChildItem -LiteralPath $testPluginRoot -Recurse -Force) | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }
    if (@($links).Count) { throw 'A reparse point exists in the test plugin removal path.' }
    Remove-Item -LiteralPath $testPluginRoot -Recurse -Force
}
New-Item -ItemType Directory -Path (Split-Path -Parent $testPluginRoot) -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $artifactRoot 'CinderLink') -Destination $testPluginRoot -Recurse

$projectDefinition = [ordered]@{
    FileVersion = 3
    EngineAssociation = '5.8'
    Category = ''
    Description = 'Generated host for CinderLink automation tests.'
    Plugins = @(
        [ordered]@{
            Name = 'CinderLink'
            Enabled = $true
            SupportedTargetPlatforms = @('Win64')
        }
    )
}
$projectDefinition | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $testProject -Encoding utf8NoBOM

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
$requiredTests = @(
    'CinderLink.Integration.AppServerHandshake',
    'CinderLink.Protocol.Steering',
    'CinderLink.Security.EditProfile',
    'CinderLink.Security.EditorToolPolicy',
    'CinderLink.Security.ReadOnlyProfile',
    'CinderLink.Security.PythonAuthoringPolicy',
    'CinderLink.Security.PythonTurnRevocation',
    'CinderLink.Security.PythonPanelPreference',
    'CinderLink.Python.ExecuteAndArchive',
    'CinderLink.Python.BackupAndDiskChanges',
    'CinderLink.Python.MaterialAuthoring',
    'CinderLink.Python.ExceptionEvidence'
)
$reportedTests = @($report.tests | ForEach-Object { $_.fullTestPath })
$missing = @($requiredTests | Where-Object { $_ -notin $reportedTests })
if ($editorExitCode -ne 0 -or $failed.Count -gt 0 -or $missing.Count -gt 0) {
    $failedNames = ($failed | ForEach-Object { $_.fullTestPath }) -join ', '
    $missingNames = $missing -join ', '
    throw "CinderLink automation failed. Editor exit code: $editorExitCode. Failed tests: $failedNames. Missing tests: $missingNames"
}

Write-Host "CinderLink automation passed: $($report.tests.Count) tests." -ForegroundColor Green
