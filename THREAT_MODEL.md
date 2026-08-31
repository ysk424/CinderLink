# Threat model

## Goal

Allow a user to work with a coding agent inside Unreal Editor while reducing the chance that unrelated host files, environment credentials, or services become available to agent-controlled tools.

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
| Arbitrary host file writes | The default edit profile grants writes only under the current project root. The user can clear the persistent project-edit toggle to select the read-only profile for a turn. |
| Reading unrelated host files | Require Codex's elevated Windows sandbox and a custom profile whose only filesystem entry is `:workspace_roots`. Refuse startup if the profile is unavailable. |
| Accidental temp-directory expansion | Replace `TEMP` and `TMP` with a project-local directory before starting Codex. |
| Tool-based exfiltration | Disable tool network access, external features, and every discovered MCP server; verify MCP runtime status again inside the thread before sending prompts. |
| Permission escalation | Use `approvalPolicy: never`; decline every command, file, and permission escalation request. |
| Process persistence | Put the App Server in a Windows Job object configured to terminate descendants on close. |
| Sensitive Unreal logs | Do not log raw prompts, responses, JSON messages, or child output. |
| Protocol confusion | Parse one bounded JSON object per line and fail closed on unknown server requests. |
| Unbounded Editor mutation | Keep read-only UE inspection separate from the persistent **Allow UE Editor actions** toggle, expose only fixed native actions, and continue to require per-call confirmation for PIE and viewport image capture. |
| Arbitrary Editor execution | Expose fixed native functions only; provide no arbitrary Python, console-command, Blueprint-call, delete, or external MCP tool. |
| Project asset loss | Restrict mutable level/asset targets to `/Game`, refuse dirty-level transitions and existing import/create targets, and use Unreal transactions where applicable. |
| Host-file import | Accept only bounded PNG/JPEG/EXR/HDR files under the project root and reject traversal or Windows reparse points. |
| Runtime side effects | Require a visible per-call confirmation before PIE start and refuse it in unattended sessions. |
| Image disclosure | Require a visible per-call confirmation, downsize the active viewport capture, save it under the project's `Saved` directory, and disclose that it is sent to the model. |

## Residual risks

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
- Providing general Python, console-command, Blueprint-call, deletion, or arbitrary reflection execution in Unreal Editor.
- Providing an approval path that broadens filesystem or network permissions.
- Claiming formal verification or absolute security.
