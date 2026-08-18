# Security Policy

## Reporting a vulnerability

Please report suspected security vulnerabilities privately to **hi@stephenlovino.com** with the subject line `BetterCast Security`. Do not open a public GitHub issue for an unpatched vulnerability.

Include the affected release or commit, operating system, connection mode, a concise impact statement, reproduction steps, and any non-destructive proof of concept. Do not include passwords, private keys, personal data, or real user screen content in a report.

## Response targets

The maintainers aim to acknowledge reports within 72 hours, provide periodic status updates, and credit the reporter after a fix is released unless anonymity is requested. These are response targets rather than a guarantee of a particular remediation timeline.

## Scope

Reports relevant to BetterCast include unauthorized peer connection, plaintext or unauthenticated media/control traffic, discovery spoofing, input injection, memory-safety and parser issues, driver or installer execution, update/release integrity, and sensitive information disclosure through logs or issue-reporting URLs.

## Supported versions

Security fixes target the latest release and the default branch. Users should reproduce against the latest release where possible and include the exact version in their report.

## Safe testing

Researchers should test only on devices and networks they own or are authorized to assess. Avoid disruptive denial-of-service testing, kernel-driver installation on production systems, credential collection, or access to other users’ media.
