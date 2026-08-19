# BetterCast Windows–Android Security and Ecosystem Handoff

_Last updated: 2026-08-19_  
_Author: Manus AI_  
_Repository: [Lonely-Demon/BetterCast-Fork](https://github.com/Lonely-Demon/BetterCast-Fork)

## Executive conclusion

The fork now contains a substantially hardened Windows–Android dual-mode milestone with **secure transport v2 implemented and compiled successfully in the Windows all-in-one artifact**. Windows and Android negotiate authenticated P-256 ECDH/ECDSA sessions, derive directional AES-256-GCM keys through HKDF-SHA-256, use monotonic record sequence numbers, display a first-pair SAS, pin the approved peer identity, and reject plaintext or unauthenticated application frames. This removes the previous plaintext transport release blocker, but the artifacts remain debug/test builds and still require the black-box installation and first-pair verification described below; no software can honestly guarantee safety against every future implementation or operating-system defect.

The implemented milestone combines Windows as the sender with Android as the receiver for a Windows-to-Android display stream and a separate Phone Control path. Phone Control uses an explicitly user-enabled Android `AccessibilityService`; it does not use wireless ADB escalation. Windows also includes a display suspend/resume hotkey (`Ctrl+Alt+Shift+B`) that sends a session command without intentionally tearing down the connection. The Android receiver now rate-limits control packets, rejects truncated typed frames, and disarms the control service on disconnect and teardown.

## Current source and CI state

| Item | Current state |
|---|---|
| Latest fork `main` | `2d22954911c9fb2cd30b42a7cc83556b2bf186b0` |
| Latest Android secure-v2 validation | Run [`32216613460`](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32216613460), **success**, code commit `48ab4f6b8ac71ca52d7196271b7a4b46902af804` |
| Latest Windows all-in-one secure-v2 validation | Run [`32225528285`](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32225528285), **success**, code commit `2d22954911c9fb2cd30b42a7cc83556b2bf186b0` |
| Windows artifact | GitHub artifact `BetterCast-Windows-Android-All-in-One`, ID `9356084820`, 138,726,417-byte archive; downloaded executable is `artifacts/windows-2d22954/BetterCast.exe` |
| Windows executable SHA-256 | `3d958126cccd12887b0da6083487034e25479617346c61af94ca67cf05e5206b` |
| Android APK downloaded locally | `artifacts/android-48ab4f6/app-debug.apk`, 9,115,415 bytes |
| Android APK SHA-256 | `fca16864585ed21cf2768f5dcc6cb7e7a8c520ae06b5c286bde85349127def1f` |
| Repository regression tests | **13 passing** |
| Linux validation | The post-CMake-fix run was still in progress when this handoff was updated; Windows all-in-one validation is successful |

The Windows all-in-one workflow uses Qt 6.7.3, the Ninja generator, and FFmpeg with OpenH264 rather than the previously failing x264 vcpkg feature. The successful package includes the combined sender/receiver application, Windows software-OpenGL fallback deployment, verified VDD payload, and the SecureSession implementation. The current source revision was built successfully by run `32225528285`.

## Completed security hardening

The transport parsers now reject zero-length, oversized, empty, and unknown-type frames instead of attempting to interpret them. TCP and UDP reassembly are bounded by frame, chunk, in-flight-frame, and in-flight-byte limits. UDP peers are pinned to the admitted address and port, duplicate chunks are handled safely, and expired incomplete frames are reclaimed. The desktop listener enforces a single active TCP peer policy rather than allowing arbitrary concurrent senders.

Remote input validation is implemented on the Swift and Android sides. Event types are allow-listed, numeric values must be finite, and coordinate ranges are bounded. The Android control path additionally bounds JSON payloads to 16 KiB, uses a 240-control-packet-per-connection-second limit, requires the user-enabled AccessibilityService, and exposes only bounded gesture and selected global-action operations. The latest Android transport change rejects a typed packet containing only its type byte and closes the peer rather than falling through to legacy-video parsing.

Wireless ADB auto-escalation has been removed from all relevant paths. Windows sender queues and Android video/control queues are bounded to prevent unbounded memory growth under network backpressure. The Windows installer no longer uses wildcard executable or INF fallbacks, and its firewall rules are scoped to the Private profile. CI downloads are SHA-256 verified or pinned. Dependabot, `SECURITY.md`, and opt-in AddressSanitizer/UBSan build support are present.

Secure transport v2 is now implemented on both the Windows sender and Android receiver, with Windows receiver-side responder integration also compiled in the all-in-one build. The handshake uses ephemeral P-256 ECDH, transcript signatures using persistent identity keys, HKDF-SHA-256 directional key derivation, AES-256-GCM records with 64-bit sequence numbers, constant-time confirmation checks, first-pair SAS approval, and pinned peer identities. UDP is disabled in secure-v2 mode rather than left as an unauthenticated side channel. Plaintext application frames are rejected before decoding, and encrypted outbound control records receive fresh sequence numbers for each retry.

## Completed dual-mode functionality

| Capability | Implemented behavior | Important limitation |
|---|---|---|
| Windows → Android display stream | Existing video sender path is integrated with the Android receiver; Windows can also start a control-only session without video | This is not yet a complete virtual extended monitor with production-grade coordinate mapping |
| Phone Control | Windows pointer capture sends bounded move/tap JSON over TCP; Android displays a remote pointer overlay and dispatches gestures through the user-enabled AccessibilityService | Full keyboard/text injection and automatic screen-edge handoff are not implemented |
| Suspend/resume | `Ctrl+Alt+Shift+B` toggles an explicit display suspend/resume command while retaining the network session; resume requests a keyframe | Android background-activity restrictions mean notification-mediated restoration remains the safe fallback on some versions |
| Disconnect safety | Android disarms the AccessibilityService session on LISTENING, ERROR, IDLE, explicit disconnect, and view-model teardown; Windows destroys per-socket secure-session state on disconnect | First-pair approval is required before an unknown peer becomes trusted |
| Low-spec Windows support | Software OpenGL fallback is deployed; `--software-opengl` and `BETTERCAST_SOFTWARE_OPENGL=1` are available | The Acer Pentium P6200 system may require modest resolution, frame rate, and bitrate settings |

## Regression validation

The repository’s static regression suite now contains thirteen tests covering bounded desktop TCP/UDP parsing, peer pinning, receiver/sender role clarity, Android control authorization and bounds, Android control rate limiting, truncated typed-frame rejection, suspend/resume safety, installer fallback removal, CI download verification, the CNG ECDH call signature, receiver-side secure handshake dispatch, encrypted control output, and explicit SecureSession CMake linkage. The suite completed successfully after the latest source changes:

```text
Ran 13 tests in 0.003s
OK
```

These are source-level regression tests rather than a replacement for black-box testing on the user’s actual Windows 10 laptop and Android device. No live network fuzzing, installer execution, driver installation, APK installation, or reverse engineering of compiled releases was performed in the sandbox.

## Remaining release blockers

### Secure transport v2 verification conditions

The former plaintext transport blocker is resolved in the current source and in the successful Windows all-in-one build. Every TCP application frame is now either a bounded handshake message or an AES-256-GCM record authenticated against a pinned peer identity. Unknown peers remain in a first-pair approval state, UDP is disabled, sequence reuse is rejected, and disconnect cleanup destroys per-connection cryptographic state.

This should not be interpreted as a claim of absolute security. The downloaded packages are debug/test artifacts, the sandbox cannot install them on the user’s Windows 10 laptop or Android phone, and the source-level suite is not a substitute for device-level protocol testing. For public-network use, the user must verify that the SAS is displayed and compared on both devices, approve only the intended peer, confirm that plaintext or mismatched peers are rejected, and keep the peer identity files protected by the operating system’s application-data permissions. A future release should add independent black-box interoperability and negative tests before being labeled production-grade.

### File transfer

File transfer is not yet implemented. The safe design is a separate authenticated channel (`0x04`) with transfer ID, direction, basename-only metadata, declared size, SHA-256 digest, chunk size, resumable offset, cancellation, and bounded rates. Incoming data must be written below a sandbox or user-selected destination through a temporary file, verified, atomically renamed, and never automatically opened or executed.

### Clipboard synchronization

Clipboard synchronization is not yet implemented. A first version should be text-only and opt-in. Windows can use event-driven clipboard notifications through `AddClipboardFormatListener` rather than polling. Each update needs a content hash, source/session identifier, monotonic update ID, and loop-prevention token. Sensitive clipboard content and non-text formats should be disabled by default.

### Audio routing

Audio routing is not yet implemented. Microsoft documents WASAPI loopback as shared-mode capture from a rendering endpoint using `AUDCLNT_STREAMFLAGS_LOOPBACK`; Windows 10 version 1703 and later support event-driven loopback directly, while protected content and endpoint changes remain relevant constraints.[1] Android playback should use a bounded foreground media/audio service and `AudioTrack`-style streaming. Android-to-Windows capture has additional Android permission and version-specific policy requirements.

### True second display

The current Windows display path is a useful stream milestone but does not yet provide the complete behavior of a virtual extended monitor. A production second-display mode needs a Windows virtual-display adapter or equivalent virtual monitor, explicit monitor geometry, resize/reconfiguration handling, and a stable mapping from Android touch coordinates back to Windows virtual-screen coordinates, including negative coordinates and DPI scaling.

### Phone mirroring

Phone mirroring is not implemented and remains lower priority. Android’s official MediaProjection path requires explicit user consent for each projection session, a foreground service declaration on Android 14 and later, and a single-use projection token for each `createVirtualDisplay()` operation.[2] Windows must not silently start mirroring from a remote control command.

## Research-backed architecture decisions

The roadmap now treats the product as multiple capability planes over one authenticated Wi-Fi session rather than one undifferentiated protocol. Existing type-byte values remain `0x01` for video, `0x02` for audio, and `0x03` for control. Proposed future values are `0x04` for file-transfer control/data and `0x05` for clipboard updates, but they must be rejected until pairing and explicit capability grants exist.

Microsoft’s WASAPI documentation states that loopback capture uses a render endpoint in shared mode and the `AUDCLNT_STREAMFLAGS_LOOPBACK` flag.[1] Android’s MediaProjection documentation identifies `createVirtualDisplay()` as the central virtual-display capture method and requires user consent and foreground-service handling for modern Android releases.[2] Android’s app-specific storage guidance supports staging transfer data in private app storage without broad storage permission, while durable user-owned files should use shared storage or media collections.[3] Microsoft’s clipboard documentation supports event-driven notification through `AddClipboardFormatListener`.[4]

> “The current secure transport is a release milestone, not a proof of perfect security. New capabilities must remain behind authenticated session records, explicit capability grants, bounded resource use, and revocation.”

## Black-box verification procedure for the user

The user should use the Android APK and Windows all-in-one artifact from the successful GitHub Actions runs. On Android, the user will need to start Receiver mode and, only if Phone Control is desired, enable the disclosed BetterCast AccessibilityService once in Android Settings. On Windows, the user should start the sender-enabled application, enter or select the Android receiver’s Wi-Fi address, and choose either Second Display or Phone Control. The legacy Acer hardware should first be tested with the software OpenGL fallback enabled if the normal launch still produces a blank or white window.

The first verification should confirm that Android receives the Windows display stream. On the first connection, both devices should show the same short authentication string; the user should compare the strings out-of-band and approve the peer on both devices before any media or control flow is accepted. A later connection should use the pinned identity without silently accepting a changed peer. The second verification should arm Phone Control, verify that the pointer overlay appears, test a tap and bounded movement, and press Escape to stop control. The third should press `Ctrl+Alt+Shift+B`, confirm that the display presentation suspends without an intentional session teardown, press it again, and confirm that restoration is attempted. If the SAS is absent, mismatched, or a plaintext/legacy peer is accepted, the user should stop and report the exact logs rather than continuing on a public network.

## Recommended next engineering sequence

The next implementation milestone is capability negotiation and revocation over the completed authenticated session. File transfer and text clipboard synchronization should then be implemented over separate bounded channels with static protocol tests and black-box CI packaging. Audio should follow as an independent bounded media stream. True virtual second-display geometry and touch mapping should be implemented after the session and file/clipboard foundations are stable. Phone mirroring should remain last because its consent and foreground-service lifecycle are more restrictive.

## References

[1]: https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording "Loopback Recording - Win32 apps | Microsoft Learn"
[2]: https://developer.android.com/media/grow/media-projection "Media projection | Android Developers"
[3]: https://developer.android.com/training/data-storage/app-specific "Access app-specific files | Android Developers"
[4]: https://learn.microsoft.com/en-us/windows/win32/dataxchg/using-the-clipboard "Using the Clipboard - Win32 apps | Microsoft Learn"
[5]: https://developer.android.com/reference/android/media/AudioTrack "AudioTrack | Android Developers"
