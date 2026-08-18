# Windows–Android Second-Monitor Design

## Product contract

The first supported direction is **Windows sender → Android receiver**. Windows owns the desktop and the cursor. Android renders a streamed Windows display surface and sends touch/mouse gestures back as pointer events addressed to the Windows display coordinate space. Android is not treated as a remote-controlled phone in this mode, and BetterCast will not attempt to inject input into Android’s operating system.

The long-term product is a Windows–Android companion suite with four separate planes:

| Plane | First milestone | Later capability |
|---|---|---|
| Display media | Windows captures a selected physical or virtual monitor; Android decodes and renders H.264 | Adaptive bitrate, rotation, multiple displays, pause/resume |
| Input | Android touch sends normalized pointer events back to Windows; Windows maps them to the selected display | Cursor-boundary handoff, stylus pressure, keyboard focus, gestures |
| Audio | Explicitly deferred from the first milestone | Windows WASAPI loopback to Android and Android microphone/playback return channel |
| Companion data | Explicitly deferred from the first milestone | Authenticated bidirectional files, clipboard, drag/drop, notifications |

## Transport

The existing TCP media framing is retained for the initial interoperability milestone: a bounded big-endian frame length followed by a media type byte and payload. Video uses type `0x01`; audio is reserved as `0x02`. Reverse input uses a bounded length-prefixed JSON control message. This is an interim compatibility layer only; before production release it must be upgraded to an authenticated session with explicit capabilities and replay protection.

The first session handshake should advertise the role and capabilities:

```text
role=windows_sender
capabilities=display_video,touch_input
session_mode=second_monitor
```

The Android receiver must reject a session that claims phone-control mode or an unsupported capability. The Windows sender must not interpret Android touch as Android OS control; it maps the coordinates to the selected Windows monitor.

## Windows display model

The normal mode captures a selected Windows monitor. The true second-monitor mode uses the existing Virtual Display Driver integration to create or select a virtual monitor, positions it in Windows display settings, and captures that monitor. The virtual-display driver remains optional because it is a privileged kernel-adjacent dependency and can affect display topology. The application must provide a safe fallback to ordinary monitor capture and must never install or execute an unverified driver package.

## Input mapping

Android touch coordinates are normalized to `[0,1]` relative to the rendered frame. Windows converts them to the selected monitor’s absolute desktop rectangle, accounting for letterboxing and monitor origin. Pointer motion and button transitions are sent through a dedicated Windows input injector with finite-value checks, coordinate bounds, event-rate limits, and explicit session authorization. The initial milestone maps touch to the Windows pointer on the selected display; it does not attempt to make a physical cursor cross an unavailable physical boundary.

## Audio

Audio is intentionally a separate milestone. Windows output capture should use shared-mode WASAPI loopback, while playback on Android should use a low-latency `AudioTrack` path. Android’s API level and playback-capture restrictions must be detected and surfaced. Reverse microphone/audio return requires a separate negotiated stream and echo/feedback policy; it should not be mixed into the video packet queue.

## Files and clipboard

Files and clipboard require a separate authenticated control plane with explicit user approval, filename/path sanitization, quotas, resumable transfers, and a user-visible transfer queue. Drag-and-drop should be implemented as a UI operation that starts an authenticated file transfer; it must not expose arbitrary Windows paths to the Android client. Clipboard synchronization should be opt-in and text-only initially.

## Performance target for the first milestone

The target is a low-resolution, low-bitrate second screen appropriate for the Acer Aspire 4739Z: start at 1024×600 or 1280×720, 20–30 fps, and a conservative bitrate. Video frames must be dropped under backpressure while control messages remain prioritized. The Android receiver should keep a bounded decoder queue and report connection, decode, and dropped-frame status.

## Implementation order

1. Produce a Windows sender-enabled build while retaining receiver mode.
2. Confirm Windows sender → Android receiver video interoperability over manual IP on the same trusted LAN.
3. Add the reverse pointer-control reader and selected-monitor coordinate mapping.
4. Add optional virtual-display creation/selection and second-monitor placement guidance.
5. Add WASAPI/Android audio as a separate negotiated capability.
6. Add authenticated file, clipboard, and drag/drop transfer.
7. Replace plaintext transport with pairing, encryption, capability authorization, and authenticated datagrams before general release.

## References

[1]: https://github.com/Genymobile/scrcpy "scrcpy official repository"
[2]: https://developer.android.com/media/grow/media-projection "Android MediaProjection documentation"
[3]: https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording "Microsoft WASAPI loopback recording"
[4]: https://superdisplay.app/ "SuperDisplay second-monitor product"
[5]: https://deskreen.com/ "Deskreen second-screen architecture"
[6]: https://github.com/VirtualDrivers/Virtual-Display-Driver "Virtual Display Driver project"
