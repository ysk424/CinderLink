# CinderLink

CinderLink is a local-first, auditable AI agent panel for Unreal Editor 5.8 on Windows. It connects the editor to the official Codex App Server over private standard input/output pipes. It does not open a listening port, ship a runtime script, collect telemetry, or operate a CinderLink server.

> **Early preview:** CinderLink is security-oriented, but no software can promise absolute safety. Review the source, read the threat model, and keep backups or version control for every Unreal project.

## Security boundaries and operational defaults

- Connects automatically when the panel opens. The user can still press **Disconnect** at any time.
- Shows the resolved `codex.exe` path before launch.
- Uses local JSONL over anonymous `stdio` pipes; there is no inbound network listener.
- Gives the child process a small environment allowlist. API keys, GitHub tokens, cloud credentials, proxy overrides, and unrelated environment variables are not inherited. `TEMP` and `TMP` point inside the current project.
- Uses custom Codex permission profiles whose only filesystem root is the current Unreal project, with tool network access disabled.
- Requires the elevated Windows sandbox. If Codex cannot enforce the split read boundary, CinderLink fails closed before a prompt can be sent.
- Project-file edits and allowlisted Editor actions are enabled by default. Their checkboxes persist after each prompt and can be cleared whenever a read-only turn is preferred.
- Exposes only a fixed set of in-process Unreal Editor tools. It has no arbitrary Python, Blueprint, or console-command execution and no actor-deletion, asset-deletion, or overwrite tool.
- Requires an additional visible Yes/No confirmation before starting PIE or capturing and sending a viewport image. Unattended sessions refuse both actions.
- Enumerates configured MCP servers, disables them in the thread, and verifies that none expose tools before accepting a prompt. Apps, browser/computer control, plugins, hooks, image generation, and skill discovery are disabled at process startup.
- Uses `approvalPolicy: never` and automatically declines every command, file, network, or filesystem escalation request. There is no approval button that can broaden the boundary.
- Stops the Codex process tree when the panel disconnects or Unreal Editor exits.
- Does not write conversation content to Unreal logs.

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

The integration test starts the real local App Server using harmless canaries. It verifies the sanitized child environment, project-only read boundary, disabled MCP state, built-in Editor tool policy, and acceptance of the dynamic tool schema. It does not send a model prompt.

## Upstream security mechanisms

CinderLink deliberately builds on the official [Codex App Server protocol](https://learn.chatgpt.com/docs/app-server), [Codex permission profiles](https://learn.chatgpt.com/docs/permissions), and [Windows sandbox](https://learn.chatgpt.com/docs/windows/windows-sandbox). These permission-profile interfaces are currently beta upstream. Run `codex doctor --json` and confirm that the reported sandbox backend is `elevated` and sandbox provisioning is complete before use.

## Use

1. Confirm the displayed executable and project paths.
2. Wait for the automatic connection to report **Ready**. Press **Disconnect** if the agent is not needed.
3. Both **Allow project file edits** and **Allow UE Editor actions** start enabled and remain in their current state after a prompt.
4. Clear **Allow project file edits** for analysis or questions that should not change project files.
5. Clear **Allow UE Editor actions** when a turn should inspect, but not change, the open level, assets, viewport, or PIE state. This is separate from direct Codex filesystem writes, although saving a level or importing an asset naturally makes Unreal write `.umap` or `.uasset` files inside the project.
6. If CinderLink cannot verify the project boundary and disabled external tools, it remains disconnected and sends no prompt.

The model request itself is sent by the official Codex client to the configured OpenAI service. Files read into model context can therefore leave the PC as part of that request. CinderLink's boundary is designed to prevent agent tools from reading unrelated host files and environment credentials; it does not make model use offline and it cannot restrict the trusted official Codex executable itself. Do not place secrets inside the Unreal project.

## Built-in Unreal Editor actions

Read-only calls can inspect Editor/PIE state, list and inspect actors, query `/Game` assets, and run Map Check. While the Editor-actions checkbox is enabled (the default), Codex may create/load/save `/Game` levels, spawn or update actors, set the level GameMode, import a project-local image as a new asset, move the viewport camera, capture the viewport, and start/stop PIE. Changes use Unreal transactions where applicable.

Creation and update are deliberately bounded: levels and assets stay under `/Game`, existing assets are not replaced, image sources must be real project-local files of an allowlisted type, sensitive-named actor properties are hidden, and no delete primitive exists. Viewport capture sends an image to the configured model only after a separate visible confirmation. PIE start has the same confirmation because Arrietty runtime code can access BLE, VR, ESP32, audio, or network services.

The App Server's client-defined `dynamicTools` and `item/tool/call` interfaces used for these actions are currently experimental upstream. CinderLink validates the active thread and turn, executes calls only on Unreal's game thread, and rejects unknown tool names.

## Arrietty-UE and Cesium

CinderLink is project-agnostic and works from `Arrietty.uproject`; it does not copy or depend on Arrietty source. Its reflection-based actor tools cover ordinary editable primitive properties and actor references, including common Cesium georeference and tileset settings such as origin coordinates, ion asset ID, and maximum screen-space error when those properties are exposed by the installed Cesium version. Exact available names should first be read with `ue_level_get_actor`.

This is Editor automation, not an Arrietty runtime dependency. CinderLink is not packaged into Shipping builds, and starting PIE still requires the user-facing confirmation.

## Release scope

Version 0.2 adds the bounded Unreal Editor bridge to the original conversation streaming, new-thread, interruption, project read/edit modes, sanitized process launch, fail-closed permission handling, and selected command/file status events. Version 0.2.1 automatically connects when the panel opens and keeps both bounded edit toggles enabled by default. CinderLink intentionally omits approval-based escalation, arbitrary shell shortcuts, arbitrary Editor scripting, remote listeners, MCP configuration editing, automatic update code, analytics, and credential management.

## 日本語

CinderLinkは、Unreal Editor 5.8から公式Codex App Serverを利用するための、Windows向けオープンソースプラグインです。通信はローカルの標準入出力だけを使い、待受ポート、独自サーバー、テレメトリ、ランタイム用外部スクリプトを持ちません。

パネルを開くと自動接続します。初期状態では **Allow project file edits** と **Allow UE Editor actions** の両方が有効で、送信後もチェック状態を維持します。解析だけを行うターンでは、必要に応じて一方または両方を外してください。PIE開始とViewport画像送信には、チェック状態にかかわらず毎回Yes/No確認が表示されます。

UE操作はプラグイン内に固定実装した許可リストだけです。任意Python、任意Console Command、Actor/Asset削除、既存Assetの上書きは公開していません。Arrietty-UEでは通常のActorに加えて、Cesium Georeferenceや3D Tilesetの公開された編集可能プロパティも読み取り・設定できます。CinderLinkはEditor専用なのでShippingには入りません。

アクセス可能なファイルを現在のUnrealプロジェクト内に限定し、外部MCP・アプリ・ブラウザ操作・プラグインなどを無効化してから接続完了とします。追加権限の要求はすべて拒否し、境界を確認できない場合はプロンプトを送らず停止します。Codexへ渡す子プロセス環境からAPIキー、GitHubトークン、クラウド資格情報などを除外します。

ただし、AIが読んだプロジェクト内ファイルは、通常のモデルリクエストの一部としてPC外へ送信され得ます。また、公式Codex実行ファイル、OS、Unreal Editor、接続先サービスは信頼する設計です。Unrealプロジェクト内にも秘密情報を置かないでください。

## Independence and trademarks

CinderLink contains no Unreal Engine source or assets and no third-party plugin code. Users obtain Unreal Engine separately under Epic's terms. See [PROVENANCE.md](PROVENANCE.md).

Unreal Engine is a trademark or registered trademark of Epic Games, Inc. OpenAI and Codex are trademarks or registered trademarks of OpenAI, L.L.C. CinderLink is not affiliated with or endorsed by either company.

## License

Licensed under the [Apache License 2.0](LICENSE).
