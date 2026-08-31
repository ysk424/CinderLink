# CinderLink

CinderLink is a local-first, auditable AI agent panel for Unreal Editor 5.8 on Windows. It connects the editor to the official Codex App Server over private standard input/output pipes. It does not open a listening port, ship a runtime script, collect telemetry, or operate a CinderLink server.

> **Early preview:** CinderLink is security-oriented, but no software can promise absolute safety. Review the source, read the threat model, and keep backups or version control for every Unreal project.

## Security defaults

- Starts only after the user presses **Connect**.
- Shows the resolved `codex.exe` path before launch.
- Uses local JSONL over anonymous `stdio` pipes; there is no inbound network listener.
- Gives the child process a small environment allowlist. API keys, GitHub tokens, cloud credentials, proxy overrides, and unrelated environment variables are not inherited. `TEMP` and `TMP` point inside the current project.
- Uses custom Codex permission profiles whose only filesystem root is the current Unreal project, with tool network access disabled.
- Requires the elevated Windows sandbox. If Codex cannot enforce the split read boundary, CinderLink fails closed before a prompt can be sent.
- Defaults to read-only. Project edits require the user to enable **Allow project edits** for the turn.
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

The integration test starts the real local App Server using harmless canaries. It verifies the sanitized child environment, the project-only read boundary, and disabled MCP state. It does not send a model prompt.

## Upstream security mechanisms

CinderLink deliberately builds on the official [Codex App Server protocol](https://developers.openai.com/codex/app-server), [Codex permission profiles](https://learn.chatgpt.com/docs/permissions), and [Windows sandbox](https://learn.chatgpt.com/docs/windows/windows-sandbox). These permission-profile interfaces are currently beta upstream. Run `codex doctor --json` and confirm that the reported sandbox backend is `elevated` and sandbox provisioning is complete before use.

## Use

1. Confirm the displayed executable and project paths.
2. Press **Connect**.
3. Leave **Allow project edits** off for analysis and questions.
4. Enable it only for a turn that should modify the current project.
5. If CinderLink cannot verify the project boundary and disabled external tools, it remains disconnected and sends no prompt.

The model request itself is sent by the official Codex client to the configured OpenAI service. Files read into model context can therefore leave the PC as part of that request. CinderLink's boundary is designed to prevent agent tools from reading unrelated host files and environment credentials; it does not make model use offline and it cannot restrict the trusted official Codex executable itself. Do not place secrets inside the Unreal project.

## Scope of the first release

The initial release provides conversation streaming, new threads, turn interruption, read-only/edit modes, sanitized process launch, fail-closed permission handling, and selected command/file status events. It intentionally omits approval-based escalation, arbitrary shell shortcuts, remote listeners, MCP configuration editing, automatic update code, analytics, and credential management.

## 日本語

CinderLinkは、Unreal Editor 5.8から公式Codex App Serverを利用するための、Windows向けオープンソースプラグインです。通信はローカルの標準入出力だけを使い、待受ポート、独自サーバー、テレメトリ、ランタイム用外部スクリプトを持ちません。

初期状態は読み取り専用です。プロジェクトを書き換えるターンだけ **Allow project edits** を有効にしてください。アクセス可能なファイルを現在のUnrealプロジェクト内に限定し、外部MCP・アプリ・ブラウザ操作・プラグインなどを無効化してから接続完了とします。追加権限の要求はすべて拒否し、境界を確認できない場合はプロンプトを送らず停止します。Codexへ渡す子プロセス環境からAPIキー、GitHubトークン、クラウド資格情報などを除外します。

ただし、AIが読んだプロジェクト内ファイルは、通常のモデルリクエストの一部としてPC外へ送信され得ます。また、公式Codex実行ファイル、OS、Unreal Editor、接続先サービスは信頼する設計です。Unrealプロジェクト内にも秘密情報を置かないでください。

## Independence and trademarks

CinderLink contains no Unreal Engine source or assets and no third-party plugin code. Users obtain Unreal Engine separately under Epic's terms. See [PROVENANCE.md](PROVENANCE.md).

Unreal Engine is a trademark or registered trademark of Epic Games, Inc. OpenAI and Codex are trademarks or registered trademarks of OpenAI, L.L.C. CinderLink is not affiliated with or endorsed by either company.

## License

Licensed under the [Apache License 2.0](LICENSE).
