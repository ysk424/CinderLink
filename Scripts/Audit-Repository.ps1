# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ysk424 and CinderLink contributors

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

$pluginPath = Join-Path $repositoryRoot 'CinderLink.uplugin'
$manifest = Get-Content -Raw -LiteralPath $pluginPath | ConvertFrom-Json
Assert-True ($manifest.FriendlyName -eq 'CinderLink') 'Unexpected plugin name.'
Assert-True ($manifest.Modules.Count -eq 1) 'The initial release must contain exactly one module.'
Assert-True ($manifest.Modules[0].Type -eq 'Editor') 'The module must remain editor-only.'
Assert-True ($manifest.Modules[0].PlatformAllowList -contains 'Win64') 'The initial release must remain Win64-only.'

$licensePath = Join-Path $repositoryRoot 'LICENSE'
$licenseText = Get-Content -Raw -LiteralPath $licensePath
Assert-True ($licenseText -match 'Apache License\s+Version 2\.0') 'Apache-2.0 LICENSE is missing.'

$generatedDirectoryNames = @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache')
$trackedGenerated = Get-ChildItem -LiteralPath $repositoryRoot -Directory -Recurse -Force |
    Where-Object { $generatedDirectoryNames -contains $_.Name -and $_.FullName -notlike '*\BuildArtifacts\*' }
Assert-True ($null -eq $trackedGenerated) 'Generated Unreal directories must not be committed.'

$sourceRoot = Join-Path $repositoryRoot 'Source'
$sourceFiles = Get-ChildItem -LiteralPath $sourceRoot -File -Recurse -Include '*.h', '*.cpp', '*.cs'
Assert-True ($sourceFiles.Count -gt 0) 'No source files were found.'

foreach ($file in $sourceFiles) {
    $text = Get-Content -Raw -LiteralPath $file.FullName
    Assert-True ($text -match 'SPDX-License-Identifier: Apache-2\.0') "Missing SPDX header: $($file.FullName)"
}

$combinedSource = ($sourceFiles | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"

$forbiddenSecretNames = @(
    'OPENAI_API_KEY',
    'GITHUB_TOKEN',
    'GH_TOKEN',
    'AWS_SECRET_ACCESS_KEY',
    'AZURE_CLIENT_SECRET',
    'GOOGLE_APPLICATION_CREDENTIALS'
)
foreach ($name in $forbiddenSecretNames) {
    Assert-True (-not $combinedSource.Contains($name)) "Runtime source references forbidden credential variable: $name"
}

$forbiddenNetworkPatterns = @(
    'FHttpModule',
    'IHttpRequest',
    'WinHttp',
    'WinInet',
    'ISocketSubsystem',
    'FSocket',
    'CreateNamedPipe',
    'bind\s*\(',
    'listen\s*\('
)
foreach ($pattern in $forbiddenNetworkPatterns) {
    Assert-True (-not ($combinedSource -match $pattern)) "Runtime source contains forbidden listener/network API pattern: $pattern"
}

$runtimeScripts = Get-ChildItem -LiteralPath $sourceRoot -File -Recurse -Include '*.js', '*.mjs', '*.cjs', '*.py', '*.ps1', '*.bat', '*.cmd'
Assert-True ($null -eq $runtimeScripts) 'Runtime scripts are not permitted under Source.'

Write-Host 'CinderLink repository audit passed.' -ForegroundColor Green
