# BetterCast Autonomous Work Ledger

_Last updated: 2026-08-19_

## Final product target

The final target is a Windows–Android ecosystem companion rather than a single casting mode. The intended product combines two explicit modes—Android as a Windows second display and Windows input control of the native Android UI—with a shared Wi-Fi session, safe suspend/resume, bidirectional file transfer, clipboard synchronization, audio routing where the operating systems permit it, discovery and pairing, recovery after sleep/network changes, low-spec performance tuning, and optional Android-to-Windows phone mirroring as a lower-priority capability. Second Display is a milestone, not the final endpoint.

## Confirmed requirements

| Area | Confirmed requirement |
|---|---|
| Primary platforms | Windows laptop and Android phone are the only current priorities. macOS, iOS, and Linux are not product priorities, although existing code is preserved unless removal is safe and useful. |
| Connectivity | Wi-Fi only. Do not require Bluetooth, USB, cloud relays, or user-operated pairing during this autonomous work. |
| Product modes | The Windows and Android clients need two explicit modes: **Second Display** and **Phone Control**. |
| Second Display | Android displays a Windows physical or virtual display stream. The Windows desktop remains the source. |
| Phone Control | Android keeps its native Android UI; Windows mouse and keyboard input should control Android over Wi-Fi. The first implemented authorization path is a user-enabled BetterCast AccessibilityService. |
| Suspend/resume | A Windows hotkey should suspend the second-display presentation without closing the session, allowing normal phone use, and request restoration with the same hotkey. Android background-activity restrictions mean immediate silent restoration cannot be guaranteed on every Android version; notification-mediated restore is the safe fallback. |
| Lower priority | Phone mirroring from Android to Windows is optional and not a first milestone, but remains part of the eventual product roadmap. |
| User capabilities | The user cannot perform development or interactive debugging. The user can install and run final artifacts for black-box verification. |
| Autonomous constraint | Do not perform actions that require user login, manual takeover, payment, approval, or interactive device input. Use automated CI and static validation only. |

## Current implementation status

| Component | Status |
|---|---|
| Security hardening | Implemented on the fork: bounded TCP/UDP parsing, peer admission, input validation, backpressure, installer hardening, CI download verification, and regression tests. |
| Legacy Windows white-window fix | Implemented: software OpenGL fallback deployment and `--software-opengl`/environment support. |
| Windows portable receiver | Previously built successfully and contains `opengl32sw.dll`. |
| Linux receiver | Previously built successfully. |
| Android AccessibilityService | Added and built successfully in CI after fixing the accessibility XML flag. It supports bounded tap, swipe, move overlay, and allow-listed global actions when explicitly armed. |
| Android control packet channel | Added as TCP packet type `0x03`, bounded to 16 KiB and limited to 240 packets per connection-second; the bound is covered by regression tests. |
| Windows control-only sender API | Added to `SenderController` and `NetworkSender`. |
| Windows Phone Control pointer milestone | Added: explicit control-only connection and center-anchored mouse movement/tap loop with Escape safety stop. This is not yet a true screen-edge handoff and does not yet provide full keyboard mapping. |
| Android session cleanup | TCP disconnect, LISTENING/ERROR/IDLE transitions, explicit disconnect, and view-model teardown now disarm the AccessibilityService session and clear suspended-display state where applicable. |
| Android debug APK | Successful CI artifacts from runs `32181809963` and `32185136237`; the latest hardening commit `ac631d5285e1c02342097c3eeee902a49c8529fd` is being validated by run `32185260188`. |
| Windows all-in-one sender/receiver | Successful CI artifact from run `32181810245` at commit `0ec94468590ee7b1cfc5d5dece29687f16bbb95d`; a current-commit rebuild is running as `32185288715`. Earlier attempts failed on x264 source hash mismatch; the workflow now uses OpenH264 and Qt 6.7.3. |
| Feature research notes | Official WASAPI loopback and Android MediaProjection constraints are recorded in `research_feature_constraints.md`; mirroring remains optional and requires per-session Android consent. |

## Assumptions made while user was unavailable

1. The first Phone Control milestone may use explicit activation rather than automatic edge handoff. It centers and confines the Windows pointer while active, uses Escape as a safety stop, and sends normalized coordinates to Android.
2. Full arbitrary Android keyboard injection is deferred because AccessibilityService gesture control is broadly available but raw key injection is restricted by Android. The implementation must distinguish gesture support from full keyboard support.
3. The Android app may require the user to enable its AccessibilityService once before Phone Control can be armed. This is a necessary OS permission, not a hidden escalation.
4. The Android notification/foreground-service path is the safe fallback for second-display restoration when Android blocks background activity launches.
5. The initial control protocol remains an intermediate milestone. Before production use on untrusted networks, it must gain authenticated pairing, encryption, replay protection, capability negotiation, and explicit revocation.
6. OpenH264 is an acceptable non-GPL software H.264 fallback candidate for the Windows sender, subject to CI/runtime verification. x264 is not silently re-enabled after its source hash failure.
7. Existing desktop sender and Android receiver video paths are reused rather than replacing them with a new media stack until a successful Windows all-in-one build proves the current path works.

## Open issues to resolve autonomously

- Complete the Windows all-in-one build and repair any Qt, OpenH264, CMake, linker, or packaging failures.
- Run automated Android and repository regression tests after each fix.
- Audit the new control protocol for malformed JSON, rate abuse, session authorization, and disconnect behavior; rate abuse and disconnect cleanup have now been hardened, while authentication/encryption remain release blockers.
- Add safe session suspend/resume state handling and the Windows hotkey foundation where it can be implemented without human setup.
- Produce a broader feature roadmap for files, clipboard, audio, second display, keyboard/text, and optional phone mirroring, clearly labeling platform limitations.
- Keep a final handoff with commit IDs, artifact links, hashes, test results, unresolved limitations, and a clear separation between completed capabilities, milestones, and future roadmap items.
