# BetterCast Windows–Android Secure Transport v2 Validation Manifest

_Last updated: 2026-08-19_  
_Repository: [Lonely-Demon/BetterCast-Fork](https://github.com/Lonely-Demon/BetterCast-Fork)_

## Validated source state

The fork’s `main` branch contains the secure transport implementation and handoff documentation at commit [`ac30928`](https://github.com/Lonely-Demon/BetterCast-Fork/commit/ac30928). The last code-bearing commit is [`2d22954`](https://github.com/Lonely-Demon/BetterCast-Fork/commit/2d22954911c9fb2cd30b42a7cc83556b2bf186b0); the later commits update only the project handoff records and validation metadata.

Secure transport v2 is implemented for the Windows sender and Android receiver, with Windows responder-side integration also compiled into the all-in-one target. The protocol uses ephemeral P-256 ECDH, persistent identity-key transcript signatures, HKDF-SHA-256, directional AES-256-GCM record keys, 64-bit sequence numbers, constant-time confirmation comparison, first-pair SAS approval, and pinned peer identities. Plaintext application frames are rejected, and UDP is disabled in secure-v2 mode rather than remaining as an unauthenticated side channel.

## Automated validation

| Validation | Result | Evidence |
|---|---|---|
| Repository regression suite | **13 tests passed** | `python3 -m unittest discover -s tests -v` |
| Windows all-in-one sender/receiver | **Success** | [GitHub Actions run 32225528285](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32225528285) at code commit `2d22954911c9fb2cd30b42a7cc83556b2bf186b0` |
| Windows portable receiver | **Success** | [GitHub Actions run 32225528264](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32225528264) |
| Android secure-v2 debug APK | **Success** | [GitHub Actions run 32216613460](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32216613460) at commit `48ab4f6b8ac71ca52d7196271b7a4b46902af804` |
| Linux receiver | **Not a release gate** | Post-CMake-fix run [32225528306](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32225528306) was cancelled after stalling in system-dependency installation; Linux is deprioritized by the project requirements |

The CI evidence proves compilation and packaging on the target Windows workflow and compilation of the Android debug artifact. It does not replace device-level black-box interoperability testing on the user’s laptop and phone.

## Downloaded artifacts and hashes

| Artifact | Local path | SHA-256 |
|---|---|---|
| Self-contained Windows all-in-one package | `artifacts/BetterCast-Windows-Android-All-in-One-2d22954.zip` | `c8fa52408a4901fb71c78645a21aeea46b5317428002f022abf6064c661dca0b` |
| Windows executable inside package | `artifacts/windows-2d22954/BetterCast.exe` | `3d958126cccd12887b0da6083487034e25479617346c61af94ca67cf05e5206b` |
| Android secure-v2 debug APK | `artifacts/android-48ab4f6/app-debug.apk` | `fca16864585ed21cf2768f5dcc6cb7e7a8c520ae06b5c286bde85349127def1f` |

The Windows ZIP contains the executable, Qt and FFmpeg runtime libraries, software OpenGL fallback (`opengl32sw.dll`), OpenH264, the verified VDD payload, and the bundled Visual C++ redistributable. The Android package is a debug APK and may require the normal Android installation confirmation for an APK obtained outside the Play Store.

## Black-box verification sequence

Install the Android APK and extract the complete Windows ZIP into one directory; do not copy only `BetterCast.exe`, because the runtime DLLs and software OpenGL fallback are required. Start the Android receiver and start the Windows sender-enabled application on the same Wi-Fi network. On the first connection, compare the short authentication string displayed by both devices through an independent channel and approve only if the values match. The connection must not proceed to media or control before approval.

After approval, test the Windows-to-Android display stream, then separately enable the BetterCast AccessibilityService in Android Settings if Phone Control is required. Test bounded pointer movement and a tap, stop with Escape, and verify that the Android session is disarmed after disconnect. Test `Ctrl+Alt+Shift+B` twice to verify display suspend/resume without an intentional Wi-Fi session teardown.

Do not continue on a public network if the SAS is missing or mismatched, if a legacy/plaintext peer is accepted, or if a changed peer identity is silently trusted. These artifacts have passed automated build and source-level checks but have not been installed or exercised on the user’s physical devices by the sandbox.

## Remaining product scope

File transfer, clipboard synchronization, audio routing, full keyboard/text mapping, production-grade virtual second-display geometry, and optional phone mirroring remain future milestones. Capability negotiation and explicit revocation should be added before those features are exposed over the authenticated session.

For the full security analysis and ecosystem roadmap, see [`BETTERCAST_SECURITY_AND_ECOSYSTEM_HANDOFF.md`](BETTERCAST_SECURITY_AND_ECOSYSTEM_HANDOFF.md). For the autonomous assumptions and work ledger, see [`AUTONOMOUS_ASSUMPTIONS_AND_PROGRESS.md`](AUTONOMOUS_ASSUMPTIONS_AND_PROGRESS.md).
