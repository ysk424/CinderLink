# Security policy

## Supported versions

CinderLink is an early preview. Security fixes are applied to the latest release and the default branch.

## Reporting a vulnerability

Please do not publish an exploit or secret in a public issue. Use GitHub's **Report a vulnerability** private reporting feature for this repository. Include:

- the affected version or commit;
- the observed behavior and expected security boundary;
- minimal reproduction steps;
- whether credentials or private files may have been exposed.

If private vulnerability reporting is unavailable, open a public issue containing no exploit details or secrets and ask the maintainer to establish a private channel.

## Immediate response

If you suspect credential exposure:

1. Disconnect CinderLink and close Unreal Editor.
2. Revoke or rotate the affected credential at its provider.
3. Review Codex and provider account activity.
4. Preserve relevant local logs without posting them publicly.

## Security promises and non-promises

CinderLink does not intentionally collect telemetry or upload host information. Its source is designed to avoid credential inheritance, inbound listeners, hidden persistence, external agent tools, and permission escalation. It cannot guarantee the behavior of Unreal Engine, the trusted Codex CLI, operating-system components, models, dependencies, or commands operating inside the selected project profile.
