# BetterCast Windows–Android Secure Transport v2 Validation Manifest

_Last updated: 2026-08-19_  
_Repository: [Lonely-Demon/BetterCast-Fork](https://github.com/Lonely-Demon/BetterCast-Fork)_

## Validated source state

The fork’s `main` branch contains the secure transport, Android portrait UI, Windows discovery, manual endpoint, firewall, and VDD installation fixes at commit [`937ddf2`](https://github.com/Lonely-Demon/BetterCast-Fork/commit/937ddf2e324daaaf5ae351b73b5ef46fb25db78f). The Android-only UI fix is [`f7fed73`](https://github.com/Lonely-Demon/BetterCast-Fork/commit/f7fed7374ed2ff447a7cb3b0dc3a06f128e83c29).

Secure transport v2 is implemented for the Windows sender and Android receiver, with Windows responder-side integration also compiled into the all-in-one target. The protocol uses ephemeral P-256 ECDH, persistent identity-key transcript signatures, HKDF-SHA-256, directional AES-256-GCM record keys, 64-bit sequence numbers, constant-time confirmation comparison, first-pair SAS approval, and pinned peer identities. Plaintext application frames are rejected, and UDP is disabled in secure-v2 mode rather than remaining as an unauthenticated side channel.

## Automated validation

| Validation | Result | Evidence |
|---|---|---|
| Repository regression suite | **15 tests passed** | `python3 -m unittest discover -s tests -v` |
| Windows all-in-one sender/receiver | **Success** | [GitHub Actions run 32285401293](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32285401293) at code commit `937ddf2e324daaaf5ae351b73b5ef46fb25db78f` |
| Windows portable receiver | **Success** | [GitHub Actions run 32285401279](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32285401279) |
| Android secure-v2 and portrait UI debug APK | **Success** | [GitHub Actions run 32258871282](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32258871282) at commit `f7fed7374ed2ff447a7cb3b0dc3a06f128e83c29` |
| Linux receiver | **Not a release gate** | Post-CMake-fix run [32225528306](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32225528306) was cancelled after stalling in system-dependency installation; Linux is deprioritized by the project requirements |

The CI evidence proves compilation and packaging on the target Windows workflow and compilation of the Android debug artifact. It does not replace device-level black-box interoperability testing on the user’s laptop and phone.

## Downloaded artifacts and hashes

| Artifact | Local path | SHA-256 |
|---|---|---|
| Self-contained corrected Windows all-in-one package | `artifacts/BetterCast-Windows-Android-All-in-One-937ddf2.zip` | `8ff1cd958a438d02f29ed4ec66f2dedac5def497af9b32836a238c9938df32f0` |
| Corrected Windows executable inside package | `artifacts/windows-937ddf2/BetterCast.exe` | `08f91d0f3ea36a4caaaf6f27dfcc0c0ac75c8c9f154076a00d46903670e52666` |
| Android secure-v2 and portrait UI debug APK | `artifacts/android-f7fed73/app-debug.apk` | `7630a256f4c85685b0501b79370b6ff33f91b9c26c5c4267c025284e366326d6` |

The Windows ZIP contains the executable, Qt and FFmpeg runtime libraries, software OpenGL fallback (`opengl32sw.dll`), OpenH264, the verified VDD payload, and the bundled Visual C++ redistributable. The Android package is a debug APK and may require the normal Android installation confirmation for an APK obtained outside the Play Store.

## Black-box verification sequence

Install the corrected Android APK and extract the complete corrected Windows ZIP into one directory; do not copy only `BetterCast.exe`, because the runtime DLLs and software OpenGL fallback are required. The corrected APK keeps the receive/setup screen in portrait and makes its extended setup content scrollable. Start the Android receiver and start the Windows sender-enabled application on the same Wi-Fi network.

For manual sender connection, enter either `192.168.29.14` or `192.168.29.14:51820`; the corrected Windows build now parses the optional port instead of passing the entire string to DNS. If mDNS remains unavailable on the Wi-Fi, manual entry is the supported fallback. On the first connection, compare the short authentication string displayed by both devices through an independent channel and approve only if the values match. The connection must not proceed to media or control before approval.

After approval, test the Windows-to-Android display stream, then separately enable the BetterCast AccessibilityService in Android Settings if Phone Control is required. Test bounded pointer movement and a tap, stop with Escape, and verify that the Android session is disarmed after disconnect. Test `Ctrl+Alt+Shift+B` twice to verify display suspend/resume without an intentional Wi-Fi session teardown. For virtual display installation, approve the UAC prompt and allow the verified `devcon.exe`/PnPUtil operations; the corrected build waits for delayed Windows Plug and Play visibility and retries device creation after PnPUtil rather than treating exit code 259 as success.

Do not continue on a public network if the SAS is missing or mismatched, if a legacy/plaintext peer is accepted, or if a changed peer identity is silently trusted. These artifacts have passed automated build and source-level checks but have not been installed or exercised on the user’s physical devices by the sandbox.

## Remaining product scope

File transfer, clipboard synchronization, audio routing, full keyboard/text mapping, production-grade virtual second-display geometry, and optional phone mirroring remain future milestones. Capability negotiation and explicit revocation should be added before those features are exposed over the authenticated session.

For the full security analysis and ecosystem roadmap, see [`BETTERCAST_SECURITY_AND_ECOSYSTEM_HANDOFF.md`](BETTERCAST_SECURITY_AND_ECOSYSTEM_HANDOFF.md). For the autonomous assumptions and work ledger, see [`AUTONOMOUS_ASSUMPTIONS_AND_PROGRESS.md`](AUTONOMOUS_ASSUMPTIONS_AND_PROGRESS.md).
