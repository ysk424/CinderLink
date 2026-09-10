# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ysk424 and CinderLink contributors

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9_-]{1,80}$')][string]$RunId,
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
$restoreProject = (Resolve-Path -LiteralPath $ProjectRoot).Path.TrimEnd('\', '/')
if (@(Get-ChildItem -LiteralPath $restoreProject -Filter '*.uproject' -File).Count -ne 1) {
    throw 'ProjectRoot must contain exactly one .uproject file.'
}

function Get-CheckedLocalPath([string]$Relative) {
    if ([string]::IsNullOrEmpty($Relative) -or $Relative -match '[\\:<>"|?*\x00-\x1f]' -or $Relative.StartsWith('/') -or
        @($Relative.Split('/') | Where-Object { $_ -in @('', '.', '..') -or $_.EndsWith('.') -or $_.EndsWith(' ') }).Count) {
        throw "Invalid relative path in archive: $Relative"
    }
    $candidate = [IO.Path]::GetFullPath((Join-Path $restoreProject $Relative))
    if (-not $candidate.StartsWith($restoreProject + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped project: $Relative"
    }
    $ancestor = $candidate
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Link in path: $Relative" }
        }
        $next = [IO.Path]::GetDirectoryName($ancestor)
        if ($next -eq $ancestor) { break }
        $ancestor = $next
    }
    return $candidate
}

$restoreRunRelative = "Saved/CinderLink/Python/$RunId"
$restoreManifest = Get-CheckedLocalPath "$restoreRunRelative/result.json"
$restoreRecord = Get-Content -LiteralPath $restoreManifest -Raw | ConvertFrom-Json
if ($restoreRecord.run_id -ne $RunId -or $null -eq $restoreRecord.backups) { throw 'Invalid run manifest.' }

$restorePlan = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $restoreRecord.backups) {
    if ($entry.path -notlike 'Content/*' -or $entry.md5 -notmatch '^[a-fA-F0-9]{32}$') { throw 'Invalid backup entry.' }
    $target = Get-CheckedLocalPath $entry.path
    $backup = Get-CheckedLocalPath "$restoreRunRelative/backup/$($entry.path)"
    if (-not (Test-Path -LiteralPath $backup -PathType Leaf) -or
        (Get-FileHash -LiteralPath $backup -Algorithm MD5).Hash -ne $entry.md5) { throw "Backup verification failed: $($entry.path)" }
    if ($restorePlan.ContainsKey($entry.path)) { throw 'Duplicate backup entry.' }
    $restorePlan.Add($entry.path, [pscustomobject]@{ Path = $entry.path; Target = $target; Backup = $backup; Action = 'restore' })
}
foreach ($entry in $restoreRecord.disk_changes) {
    if ($entry.change -ne 'created') { continue }
    if ($entry.path -notlike 'Content/*' -or $restorePlan.ContainsKey($entry.path)) { throw 'Invalid created-file entry.' }
    $target = Get-CheckedLocalPath $entry.path
    $restorePlan.Add($entry.path, [pscustomobject]@{ Path = $entry.path; Target = $target; Backup = $null; Action = 'archive-created' })
}
foreach ($entry in $restorePlan.Values) {
    if (Test-Path -LiteralPath $entry.Target -PathType Container) { throw "Expected a file: $($entry.Path)" }
}
$restorePlan.Values | Sort-Object Path | Select-Object Action, Path
if (-not $Apply) {
    Write-Output 'Preview only. Close Unreal Editor, then rerun with -Apply to restore. Current files will be archived first.'
    return
}
if (@(Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'Close Unreal Editor before restoring on-disk assets.'
}

$restoreStamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmss') + '-' + [Guid]::NewGuid().ToString('N')
$restoreArchiveRelative = "$restoreRunRelative/before-restore/$restoreStamp"
$restoreArchiveRoot = Get-CheckedLocalPath $restoreArchiveRelative
New-Item -ItemType Directory -Path $restoreArchiveRoot -Force | Out-Null
$restoreCurrent = @()
# Preserve every current file before applying any restoration.
foreach ($entry in $restorePlan.Values) {
    if (-not (Test-Path -LiteralPath $entry.Target -PathType Leaf)) { continue }
    $archive = Get-CheckedLocalPath "$restoreArchiveRelative/$($entry.Path)"
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($archive)) -Force | Out-Null
    $hash = (Get-FileHash -LiteralPath $entry.Target -Algorithm MD5).Hash
    Copy-Item -LiteralPath $entry.Target -Destination $archive
    if ((Get-FileHash -LiteralPath $archive -Algorithm MD5).Hash -ne $hash) { throw 'Current-file archival failed; restoration stopped.' }
    $restoreCurrent += [ordered]@{ path = $entry.Path; md5 = $hash }
}
$restoreRecovery = [ordered]@{ status = 'prepared'; original_run = $RunId; current_files = $restoreCurrent }
$restoreRecoveryPath = Get-CheckedLocalPath "$restoreArchiveRelative/recovery.json"
$restoreRecovery | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $restoreRecoveryPath -Encoding utf8NoBOM
foreach ($entry in $restorePlan.Values) {
    # Revalidate the exact absolute target before every file mutation.
    $target = Get-CheckedLocalPath $entry.Path
    if ($entry.Action -eq 'restore') {
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
        Copy-Item -LiteralPath $entry.Backup -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target -Algorithm MD5).Hash -ne
            (Get-FileHash -LiteralPath $entry.Backup -Algorithm MD5).Hash) { throw "Restored-file verification failed: $($entry.Path)" }
    } elseif (Test-Path -LiteralPath $target -PathType Leaf) {
        $archive = Get-CheckedLocalPath "$restoreArchiveRelative/$($entry.Path)"
        if ((Get-FileHash -LiteralPath $archive -Algorithm MD5).Hash -ne
            (Get-FileHash -LiteralPath $target -Algorithm MD5).Hash) { throw 'Created file changed since archival; refusing removal.' }
        Remove-Item -LiteralPath $target
    }
}
$restoreRecovery.status = 'completed'
$restoreRecovery | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $restoreRecoveryPath -Encoding utf8NoBOM
Write-Output "Restored recorded files. Previous current files are preserved under $restoreArchiveRelative"
if ($restoreRecord.change_scan_complete -ne $true) {
    Write-Output 'The run did not complete its change scan. Unrecorded new files and effects outside the declared folders are not restored.'
}
