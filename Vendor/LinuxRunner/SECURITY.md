# Security Policy

## Security Model

LinuxRunner runs Linux programs inside one iOS/iPadOS application. **The platform sandbox is the security boundary.** Linux accounts, permissions and guest memory mappings do not isolate hostile programs from the app or from one another.

Shared folders intentionally grant selected file access; Linux programs can use the device's network through the compatibility layer. The experimental graphics bridge accepts guest rendering requests. Do not treat environments as secure containers for untrusted software or sensitive secrets.

Distribution installers use signature-checked package data and TLS. Keep those checks enabled. Package repair invokes real package tools and maintainer scripts; metadata checkpoints do not provide complete rollback.

## Report a Vulnerability Privately

Use [GitHub's private vulnerability report form](https://github.com/mxhm11cr/LinuxRunner/security/advisories/new). Private reporting was enabled for the public repository; if the form is unavailable, do not post exploit details, credentials or private data in an ordinary issue.

Include app version/build, source/artifact identity when known, device/OS, distribution and a minimal reproduction. No response deadline is promised. Ordinary crashes and compatibility problems can be reported through [Issues](https://github.com/mxhm11cr/LinuxRunner/issues).

## Diagnostic Privacy

Reports apply bounded logging and redaction, but exported filenames, application output and screenshots may still contain personal information. Review files before sharing. Never publish passwords, signing material, tokens or personal messages.

## Upstream

LinuxRunner derives from [iSH](https://github.com/ish-app/ish) through [ios-linuxkit](https://github.com/rcarmo/ios-linuxkit). Their upstream security policies and retained source notices continue to apply to their components.
