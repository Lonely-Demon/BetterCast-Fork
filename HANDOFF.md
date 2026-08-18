# BetterCast Security Remediation and Test Handoff

**Repository:** [Lonely-Demon/BetterCast-Fork](https://github.com/Lonely-Demon/BetterCast-Fork)

**Final remediation branch:** `main`

**Final source commit:** `8269455` (`test: cover pinned Linux ADB checksum`)

## Result

The security-hardening changes and the Windows legacy-OpenGL remediation were implemented directly in the fork. The Windows portable workflow completed successfully on commit `477b66f`, run [32153941257](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32153941257). The Linux workflow was repaired by pinning and verifying the current Google platform-tools digest; run [32147238561](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32147238561) completed successfully.

The standard-library regression suite contains five repository hardening tests and passes with Python `unittest`.

## Windows artifact

Download the artifact from the successful [Windows portable workflow run](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32153941257), then download `BetterCastReceiver-Windows-portable` and extract the ZIP. The package was independently inspected without executing it and contains `BetterCastReceiver.exe`, Qt/FFmpeg runtime DLLs, and `opengl32sw.dll` (Qt software OpenGL fallback). It also contains `START_WITH_SOFTWARE_OPENGL.txt`.

Verified local metadata for the downloaded artifact:

| File | SHA-256 |
|---|---|
| `BetterCastReceiver.exe` | `e0f43d3d904287a38a539a8b1a750dd7585dad5f4d88268f9c8ee2afd466500a` |
| `opengl32sw.dll` | `b04de4541863bc7d8879040a78889c4849c1b1da2784c4630f734c146c2998ce` |
| `START_WITH_SOFTWARE_OPENGL.txt` | `13ccf65627392a0b583de6ae5b8e4d074f7b1fb68f77ba85d22876ea989adc10` |

## Black-box test on the Acer Aspire 4739Z

Extract the ZIP to a normal folder. First double-click `BetterCastReceiver.exe` and check whether the dark BetterCast interface, sidebar, overview page, and controls are visible.

If the window remains white or blank, open **Command Prompt** in the extracted folder and run:

```text
BetterCastReceiver.exe --software-opengl
```

The same fallback can be requested with:

```text
set BETTERCAST_SOFTWARE_OPENGL=1
BetterCastReceiver.exe
```

If the software-rendered UI appears, the original problem was the legacy Intel graphics path. Please report only whether the normal launch and the `--software-opengl` launch each show the interface; no development tools or additional testing are required.

## Implemented hardening

The fork now rejects malformed or oversized TCP frames, bounds UDP chunk/frame/byte reassembly, expires incomplete frames, pins UDP peers, limits active sessions, validates Swift and Android input events, removes automatic wireless ADB escalation, bounds sender queues, removes unsafe Windows installer wildcard execution, scopes firewall rules to Private/local-subnet use, verifies CI downloads, adds Dependabot and security policy coverage, and supports sanitizer/strict-warning builds.

The Windows white-window fix applies software OpenGL before `QApplication` construction, retains `opengl32sw.dll` in deployment, detects OpenGL context/shader/VBO failures, and exposes a visible graphics-unavailable status instead of silently painting a blank surface.

## Remaining release blockers

The transport is still plaintext and unauthenticated across the cross-platform protocol. The hardening reduces malformed-input and resource-exhaustion risk but does not prevent a same-LAN attacker from impersonating a peer, reading media, or injecting control events. Cryptographic pairing, authenticated TCP/TLS or Noise sessions, capability negotiation, authenticated UDP datagrams, replay protection, and signed project-owned Windows binaries remain required before treating BetterCast as safe on shared networks. Use the rebuilt artifact only on a trusted network and do not install the privileged VDD path blindly.

See [`SECURITY_REMEDIATION.md`](SECURITY_REMEDIATION.md) and [`../bettercast_security_assessment.md`](../bettercast_security_assessment.md) for the detailed assessment and implementation record.
