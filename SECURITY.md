# Security

## Reporting a problem

Please report security problems privately through
[GitHub's private reporting](https://github.com/mihai2mn/robin-webos/security/advisories/new), not as a public issue.
Describe what an attacker could do, on which TV model and Robin version, and how to reproduce it.

Robin is maintained by one person in their spare time. Expect a first answer within about a week. A fix ships in a new
release; the advisory is published once it is out.

## What Robin is and is not

Read this before you rely on it.

- **Robin runs on rooted TVs.** Rooting removes protections webOS normally has, and Robin cannot restore them.
- **The engine is WPE WebKit 2.54.3**, with Robin's own patches. Security fixes in later WebKit versions reach Robin
  only when Robin moves to them. Do not use it for banking or anything you would not do on an outdated browser.
- **The JavaScript JIT runs on the 32-bit engine** through patches WebKit itself no longer ships
  ([details](docs/jit-armv7.md)). That code has had less review than WebKit's supported platforms.
- **Widevine** comes from the TV's own firmware and runs at software security level.

## Supported versions

Only the latest release gets fixes.
