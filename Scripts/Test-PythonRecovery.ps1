# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ysk424 and CinderLink contributors

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$recoveryTestRepo = Split-Path -Parent $PSScriptRoot
$recoveryTestRoot = Join-Path $recoveryTestRepo ('BuildArtifacts/RecoveryTest-' + [Guid]::NewGuid().ToString('N'))
$recoveryTestRun = 'recovery-test'
$recoveryTestArchive = Join-Path $recoveryTestRoot "Saved/CinderLink/Python/$recoveryTestRun"
$recoveryTestBackup = Join-Path $recoveryTestArchive 'backup/Content/Test'
$recoveryTestContent = Join-Path $recoveryTestRoot 'Content/Test'
New-Item -ItemType Directory -Path $recoveryTestBackup, $recoveryTestContent -Force | Out-Null
'{"FileVersion":3}' | Set-Content -LiteralPath (Join-Path $recoveryTestRoot 'RecoveryTest.uproject') -Encoding utf8NoBOM
$recoveryTestOriginal = Join-Path $recoveryTestBackup 'original note.txt'
$recoveryTestTarget = Join-Path $recoveryTestContent 'original note.txt'
$recoveryTestNew = Join-Path $recoveryTestContent 'new.txt'
[IO.File]::WriteAllText($recoveryTestOriginal, 'original')
[IO.File]::WriteAllText($recoveryTestTarget, 'changed')
[IO.File]::WriteAllText($recoveryTestNew, 'new')
$recoveryTestRecord = [ordered]@{
    run_id = $recoveryTestRun
    backups = @([ordered]@{ path = 'Content/Test/original note.txt'; md5 = (Get-FileHash -LiteralPath $recoveryTestOriginal -Algorithm MD5).Hash })
    disk_changes = @([ordered]@{ path = 'Content/Test/new.txt'; change = 'created' })
    change_scan_complete = $true
}
$recoveryTestManifest = Join-Path $recoveryTestArchive 'result.json'
$recoveryTestRecord | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $recoveryTestManifest -Encoding utf8NoBOM
$recoveryTestScript = Join-Path $PSScriptRoot 'Restore-PythonRun.ps1'

& $recoveryTestScript -ProjectRoot $recoveryTestRoot -RunId $recoveryTestRun | Out-Null
if ([IO.File]::ReadAllText($recoveryTestTarget) -ne 'changed' -or -not (Test-Path -LiteralPath $recoveryTestNew)) {
    throw 'Preview modified content.'
}
& $recoveryTestScript -ProjectRoot $recoveryTestRoot -RunId $recoveryTestRun -Apply | Out-Null
if ([IO.File]::ReadAllText($recoveryTestTarget) -ne 'original' -or (Test-Path -LiteralPath $recoveryTestNew)) {
    throw 'Recovery did not restore the original state.'
}
$recoveryTestSaved = @(Get-ChildItem -LiteralPath (Join-Path $recoveryTestArchive 'before-restore') -Directory)
if ($recoveryTestSaved.Count -ne 1 -or
    [IO.File]::ReadAllText((Join-Path $recoveryTestSaved[0].FullName 'Content/Test/original note.txt')) -ne 'changed' -or
    [IO.File]::ReadAllText((Join-Path $recoveryTestSaved[0].FullName 'Content/Test/new.txt')) -ne 'new') {
    throw 'Recovery did not preserve the current content.'
}

[IO.File]::WriteAllText($recoveryTestOriginal, 'corrupt')
$recoveryTestRejected = $false
try { & $recoveryTestScript -ProjectRoot $recoveryTestRoot -RunId $recoveryTestRun -Apply | Out-Null } catch { $recoveryTestRejected = $true }
if (-not $recoveryTestRejected -or [IO.File]::ReadAllText($recoveryTestTarget) -ne 'original') {
    throw 'Corrupted backup was not rejected before mutation.'
}
$recoveryTestRecord.backups[0].path = 'Content/../../outside.txt'
$recoveryTestRecord | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $recoveryTestManifest -Encoding utf8NoBOM
$recoveryTestRejected = $false
try { & $recoveryTestScript -ProjectRoot $recoveryTestRoot -RunId $recoveryTestRun -Apply | Out-Null } catch { $recoveryTestRejected = $true }
if (-not $recoveryTestRejected) { throw 'Traversal in recovery metadata was not rejected.' }
Write-Output 'PASS recovery preview, restore, current-file preservation, corrupt-backup refusal, traversal refusal.'
