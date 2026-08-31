# Contributing

Thank you for helping make editor-agent integration safer and easier to audit.

## Requirements

- Submit only work you have the right to license under Apache-2.0.
- Do not copy, translate, decompile, or adapt code, UI, strings, assets, or tests from proprietary Unreal AI extensions.
- Base protocol behavior on public documentation or schemas produced by an official local tool.
- Add the repository's SPDX header to new C++ and script files.
- Keep runtime behavior local-first: no telemetry, updater, listener, credential collection, or new outbound destination without an explicit design discussion.
- Never weaken fail-closed permission handling, project-only filesystem profiles, external-tool disabling, or secret-environment filtering silently.

By submitting a contribution, you agree that it is provided under the repository's Apache License 2.0 terms.

## Before opening a pull request

```powershell
./Scripts/Audit-Repository.ps1
./Scripts/Build-UE58.ps1
./Scripts/Test-UE58.ps1
```

Document security-relevant behavior changes in `THREAT_MODEL.md`.
