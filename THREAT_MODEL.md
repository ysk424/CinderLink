# Threat model

## Goal

Allow a user to work with a coding agent inside Unreal Editor while reducing the chance that unrelated host files, environment credentials, or services become available to agent-controlled tools.

Version 1.0.0 has two modes. The original bounded tools keep their restrictions. **Python authoring explicitly permits arbitrary code inside the trusted Unreal process.** In that mode, host files, environment, network, native APIs, asset deletion and equivalent PIE/capture operations are reachable by Python. The Codex command sandbox does not constrain this execution path. Do not advertise authoring mode as project-isolated.

At the user's explicit request, **1.0.2 enables Python authoring by default**. Opening a new panel selects ON without a separate opt-in click. Users who want bounded tools only must clear that checkbox before submitting. The choice survives reconnect, new thread and Stop turn while the panel is open; closing and reopening starts with ON again. Active-turn authority is still revoked on stop, disconnect and completion, and each new submitted turn takes a fresh snapshot of the panel permissions.

## Protected assets

- API keys, source-control tokens, cloud credentials, and unrelated environment variables.
- Files outside the current Unreal project.
- The host's inbound network surface.
- Prevention of unattended permission escalation.
- Unreal levels and assets against unapproved mutation, deletion, or replacement.
- User awareness before PIE runtime code runs or a viewport image is sent to the model.

## Trust boundaries

CinderLink trusts the local Unreal Editor process, the exact `codex.exe` selected by the user, the operating system, and the upstream service configured by Codex. Model output, project content, requested commands, and App Server messages are treated as untrusted.

## Controls

| Threat | Control |
| --- | --- |
| Secret environment-variable inheritance | Construct a new child environment from a fixed non-secret allowlist. |
| Hidden data listener | Use anonymous `stdio` pipes only; do not bind sockets. |
| Codex command host file writes | The default command edit profile grants writes only under the current project root. The user can clear the persistent project-edit toggle to select the read-only profile for a turn. This does not sandbox UE Python. |
| Codex command reads of unrelated host files | Require Codex's elevated Windows sandbox and a custom profile whose only filesystem entry is `:workspace_roots`. Refuse startup if the profile is unavailable. This does not sandbox Unreal. |
| Accidental temp-directory expansion | Replace `TEMP` and `TMP` with a project-local directory before starting Codex. |
| External agent tool exfiltration | Disable command-tool network access, external features, and every discovered MCP server; verify MCP runtime status again inside the thread before sending prompts. Arbitrary UE Python can still use host network access. |
| Permission escalation | Use `approvalPolicy: never`; decline every command, file, and permission escalation request. |
| Process persistence | Put the App Server in a Windows Job object configured to terminate descendants on close. |
| Sensitive Unreal logs | Do not log raw prompts, responses, JSON messages, or child output. |
| Protocol confusion | Parse one bounded JSON object per line and fail closed on unknown server requests. |
| Unbounded Editor mutation | Normal mode uses fixed native actions behind **Allow UE Editor actions**. Arbitrary Python requires a separate explicit authoring selection and both edit permissions. |
| Arbitrary Editor execution | Python authoring is default-on in 1.0.2 and visibly indicates host permissions. The ON/OFF preference survives connection/turn controls; active-turn authority is still revoked on stop/disconnect/completion. No in-process security sandbox is claimed. |
| Steering changes turn authority or loses input | Additional messages use `turn/steer` with the current thread and expected turn ID, without model or permission overrides. Failed or unacknowledged delivery leaves the draft for explicit retry; no automatic new turn is started. |
| Project asset loss | Bounded tools restrict targets and transitions; Python snapshots declared disk folders before running. Neither transactions nor selective backups are whole-project rollback. |
| Host-file import | Accept only bounded PNG/JPEG/EXR/HDR files under the project root and reject traversal or Windows reparse points. |
| Runtime side effects | The bounded PIE tool requires confirmation and refuses unattended use. The Python execution tool refuses active/queued PIE, but arbitrary scripts can perform equivalent operations. |
| Image disclosure | The bounded capture tool requires confirmation, downsizes the image and discloses model transmission. This is not an enforced restriction on arbitrary Python. |

## Residual risks

- Python authoring is on by default and requires both edit permissions for the submitted turn. Manual OFF remains off while that panel is open; disabling either edit permission also turns Python off. The model cannot enable the mode through a tool. Revocation blocks subsequent bridge calls; it does not undo past execution or stop native code already running.
- Python scripts can exceed every bounded-tool restriction, including access to host credentials, files, devices and network. Import restrictions, AST filters, source-directory checks and private Python globals would not provide a trustworthy sandbox; none is claimed here. Model instructions to avoid these operations are behavioral guidance only.
- Source code and bounded output/errors are saved under project `Saved/CinderLink/Python`. Unreal's Python logger may also write output and tracebacks. Never include secrets or personal/session data in scripts or output. A malicious script can alter its own archive or any other Editor-accessible file; archives and MD5 comparisons detect ordinary corruption, not adversarial tampering.
- Backups cover only declared Content directories already saved on disk, up to 1 GiB / 10,000 files. Dirty packages in declared folders are refused. Dependencies elsewhere, World Partition external packages omitted from the declaration, transient state and non-file side effects are not backed up. A declaration of `[]` is not enforced as read-only Python.
- Scripts run synchronously on the game thread. There is no safe hard timeout or guaranteed cancellation of arbitrary Python/native calls, and excessive output can consume memory before response truncation. Keep jobs short. A crash may leave a `running` archive without the after-scan; new files then need separate review.
- Recovery operates offline on validated project-local paths, verifies backup hashes and preserves current files before restoring. It does not infer omitted dependencies or provide whole-project atomic rollback. The current-file archive supports manual recovery if a restore is interrupted.
- Writable project files can themselves contain executable startup hooks, and Unreal may execute those independently. Disabling the Python bridge is not a security boundary against malicious project code already loaded by the trusted Editor. The agent is instructed not to bypass authoring mode through startup hooks or alternate execution routes.

- Content read from the current project may be sent to the configured model provider as part of normal Codex operation.
- Secrets stored inside the project are in scope and may be disclosed to the model.
- Custom permission profiles and the elevated Windows sandbox are upstream Codex security mechanisms. A change or defect in them can weaken the boundary. CinderLink refuses to continue when the expected profiles, runtime root, or disabled MCP state cannot be confirmed, but users should keep Codex current.
- A malicious or replaced `codex.exe` runs with the permissions of the editor, although its inherited environment is minimized. Always inspect the resolved path.
- Commands and project edits within the selected profile can still be harmful to the project. Keep backups and use version control.
- Project-file edits and allowlisted Editor mutations are enabled by default and remain enabled between turns. A model mistake or malicious project content can therefore cause an in-scope mutation without fresh per-turn consent; clear the relevant checkbox before analysis-only prompts.
- Unreal Editor itself has broad access to project and host data. CinderLink cannot sandbox the editor.
- An allowlisted Editor action runs inside the trusted Unreal process. Actor construction, property-change handlers, third-party Editor plugins, and PIE runtime code may themselves perform filesystem, hardware, or network activity outside CinderLink's Codex sandbox.
- The model may make an incorrect but permitted Editor change. Transactions and the absence of delete/overwrite primitives reduce impact but do not replace source control or backups.
- Client-defined App Server tools are experimental upstream and may change. A protocol compatibility failure should reject the thread or tool call, but users should test upgrades before production use.
- The official Codex service, authentication files, dependencies, and operating-system sandbox remain outside this repository's implementation boundary.

## Explicit non-goals

- Providing a general remote administration interface.
- Managing or storing API keys.
- Enabling arbitrary outbound HTTP requests from tools.
- Sandboxing arbitrary Python inside the already-running Unreal Editor, or guaranteeing complete rollback of its effects.
- Providing an approval path that broadens filesystem or network permissions.
- Claiming formal verification or absolute security.
