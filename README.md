# CinderLink

CinderLink is a local-first, auditable AI agent panel for Unreal Editor 5.8 on Windows. It connects the editor to the official Codex App Server over private standard input/output pipes. It does not open a listening port, ship a runtime script, collect telemetry, or operate a CinderLink server.

**Version 1.0.0** adds an opt-in Python authoring mode: generate and execute UE Python in the open editor, inspect output and exceptions, back up declared content folders, and export reusable scripts.

日本語の操作・復元手順: [USAGE.ja.md](Docs/USAGE.ja.md).
検証結果と限界: [VALIDATION.ja.md](Docs/VALIDATION.ja.md).

The bounded Editor tools and Python authoring have different permissions. Python runs inside Unreal Editor with its host permissions; it is not confined by the Codex command sandbox. See the [threat model](THREAT_MODEL.md).

## Security boundaries and operational defaults

- Connects automatically when the panel opens. The user can still press **Disconnect** at any time.
- Shows the resolved `codex.exe` path before launch.
- Uses local JSONL over anonymous `stdio` pipes; there is no inbound network listener.
- Gives the child process a small environment allowlist. API keys, GitHub tokens, cloud credentials, proxy overrides, and unrelated environment variables are not inherited. `TEMP` and `TMP` point inside the current project.
- Uses custom Codex permission profiles whose only filesystem root is the current Unreal project, with tool network access disabled.
- Requires the elevated Windows sandbox. If Codex cannot enforce the split read boundary, CinderLink fails closed before a prompt can be sent.
- Project-file edits and allowlisted Editor actions are enabled by default. Their checkboxes persist after each prompt and can be cleared whenever a read-only turn is preferred.
- Normal mode exposes a fixed set of bounded in-process Unreal Editor tools. Python authoring is a separate, default-off panel mode; it requires project edits and UE actions as well. Python may perform arbitrary host operations, including deletion, file access and network activity. The existing command sandbox is unchanged and does not sandbox Unreal.
- The bounded PIE-start and viewport-capture tools retain visible per-call confirmation. Unattended sessions refuse those two tools. Arbitrary Python can perform equivalent operations, so this is not a restriction on authoring-mode scripts.
- Enumerates configured MCP servers, disables them in the thread, and verifies that none expose tools before accepting a prompt. Apps, browser/computer control, plugins, hooks, image generation, and skill discovery are disabled at process startup.
- Uses `approvalPolicy: never` and automatically declines every command, file, network, or filesystem escalation request. There is no approval button that can broaden the boundary.
- Stops the Codex process tree when the panel disconnects or Unreal Editor exits.
- Does not log raw conversation messages. Python source/results are archived locally; script output and exceptions can also appear in Unreal's own logs.

See [THREAT_MODEL.md](THREAT_MODEL.md) for boundaries and residual risks.

## Requirements

- Windows 11 (64-bit recommended; Windows 10 is not currently verified)
- Unreal Engine 5.8
- A current official Codex CLI with custom permission profiles and the elevated Windows sandbox (tested with 0.151.0)
- A Codex login configured outside Unreal Editor

CinderLink deliberately does not accept an API key in its UI or read one from the environment. Authenticate with the official CLI before opening the editor:

```powershell
codex login
```

## Install from source

1. Download or clone this repository.
2. Copy the repository folder to `<YourProject>/Plugins/CinderLink`.
3. Do not copy `Binaries`, `Intermediate`, or `BuildArtifacts` from another machine.
4. Open the project and allow Unreal Engine to compile the plugin.
5. Enable **CinderLink** under **Edit > Plugins**, then restart the editor.
6. Open **Window > CinderLink**.

For a local package build:

```powershell
./Scripts/Build-UE58.ps1
```

Run the repository audit, package build, and Unreal automation suite together with:

```powershell
./Scripts/Test-UE58.ps1
```

The integration test starts the real local App Server using harmless canaries. It verifies the sanitized child environment, project-only command read boundary, disabled MCP state, built-in Editor tool policy, and acceptance of the dynamic tool schema. Python tests execute real scripts, create and modify a material, verify backups, export recipes, and check exception evidence. The suite does not send a model prompt.

## Upstream security mechanisms

CinderLink deliberately builds on the official [Codex App Server protocol](https://learn.chatgpt.com/docs/app-server), [Codex permission profiles](https://learn.chatgpt.com/docs/permissions), and [Windows sandbox](https://learn.chatgpt.com/docs/windows/windows-sandbox). These permission-profile interfaces are currently beta upstream. Run `codex doctor --json` and confirm that the reported sandbox backend is `elevated` and sandbox provisioning is complete before use.

## Use

1. Confirm the displayed executable and project paths.
2. Wait for the automatic connection to report **Ready**. Press **Disconnect** if the agent is not needed.
3. Both **Allow project file edits** and **Allow UE Editor actions** start enabled and remain in their current state after a prompt.
4. Clear **Allow project file edits** for analysis or questions that should not change project files.
5. Clear **Allow UE Editor actions** when a turn should inspect, but not change, the open level, assets, viewport, or PIE state. This is separate from direct Codex filesystem writes, although saving a level or importing an asset naturally makes Unreal write `.umap` or `.uasset` files inside the project.
6. If CinderLink cannot verify the project boundary and disabled external tools, it remains disconnected and sends no prompt.

## Python authoring

Enable **Enable Python authoring (Unreal host permissions)** before sending an authoring turn. The banner describes the broader boundary. The toggle stays on while working, but resets on disconnect, new thread, Stop turn, or disabling either edit permission. It cannot be enabled by the model or during an active turn. Turning it off revokes subsequent Python calls; an already executing script is not interrupted.

| Tool | Behavior |
| --- | --- |
| `ue_python_status` | Read Python availability and active-turn authoring permission. |
| `ue_python_execute` | Archive source, snapshot declared content directories, execute UE Python, return output/errors and disk changes. |
| `ue_python_get_run` | Read a previous result by run ID, including after authoring is disabled. |
| `ue_python_save_recipe` | Copy the exact archived script into `Scripts/CinderLink/<name>.py`; refuse overwrite or an altered archive. |

Execution arguments are `label`, `code` and `backup_paths`. Example backup paths: `["/Game/Row/Lookdev"]`; use `[]` only for inspection. Declare all affected folders, including shared dependencies and World Partition external actor/object directories. This declaration is a backup request, not a Python access restriction. Dirty packages in those folders must be saved or reverted first. Backup failure prevents execution. Each call is limited to 128 Ki characters of source and 1 GiB / 10,000 existing backup files.

Every run gets `Saved/CinderLink/Python/<run-id>/script.py`, `result.json`, and `backup/Content/...`. Source and backup metadata are written before execution; status distinguishes prepared, running, completed and failed. Output and error text are bounded to about 32 Ki characters each. Exact code can be exported as a recipe for Git review. Generated assets and private run archives do not belong in this plugin repository.

Python runs on Unreal's game thread using the official Python Editor Script Plugin. Private execution scope separates globals; it is **not a security sandbox**. Keep calls short and finite. There is no hard timeout, forced cancellation, automatic rollback, or guarantee that Undo covers asset saves. Failed scripts may leave partial changes. Do not use persistent callbacks, background work, host secrets, unrelated files, hardware or network in authoring scripts. These instructions are not a technical barrier to arbitrary Python. PIE/Simulate must be stopped before using the execution tool.

To recover recorded disk changes, close Unreal Editor and use the bundled helper:

```powershell
./Scripts/Restore-PythonRun.ps1 -ProjectRoot 'C:/path/to/UnrealProject' -RunId '<run-id>'
# Inspect the preview, then add -Apply to restore.
```

The helper validates paths and backup hashes, preserves current files before restoration, and archives files that this run recorded as newly created. It does not recover unsaved state, undeclared dependencies, unrecorded new files after a crash, or arbitrary host side effects. See the [Japanese guide](Docs/USAGE.ja.md) for the full workflow.

The model request itself is sent by the official Codex client to the configured OpenAI service. Files read into model context can therefore leave the PC as part of that request. CinderLink's boundary is designed to prevent agent tools from reading unrelated host files and environment credentials; it does not make model use offline and it cannot restrict the trusted official Codex executable itself. Do not place secrets inside the Unreal project.

## Built-in Unreal Editor actions

Read-only calls can inspect Editor/PIE state, list and inspect actors, query `/Game` assets, and run Map Check. While the Editor-actions checkbox is enabled (the default), Codex may create/load/save `/Game` levels, spawn or update actors, set the level GameMode, import a project-local image as a new asset, move the viewport camera, capture the viewport, and start/stop PIE. Changes use Unreal transactions where applicable.

The original creation/update tools remain bounded: levels and assets stay under `/Game`, image import never replaces assets, image sources must be real project-local files of an allowlisted type, sensitive-named actor properties are hidden, and no bounded delete primitive exists. These limits do not apply to arbitrary authoring Python. Viewport capture and PIE-start tools retain their separate confirmation.

The App Server's client-defined `dynamicTools` and `item/tool/call` interfaces used for these actions are currently experimental upstream. CinderLink validates the active thread and turn, executes calls only on Unreal's game thread, and rejects unknown tool names.

## Arrietty-UE and Cesium

CinderLink is project-agnostic and works from `Arrietty.uproject`; it does not copy or depend on Arrietty source. Its reflection-based actor tools cover ordinary editable primitive properties and actor references, including common Cesium georeference and tileset settings such as origin coordinates, ion asset ID, and maximum screen-space error when those properties are exposed by the installed Cesium version. Exact available names should first be read with `ue_level_get_actor`.

This is Editor automation, not an Arrietty runtime dependency. CinderLink is not packaged into Shipping builds, and starting PIE still requires the user-facing confirmation.

## Release scope

Version 1.0.0 adds explicitly enabled UE Python authoring, per-run source/results/backups, recipe export and offline recovery. It retains the 0.2.1 auto-connect behavior and persistent bounded-edit toggles. It does not add remote listeners, MCP configuration editing, automatic updates, analytics or credential management. Python authoring changes the host trust boundary openly; it is not an expansion of the Codex command permission profile.

## 日本語

CinderLinkは、Unreal Editor 5.8から公式Codex App Serverを利用するための、Windows向けオープンソースプラグインです。通信はローカルの標準入出力だけを使い、待受ポート、独自サーバー、テレメトリ、ランタイム用外部スクリプトを持ちません。

パネルを開くと自動接続します。初期状態では **Allow project file edits** と **Allow UE Editor actions** の両方が有効で、送信後もチェック状態を維持します。解析だけを行うターンでは、必要に応じて一方または両方を外してください。PIE開始とViewport画像送信には、チェック状態にかかわらず毎回Yes/No確認が表示されます。

1.0.0 では、通常の UE 操作に加え、明示的に有効にする Python 制作モードを追加しました。コード保存・指定素材のバックアップ・実行・結果確認・再利用用スクリプト保存ができます。Python は UE Editor の権限で動き、プロジェクト外へのアクセスも可能です。通常の Codex のファイル制限は Python には適用されません。詳しくは [日本語ガイド](Docs/USAGE.ja.md) を参照してください。CinderLink は Editor 専用なので Shipping には入りません。

Codex の通常ファイル操作は現在の Unreal プロジェクト内に限定し、外部 MCP・アプリ・ブラウザ操作などを無効化してから接続完了とします。追加権限の要求は拒否し、境界を確認できない場合はプロンプトを送らず停止します。Python 制作モードは別の実行経路であり、この制限による隔離を保証しません。Codex 子プロセスの環境から資格情報などを除外する従来の処理は維持します。

ただし、AIが読んだプロジェクト内ファイルは、通常のモデルリクエストの一部としてPC外へ送信され得ます。また、公式Codex実行ファイル、OS、Unreal Editor、接続先サービスは信頼する設計です。Unrealプロジェクト内にも秘密情報を置かないでください。

## Independence and trademarks

CinderLink contains no Unreal Engine source or assets and no third-party plugin code. Users obtain Unreal Engine separately under Epic's terms. See [PROVENANCE.md](PROVENANCE.md).

Unreal Engine is a trademark or registered trademark of Epic Games, Inc. OpenAI and Codex are trademarks or registered trademarks of OpenAI, L.L.C. CinderLink is not affiliated with or endorsed by either company.

## License

Licensed under the [Apache License 2.0](LICENSE).
