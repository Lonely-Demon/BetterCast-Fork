# BetterCast Windows–Android Ecosystem Roadmap

## Product vision

BetterCast should become a local-first Windows–Android companion rather than a single screen-casting utility. The intended experience combines the useful parts of a second monitor, universal input control, nearby file exchange, clipboard sharing, media/audio routing, and optional phone mirroring while keeping the phone and laptop under the user’s direct control.

The architecture should not pretend that these capabilities are one protocol. They are separate capability planes negotiated over one authenticated Wi-Fi session.

| Plane | Purpose | Primary direction | Required authorization |
|---|---|---|---|
| Session and pairing | Discovery, identity, encryption, capability negotiation, reconnect | Bidirectional | Explicit user pairing and revocation |
| Second Display | Render a Windows physical or virtual monitor on Android | Windows → Android | Android receive session; Windows virtual-display permission/driver if true extension is used |
| Phone Control | Control the native Android UI with Windows input | Windows → Android | Android AccessibilityService; keyboard/text support may require a separate user-enabled input method or other Android-supported path |
| Suspend/resume | Temporarily hide the second-display presentation without destroying the session | Bidirectional command | Persistent Android session notification; notification action is the guaranteed restore fallback |
| File transfer | Drag/drop and browse files in both directions | Bidirectional | Explicit per-transfer approval, path sandboxing, size/rate limits |
| Clipboard | Share text and selected safe clipboard formats | Bidirectional | Explicit opt-in, sensitive-content warning, loop prevention |
| Audio | Windows playback capture to Android and Android microphone/device audio to Windows | Bidirectional | Per-device audio consent and explicit mute controls; Android version-dependent capture permissions |
| Phone Mirror | View Android’s native screen on Windows when the phone is away | Android → Windows | MediaProjection consent and foreground service; lower priority |

## Dependency order

### Foundation: authenticated local session

The current transport is still plaintext and must not be treated as production-safe for remote control or file transfer. The first architectural prerequisite is authenticated pairing with a short-lived session key, encrypted TCP/UDP payloads, replay protection, a capability allow-list, per-channel rate limits, and revocation. Discovery must never itself grant control.

### Milestone 1: Windows sender to Android receiver

The existing video sender and Android receiver are the shortest path to a useful Windows-to-Android stream. The receiver-only and sender-enabled workflows must remain separate so that low-spec Windows users do not install unnecessary drivers. For the user’s legacy Intel laptop, OpenH264 or a native Windows Media Foundation encoder should remain available as a CPU/software fallback when hardware encoding is unavailable.

### Milestone 2: Phone Control mode

Phone Control must keep the native Android UI visible and use a separate control plane. The Android AccessibilityService is the broad, user-authorized gesture path for taps, swipes, scrolls, and approved global actions. Windows should expose an explicit connection and arm action, a visible tray/session indicator, a safe Escape stop, and eventually a configured screen-edge handoff.

The first implementation should not claim full keyboard equivalence. Navigation and text input require an additional Android-supported mechanism and should be negotiated as separate capabilities. A control message must never contain arbitrary shell commands or unbounded key codes.

### Milestone 3: True Second Display mode

A true extended desktop needs a Windows virtual display adapter. BetterCast’s existing virtual-display integration should create or select a virtual monitor, capture that monitor, and stream it to the Android surface. Android touch coordinates can then map back to Windows monitor coordinates. This is distinct from mirroring the phone’s native screen and distinct from Phone Control mode.

### Milestone 4: Suspend/resume

The session should model `ACTIVE`, `SUSPENDED`, `RECONNECTING`, and `STOPPED` states. The Windows hotkey toggles a session command rather than closing the virtual display or tearing down the network session. Android stops rendering while a foreground service preserves the connection and presents a persistent notification. Because Android restricts background activity launches, the second hotkey can request restoration, but the notification action remains the guaranteed user-mediated fallback.

### Milestone 5: Files and clipboard

File transfer should use a separate framed channel with resumable chunks, SHA-256 verification, temporary-file writes followed by atomic rename, collision-safe names, user-selected download directories, and per-transfer cancellation. Drag/drop is a Windows UI feature layered over this channel, not a reason to grant the Android service broad storage access.

Clipboard synchronization should begin with plain text. Each clipboard update needs a source identifier, content hash, timestamp, and loop-prevention token. Sensitive clipboard categories should be opt-in and clearly disclosed.

### Milestone 6: Audio

Windows-to-Android audio should use WASAPI loopback capture with a bounded jitter buffer and an Android foreground media/audio playback service. Android-to-Windows audio requires Android device-capture permissions and version-specific MediaProjection/audio policies. Audio must remain an independent stream with mute, device selection, backpressure, and recovery when endpoints change.

### Milestone 7: Optional Phone Mirror

Phone Mirror is the lower-priority Android-to-Windows direction. Android MediaProjection requires explicit user consent and a foreground service. It should reuse the authenticated video transport but not be conflated with Phone Control. The product should let the user choose “view phone” without implying that the Windows pointer controls the Android OS unless Phone Control is separately armed.

## Protocol sequencing for the next implementation phase

The existing type-byte framing should remain backward-compatible for video (`0x01`), audio (`0x02`), and control (`0x03`), but new capability planes must not be added as unauthenticated payloads. After pairing, each session should negotiate a protocol version and capability bitset. The proposed future types are `0x04` for file-transfer control/data and `0x05` for clipboard updates; both must be rejected until the peer has an authenticated session and an explicit capability grant.

File-transfer metadata should include a transfer identifier, direction, basename only, declared size, SHA-256 digest, chunk size, and resumable offset. Payloads must be written to a temporary file beneath a user-selected sandbox, verified before atomic rename, and never auto-opened or executed. Clipboard messages should initially carry UTF-8 text only, with a content hash, source peer/session identifier, monotonic update identifier, and loop-prevention token. The receiver should require opt-in and reject oversized or stale updates.

Audio must remain an independent media stream with its own queue and mute state. Windows capture should use shared-mode WASAPI loopback, while Android playback should use a foreground media service and bounded `AudioTrack` buffers. Optional mirroring should use a separately consented Android MediaProjection session; it must not be silently started by a Windows control command.

These designs are intentionally documented before implementation because the current plaintext transport is a release blocker. Implementing files or clipboard before authenticated pairing would turn a local-network injection flaw into a file-write or data-exfiltration capability.

## Security gates

| Gate | Required before release |
|---|---|
| Pairing | Mutual peer identity confirmation, short-lived pairing code or equivalent, revocation, and no trust based solely on IP discovery |
| Transport | Encryption and integrity for every control, file, clipboard, and audio channel; replay protection for control commands |
| Authorization | Separate capability grants for display, control, files, clipboard, audio, and mirroring |
| Control safety | Bounded coordinates and rates, allow-listed actions, emergency stop, disconnect fail-safe, and visible active-state indicator |
| File safety | Sandboxed paths, atomic writes, size limits, cancellation, hashes, and no automatic execution |
| Privacy | Explicit disclosures for AccessibilityService, MediaProjection, audio capture, clipboard, and notification persistence |
| Recovery | Safe behavior on Wi-Fi changes, sleep/wake, peer disappearance, Android process death, and Windows display reconfiguration |

## Performance targets for the legacy laptop

The Acer Aspire 4739Z-class machine should default to a modest resolution, 30 FPS, conservative bitrate, bounded queue sizes, and software OpenGL rendering where needed. The app should avoid keeping multiple decoded frames or unbounded file/audio buffers in memory. Second Display and Phone Control should be independently switchable so a low-spec system can use control and file features without continuously encoding video.

## References

[1]: https://github.com/Genymobile/scrcpy "scrcpy official repository"
[2]: https://kdeconnect.kde.org/ "KDE Connect official site"
[3]: https://developer.android.com/guide/topics/ui/accessibility/service "Android AccessibilityService guide"
[4]: https://developer.android.com/media/grow/media-projection "Android MediaProjection documentation"
[5]: https://developer.android.com/guide/components/activities/background-starts "Android background activity launch restrictions"
[6]: https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording "Microsoft WASAPI loopback recording"
[7]: https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder "Microsoft Media Foundation H.264 encoder"
[8]: https://superdisplay.app/ "SuperDisplay official site"
