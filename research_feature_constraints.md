# Remaining Feature Research Notes

## Windows audio routing

Microsoft’s official WASAPI loopback guidance states that a client captures the audio stream played by a rendering endpoint by obtaining an `IMMDevice` for the render endpoint, initializing a shared-mode capture stream with `AUDCLNT_STREAMFLAGS_LOOPBACK`, and obtaining `IAudioCaptureClient` through `IAudioClient::GetService`. Loopback is supported only for shared-mode streams, not exclusive mode. Windows 10 version 1703 and later support event-driven loopback clients directly; older systems require a render-stream event workaround. WASAPI loopback does not require a vendor-specific Stereo Mix device and can be implemented against the system mix. Source: https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording

Implication for BetterCast: implement Windows-to-Android audio as a dedicated capture thread using shared-mode WASAPI loopback, normalize frames to a bounded PCM format, and send them over a bounded, sequence-numbered audio channel. The first version should explicitly document that DRM/protected content may not be capturable and that device/session changes require stream reinitialization.

## Android phone mirroring

Android’s official MediaProjection guidance identifies `MediaProjection.createVirtualDisplay()` as the central capture path. The application supplies a `Surface` backed by `MediaRecorder`, `SurfaceTexture`, or `ImageReader`; the virtual display dimensions should be selected using `WindowMetrics`, and the density should use `Configuration.densityDpi`. Android 14+ requires foreground-service declarations for the `mediaProjection` service type and user consent before every projection session. A projection token is single-use for `createVirtualDisplay()` on Android 14+.

Implication for BetterCast: optional phone mirroring must remain a separately armed feature with explicit per-session user consent, foreground-service lifecycle management, projection-callback cleanup, and a bounded video transport. It cannot be silently enabled by the Windows peer. Source: https://developer.android.com/media/grow/media-projection

## Remaining design priorities

File transfer and clipboard sync can be implemented without privileged Android APIs, but still require authenticated session pairing, bounded message sizes, resumable state, atomic writes, SHA-256 verification, and loop prevention. True second-display behavior requires a virtual-display implementation and a coordinate mapping contract; the existing Windows desktop capture is not sufficient by itself to make Android touch input address Windows screen coordinates.

## Clipboard and file-transfer constraints

Microsoft documents event-driven Windows clipboard monitoring through `AddClipboardFormatListener`, with changes delivered to the listener window; this is preferable to polling. Source: https://learn.microsoft.com/en-us/windows/win32/dataxchg/using-the-clipboard

Android’s app-specific storage guidance states that internal app-specific files require no storage permission and are inaccessible to other apps, while files that users expect to retain independently of the app should use shared storage/media collections. App-specific files are removed on uninstall. Source: https://developer.android.com/training/data-storage/app-specific

Implication for BetterCast: a first safe file-transfer implementation should stage incoming data in the Android app’s private files/cache directory, verify and atomically finalize it, then use an explicit user-mediated export/share flow for durable shared storage. Windows clipboard monitoring should be event-driven and text-only initially; synchronized updates need a source token and hash to prevent feedback loops.
