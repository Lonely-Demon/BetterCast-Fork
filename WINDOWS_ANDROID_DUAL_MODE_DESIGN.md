# BetterCast Windows–Android Dual-Mode Design

## Product definition

BetterCast will support two explicit, mutually understandable session modes between Windows and Android.

| Mode | Android screen | Windows role | Primary interaction |
|---|---|---|---|
| **Second Display** | BetterCast renders a Windows physical or virtual display surface | Windows captures and sends desktop pixels | Windows desktop content appears on the phone; touch can optionally act as a Windows pointer on the streamed surface |
| **Phone Control** | Android remains on its normal launcher/apps and is not replaced by a Windows video surface | Windows sends pointer and keyboard events over Wi-Fi | The Windows cursor is handed off at a configured edge and Android receives authorized gestures/global actions/text input |

The low-priority fallback is **Phone Mirror**, in which Android sends its own screen to Windows for viewing. This is not part of the first dual-mode milestone and must not be confused with Phone Control.

## Wi-Fi-only requirement

The transport must work over a trusted local Wi-Fi network without Bluetooth, USB, or cloud services. Discovery may use mDNS, but manual IP entry must always remain available. The first connection must display the peer identity, network address, selected mode, and requested capabilities before control is enabled.

## Android control authorization

Phone Control cannot safely inject arbitrary system input merely because a TCP peer sends JSON. Android should expose an explicit user-enabled BetterCast AccessibilityService configured with `canPerformGestures`. The service receives only authenticated, bounded control messages from the BetterCast session and can dispatch taps, swipes, scroll gestures, and approved global actions. A persistent notification indicates that remote control is active.

Full raw keyboard injection is more restricted than gesture dispatch. The first control milestone should support navigation keys and text insertion through an explicit, user-visible input method or a device-specific compatible path. BetterCast must report “gesture control available” separately from “full keyboard control available”; it must not imply that every Android phone supports arbitrary key injection.

## Windows pointer handoff

Windows needs a global pointer monitor and a configured Android-side edge. While the pointer remains inside Windows display rectangles, Windows handles it normally. When it reaches the configured edge, BetterCast switches to Phone Control and sends normalized pointer coordinates to Android. The Windows pointer should be clamped or hidden while the remote target owns the pointer, and it should be restored when the pointer returns through the handoff edge or the user presses the escape safety key.

The implementation must use Windows virtual-screen coordinates, including negative monitor origins, and must apply an edge dead-zone and rate limit to prevent accidental mode switching. A visible tray indicator and a safety hotkey must always be available.

## Second-display suspend/resume

In Second Display mode, a global hotkey sends a `DISPLAY_SUSPEND` command. Android stops presenting the Windows stream and the session service remains alive. The phone may then be used normally. A second hotkey sends `DISPLAY_RESUME`.

Android background activity launch restrictions mean that an application cannot guarantee that a background service will silently bring a full-screen activity to the foreground on every Android version. Therefore the robust behavior is:

1. The first hotkey suspends rendering and leaves the Wi-Fi service running.
2. Android shows a persistent BetterCast notification with a **Resume Second Display** action.
3. The second Windows hotkey requests resume.
4. If Android permits the launch, the display returns immediately; otherwise the user taps the notification action.

The app must preserve the session state and never close the Windows virtual display merely because rendering is suspended.

## Security and safety

The existing plaintext transport is not acceptable for remote control or file transfer. Before enabling Phone Control outside a trusted test network, the session must use authenticated pairing, encryption, capability negotiation, replay protection, and explicit revocation. Control messages must have bounded rates, finite coordinates, an allow-list of actions, and a local emergency stop.

## Implementation order

The first implementation milestone should be the Wi-Fi session protocol and Android service lifecycle, followed by Windows edge handoff and Android gestures. Second Display streaming and virtual-display integration should then be connected to the same session, with suspend/resume as an explicit session state rather than a process teardown. File transfer, clipboard, bidirectional audio, and low-priority Phone Mirror should be added as separately negotiated capabilities.

## References

[1]: https://developer.android.com/guide/topics/ui/accessibility/service "Android AccessibilityService guide"
[2]: https://developer.android.com/guide/components/activities/background-starts "Android background activity launch restrictions"
[3]: https://learn.microsoft.com/en-us/windows/win32/gdi/the-virtual-screen "Microsoft Windows virtual screen"
[4]: https://developer.android.com/develop/background-work/services/fgs "Android foreground services"
