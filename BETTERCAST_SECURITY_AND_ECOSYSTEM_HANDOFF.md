# BetterCast Windows–Android Security and Ecosystem Handoff

_Last updated: 2026-08-19_  
_Author: Manus AI_  
_Repository: [Lonely-Demon/BetterCast-Fork](https://github.com/Lonely-Demon/BetterCast-Fork)

## Executive conclusion

The fork now contains a substantially hardened Windows–Android dual-mode milestone, but it is **not yet safe for use on an untrusted Wi-Fi network** because the transport remains plaintext and unauthenticated. The current build is appropriate for controlled, trusted local-network evaluation only. The most important remaining release blocker is authenticated pairing followed by encrypted, integrity-protected transport with replay protection and capability grants.

The implemented milestone combines Windows as the sender with Android as the receiver for a Windows-to-Android display stream and a separate Phone Control path. Phone Control uses an explicitly user-enabled Android `AccessibilityService`; it does not use wireless ADB escalation. Windows also includes a display suspend/resume hotkey (`Ctrl+Alt+Shift+B`) that sends a session command without intentionally tearing down the connection. The Android receiver now rate-limits control packets, rejects truncated typed frames, and disarms the control service on disconnect and teardown.

## Current source and CI state

| Item | Current state |
|---|---|
| Latest fork `main` | `d1c98985c42ca205c6efe40bf86798f9563079a5` |
| Latest Android code validation | Run [`32189181540`](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32189181540), **success**, code commit `50fe4d8ebde372099a7059c0c70952938ed34e9f` |
| Latest Windows code validation | Run [`32189202887`](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32189202887), **success**, code commit `50fe4d8ebde372099a7059c0c70952938ed34e9f` |
| Last successful Windows all-in-one build | Run [`32189202887`](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32189202887), **success** |
| Previous successful combined build | Run [`32181810245`](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32181810245), **success** |
| Repository regression tests | **9 passing** |
| Android APK downloaded locally | `artifacts/android/app-debug.apk`, 9,099,795 bytes |
| Android APK SHA-256 | `cd3a2faca72819341834933bb8ecc397ac6d71898bb4cb56a45b0f2642c08c8c` |
| Windows artifact metadata | Current successful package is 65,785,020 bytes; GitHub artifact ID `9344394467`; direct download requires the authenticated GitHub Actions artifact endpoint |

The Windows all-in-one workflow uses Qt 6.7.3, the Ninja generator, and FFmpeg with OpenH264 rather than the previously failing x264 vcpkg feature. The successful package includes the combined sender/receiver application and Windows software-OpenGL fallback deployment. The current source revision was built successfully by run `32189202887`.

## Completed security hardening

The transport parsers now reject zero-length, oversized, empty, and unknown-type frames instead of attempting to interpret them. TCP and UDP reassembly are bounded by frame, chunk, in-flight-frame, and in-flight-byte limits. UDP peers are pinned to the admitted address and port, duplicate chunks are handled safely, and expired incomplete frames are reclaimed. The desktop listener enforces a single active TCP peer policy rather than allowing arbitrary concurrent senders.

Remote input validation is implemented on the Swift and Android sides. Event types are allow-listed, numeric values must be finite, and coordinate ranges are bounded. The Android control path additionally bounds JSON payloads to 16 KiB, uses a 240-control-packet-per-connection-second limit, requires the user-enabled AccessibilityService, and exposes only bounded gesture and selected global-action operations. The latest Android transport change rejects a typed packet containing only its type byte and closes the peer rather than falling through to legacy-video parsing.

Wireless ADB auto-escalation has been removed from all relevant paths. Windows sender queues and Android video/control queues are bounded to prevent unbounded memory growth under network backpressure. The Windows installer no longer uses wildcard executable or INF fallbacks, and its firewall rules are scoped to the Private profile. CI downloads are SHA-256 verified or pinned. Dependabot, `SECURITY.md`, and opt-in AddressSanitizer/UBSan build support are present.

## Completed dual-mode functionality

| Capability | Implemented behavior | Important limitation |
|---|---|---|
| Windows → Android display stream | Existing video sender path is integrated with the Android receiver; Windows can also start a control-only session without video | This is not yet a complete virtual extended monitor with production-grade coordinate mapping |
| Phone Control | Windows pointer capture sends bounded move/tap JSON over TCP; Android displays a remote pointer overlay and dispatches gestures through the user-enabled AccessibilityService | Full keyboard/text injection and automatic screen-edge handoff are not implemented |
| Suspend/resume | `Ctrl+Alt+Shift+B` toggles an explicit display suspend/resume command while retaining the network session; resume requests a keyframe | Android background-activity restrictions mean notification-mediated restoration remains the safe fallback on some versions |
| Disconnect safety | Android disarms the AccessibilityService session on LISTENING, ERROR, IDLE, explicit disconnect, and view-model teardown | Cryptographic peer authentication is still absent |
| Low-spec Windows support | Software OpenGL fallback is deployed; `--software-opengl` and `BETTERCAST_SOFTWARE_OPENGL=1` are available | The Acer Pentium P6200 system may require modest resolution, frame rate, and bitrate settings |

## Regression validation

The repository’s static regression suite now contains nine tests covering bounded desktop TCP/UDP parsing, peer pinning, receiver/sender role clarity, Android control authorization and bounds, Android control rate limiting, truncated typed-frame rejection, suspend/resume safety, installer fallback removal, and CI download verification. The suite completed successfully after the latest source changes:

```text
Ran 9 tests in 0.002s
OK
```

These are source-level regression tests rather than a replacement for black-box testing on the user’s actual Windows 10 laptop and Android device. No live network fuzzing, installer execution, driver installation, APK installation, or reverse engineering of compiled releases was performed in the sandbox.

## Remaining release blockers

### Plaintext and unauthenticated transport

The current TCP and UDP channels still use plaintext and do not prove peer identity. Any hostile device able to reach the listening port on the same network may attempt to connect, send malformed data, issue control commands, or exploit the absence of authorization. This is the primary reason the application should not be described as production-safe on shared or untrusted Wi-Fi.

Before file transfer, clipboard, audio, or remote-control release, the protocol needs a mutually authenticated pairing flow, a short-lived session key, authenticated encryption for every channel, replay protection, capability negotiation, revocation, and explicit disconnect invalidation. Discovery must not itself grant control.

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

> “The current plaintext transport is a release blocker. Implementing files or clipboard before authenticated pairing would turn a local-network injection flaw into a file-write or data-exfiltration capability.”

## Black-box verification procedure for the user

The user should use the Android APK and Windows all-in-one artifact from the successful GitHub Actions runs. On Android, the user will need to start Receiver mode and, only if Phone Control is desired, enable the disclosed BetterCast AccessibilityService once in Android Settings. On Windows, the user should start the sender-enabled application, enter or select the Android receiver’s Wi-Fi address, and choose either Second Display or Phone Control. The legacy Acer hardware should first be tested with the software OpenGL fallback enabled if the normal launch still produces a blank or white window.

The first verification should confirm that Android receives the Windows display stream. The second should arm Phone Control, verify that the pointer overlay appears, test a tap and bounded movement, and press Escape to stop control. The third should press `Ctrl+Alt+Shift+B`, confirm that the display presentation suspends without an intentional session teardown, press it again, and confirm that restoration is attempted. The user should not test this prototype on a public or shared Wi-Fi network until authenticated encryption and pairing are implemented.

## Recommended next engineering sequence

The next implementation milestone should be authenticated pairing and encrypted session framing, followed by capability negotiation and revocation. File transfer and text clipboard synchronization should then be implemented over separate bounded channels with static protocol tests and black-box CI packaging. Audio should follow as an independent bounded media stream. True virtual second-display geometry and touch mapping should be implemented after the session and file/clipboard foundations are stable. Phone mirroring should remain last because its consent and foreground-service lifecycle are more restrictive.

## References

[1]: https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording "Loopback Recording - Win32 apps | Microsoft Learn"
[2]: https://developer.android.com/media/grow/media-projection "Media projection | Android Developers"
[3]: https://developer.android.com/training/data-storage/app-specific "Access app-specific files | Android Developers"
[4]: https://learn.microsoft.com/en-us/windows/win32/dataxchg/using-the-clipboard "Using the Clipboard - Win32 apps | Microsoft Learn"
[5]: https://developer.android.com/reference/android/media/AudioTrack "AudioTrack | Android Developers"
