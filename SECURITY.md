# Security Policy

LinuxAPO (SkyAPO) is currently a **beta** project. See [README.md](README.md) and [CHANGELOG.md](CHANGELOG.md) for the current release and limitations.

## Supported versions

There is no long-term security support commitment or guaranteed response timeframe at this stage. The latest published beta is the primary version to evaluate; older builds may not receive fixes.

## Reporting a vulnerability

Please **do not open a public issue** containing exploit details, credentials, or other sensitive information.

If GitHub's **Report a vulnerability** / private vulnerability reporting feature is available under the repository's Security tab, use that. Otherwise, contact the maintainers through an established private channel and ask for a secure reporting route before sharing sensitive details.

Include, when safe:
- Affected version or commit.
- Relevant build/runtime environment.
- A clear impact description.
- Minimal reproduction steps or logs with secrets removed.

Maintainers will review reports as capacity permits. Do not assume a fix or disclosure date until one is explicitly agreed.

## Plugin safety

LinuxAPO hosts third-party audio plugins **in-process**. Untrusted plugins may crash or stall the daemon. Only load plugins you trust, and do not treat plugin loading as a security sandbox.

For ordinary bugs and support requests, see [SUPPORT.md](SUPPORT.md).
