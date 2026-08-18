# BetterCast Security, Reliability, and Performance Assessment

**Repository:** [StephenLovino/BetterCast](https://github.com/StephenLovino/BetterCast)
**Reviewed commit:** `9df66d541f493db4a03204aa7654411411c1586e`
**Remediation branch:** `security-hardening`
**Review date:** 18 August 2026
**Author:** Manus AI

## Executive summary

BetterCast is a legitimate-looking local screen/audio streaming project, not an obvious malware repository. The source is readable, its major components correspond to the advertised functionality, and the review found no clear credential theft, hidden persistence, analytics SDK, or unexplained cloud exfiltration. Those are meaningful positives, but they do not make the current design safe on an untrusted network.

The dominant issue is architectural: the protocol accepts peers based on TCP/UDP reachability and mDNS discovery rather than cryptographic identity. Traffic is plaintext, and input events received after a connection is established can become mouse, keyboard, Android ADB, or Windows control actions. A malicious device on the same LAN can therefore eavesdrop on media, impersonate a peer, inject control events, or consume resources with malformed framing. The Windows distribution adds a separate high-impact supply-chain concern because the installer and main receiver executable are unsigned, the installer runs as administrator, and the VDD path installs privileged components and historically falls back to executing the first executable found in a directory.

My current safety recommendation is **do not use the unmodified project on public, guest, hotel, airport, conference, school, corporate, or otherwise shared networks**. It is only conditionally acceptable in a trusted isolated lab or home LAN with manual peer selection, no unnecessary Accessibility/ADB permissions, and a disposable Windows test machine. The fixes below are ordered by risk and practicality.

## Risk scale and scope

| Rating | Meaning in this report |
|---|---|
| Critical | Remote compromise or code execution is plausible under ordinary deployment conditions, or a privileged installer path can execute attacker-controlled code. |
| High | A network attacker can cause host/device control, serious confidentiality loss, or broad denial of service without requiring prior local access. |
| Medium | Exploitation requires a narrower condition, or the likely impact is significant resource exhaustion, privacy leakage, or operational compromise. |
| Low | Defense-in-depth, maintainability, release-hygiene, or low-impact correctness problem. |

The assessment covers tracked Swift, Kotlin, C++, Objective-C, shell, Gradle, CMake, NSIS, property-list, entitlement, and GitHub Actions files; release metadata; the macOS DMG and Android APK hashes; and static inspection of the Windows installer payload. The follow-up execution work ran only compatible build commands in a disposable Linux copy. The DMG, APK, Windows executable, installer, driver, and macOS binaries were not launched because the available host is Ubuntu Linux without macOS, Android, Windows, Wine, or a compatible emulator. A localhost fuzz harness found no running BetterCast listener, so it sent no payloads and produced no runtime crash claim.

## Findings summary

| ID | Severity | Category | Affected components | Status targeted by this work |
|---|---|---|---|---|
| BC-01 | High | Unauthenticated and plaintext transport | Swift, Android, Qt/C++ TCP and UDP paths | Design documented; full cross-platform cryptographic pairing requires staged implementation |
| BC-02 | High | Remote input/control injection after connection | macOS Accessibility, Android ADB, Windows input/control paths | Protocol authorization and peer gating required |
| BC-03 | High | Unbounded or weakly bounded TCP framing | Swift/iOS listeners; Android and Qt variants have inconsistent behavior | Implement explicit limits and fail-closed parsing |
| BC-04 | High | Unbounded UDP reassembly/resource exhaustion | Swift, iOS, Android, Qt/C++ | Implement chunk, frame, byte, age, and sender budgets |
| BC-05 | High | Privileged Windows installer executes unsafe fallback helper | NSIS installer and VDD integration | Remove wildcard executable fallback and verify exact files |
| BC-06 | High | Unsigned Windows installer and main receiver | Windows release distribution | CI/release signing required; current artifact remains unsafe for blind installation |
| BC-07 | High | CI downloads unverified moving or third-party artifacts | GitHub Actions | Pin versions and fail closed on checksum/signature mismatch |
| BC-08 | Medium | Firewall rules expose services on Public profiles | Windows installer/runtime | Restrict to Private profile and document interface scope |
| BC-09 | Medium | Peer/session confusion and multi-client exposure | All listeners | Limit to one authorized session and bind UDP to authenticated peer |
| BC-10 | Medium | Malformed input events are weakly validated | Swift/Kotlin/C++ JSON models | Validate enum, finite numbers, ranges, key-code policy, and commands |
| BC-11 | Medium | Android UDP sender address can be hijacked | Android `UdpClient` | Pin peer address/port and expire idle sessions |
| BC-12 | Medium | Decoder work is performed while holding reassembly locks | Swift/Android UDP paths | Move decode/callback outside critical section |
| BC-13 | Medium | ADB process-per-event and automatic wireless ADB | macOS receiver mode | Require explicit opt-in, add timeouts, coalesce events, and bound work |
| BC-14 | Medium | Network send queues lack clear backpressure/drop policy | Qt sender, Kotlin senders | Bound queues and prefer dropping stale video to blocking control |
| BC-15 | Low/Medium | Reconnect/orientation lifecycle can churn resources | Swift receiver, Android sender | Add cancellation, debounce, and bounded retry behavior |
| BC-16 | Low/Medium | Release/security governance is incomplete | Default branch, CI, dependency monitoring | Publish `SECURITY.md`, enable scanning, add regression tests |

## Detailed vulnerability and bug analysis

### BC-01 — Unauthenticated, unencrypted local-network transport — High

The desktop receiver listens on all addresses and accepts TCP clients immediately. Its framing begins with a four-byte length and then media or event payloads; there is no handshake, certificate, shared secret, peer fingerprint, or authorization state. The Swift listeners explicitly construct `NWParameters(tls: nil, tcp: ...)`, and the Android server accepts the first TCP connection that reaches its port. [1] [2] [3]

This is both a confidentiality and an authenticity failure. Screen and audio packets are observable to a local-network attacker. More seriously, reachability is treated as identity: an attacker can advertise a convincing mDNS name, answer a manually selected address, or connect directly to an exposed port. mDNS is discovery, not authentication.

**Fix.** Use a standard authenticated channel rather than inventing encryption. A staged design should use TLS 1.3 with locally generated device certificates and explicit public-key fingerprint confirmation for TCP, or a vetted Noise implementation with a defined pairing flow. Apple exposes TLS options through Network.framework, while Qt provides peer-verification controls through `QSslSocket`; both must verify the expected peer identity rather than merely enabling encryption. [13] [14] [15] [16] For UDP, derive directional session keys from the authenticated TCP handshake and authenticate every datagram with a unique sequence/nonce; use an AEAD construction rather than a bare hash. Libsodium specifically distinguishes ordered `crypto_secretstream` use for reliable transports from AEAD plus per-message nonce/sequence handling for UDP. [17]

### BC-02 — Remote control injection through an accepted connection — High

The macOS sender receives length-prefixed JSON input events and passes ordinary events to `InputHandler`, which posts CoreGraphics mouse and keyboard events. The receiver-mode Swift code also sends input events to all connected clients and to an ADB injector. The Android sender maps received events into callback paths that can affect the casting device, and the Windows path has analogous control channels. [2] [4] [5]

No cryptographic authorization is checked before dispatch. The required Accessibility permission makes this particularly consequential on macOS: an attacker controlling the peer can click, type, invoke shortcuts, or interact with applications under the user’s desktop privileges. ADB injection is similarly powerful on a connected Android device.

**Fix.** Separate media authorization from control authorization. Pair peers explicitly, authenticate every control channel, and negotiate capabilities such as `receive_video`, `receive_audio`, `send_input`, and `request_keyframe`. Default to media-only; require a second explicit user action before enabling remote input or ADB. Reject all input events until the authenticated session and capability scope are established.

### BC-03 — Inconsistent and unsafe TCP length handling — High

The Swift receiver reads a 32-bit length and immediately uses it as the `maximumLength` passed to `NWConnection.receive`, with no explicit upper bound. The iOS implementation follows the same pattern. Android has a ten-million-byte limit, while the desktop Qt receiver has an eight-megabyte per-frame limit and a 32-megabyte per-connection buffer, but accepted connections are still unauthenticated and multiple peers can be active. [1] [2] [3]

Oversized, zero, or repeatedly malformed lengths can cause allocation pressure, parser churn, busy loops, or connection instability. A length-prefixed protocol must validate the length before allocation and must close a peer that repeatedly violates framing; continuing after an invalid length can leave the stream desynchronized.

**Fix.** Define a protocol-wide maximum frame size, reject zero and oversized lengths, use a bounded accumulator, impose header/body deadlines, and close the connection after a framing violation. For media, use a lower practical limit than the absolute safety cap and reject unexpected type/payload combinations before invoking a decoder.

### BC-04 — UDP reassembly permits attacker-controlled state growth — High

Swift, iOS, Android, and Qt/C++ accept `frameId`, `chunkId`, and `totalChunks` from the datagram and store chunks keyed by frame ID. They do not consistently reject zero or unreasonable chunk counts, enforce contiguous chunk IDs, cap the number of incomplete frames, cap aggregate bytes, or bind frames to an authenticated peer. Reassembly is triggered when the number of stored map entries equals the advertised total, even if those IDs are not the expected range. [1] [2] [6]

A LAN attacker can create many incomplete frame IDs, advertise large totals, send high-volume payloads, or force repeated sorting and concatenation. Because UDP is connectionless at the application layer, an attacker can also spoof or replace the apparent sender unless the peer is authenticated and pinned.

**Fix.** Enforce `1 <= totalChunks <= MAX_CHUNKS`, `0 <= chunkId < totalChunks`, a maximum payload per datagram, a maximum in-flight frame count, a maximum in-flight byte budget, a short frame deadline, and per-peer packet/byte rate limits. Reassemble only when every chunk ID in `[0,totalChunks)` exists. Copy the complete frame out of the locked map, release the lock, then decode.

### BC-05 — Privileged installer wildcard execution — High

The Windows NSIS installer copies VDD files and attempts driver installation. Its fallback searches for an arbitrary `*.exe` in the VDD directory and runs the first match silently. The application also searches for helper executables and invokes `devcon`, `pnputil`, or VDD Control. [7] [8]

This is dangerous because the installer runs with administrator privileges. A modified package, stale file, or compromised build workspace can cause an unintended executable to run silently. The behavior is a code-execution primitive even if the current repository contains no malicious payload.

**Fix.** Delete the wildcard fallback. Use exact expected filenames, verify their SHA-256 hashes or Authenticode signatures against values generated in CI, reject unexpected files, and invoke helpers with absolute paths and explicit arguments. If driver installation fails, stop with a visible error instead of executing an arbitrary alternative.

### BC-06 — Unsigned Windows installer and main receiver — High

Static inspection and Linux Authenticode verification found no signature on the Windows NSIS installer or `BetterCastReceiver.exe`. The bundled VDD helper and driver components had embedded signatures and matching message digests, but full chain verification was not reproducible from the Linux CA configuration. The ADB executable and helper DLLs verified successfully with `osslsigncode`. The absence of a signature on the application that orchestrates privileged installation remains a substantial release-integrity problem.

**Fix.** Sign the installer, main executable, and every project-owned helper with an organization-controlled certificate; sign driver packages using the required Windows driver-signing path; publish SHA-256 hashes and provenance attestations; and verify signatures in CI before packaging. The application should refuse to use an unverified VDD helper or driver package.

### BC-07 — Unverified CI downloads and moving dependencies — High

The Windows and Linux workflows download Android platform tools, VDD packages, NSIS, and `linuxdeploy` without consistently verifying checksums or signatures. At least one Linux dependency comes from a continuously published URL. [9] A compromised upstream, mirror, release tag, or CI runner could alter the packaged artifact without a detectable source change.

**Fix.** Pin immutable URLs and exact versions, store expected SHA-256 values in reviewed workflow files, verify before extraction, fail closed on mismatch, and retain the downloaded digest in build metadata. Prefer official signed releases and GitHub Actions pinned by commit SHA. Generate SBOMs and provenance attestations for each release.

### BC-08 — Windows firewall exposure includes Public profile — Medium

The installer and runtime create inbound exceptions for mDNS and TCP 51820 and apply them to both private and public profiles. The receiver binds to all local addresses. [7] [8] This can expose media parsing and control services on hotel, airport, conference, or other shared Wi-Fi even when the user believes the network is not trusted.

**Fix.** Default to no firewall modification. If the feature is enabled, scope rules to the Private profile, selected interface, and exact executable; use a user-visible consent step; remove rules on uninstall; and display the active listening address and profile.

### BC-09 — Multi-client/session confusion — Medium

The desktop receiver maintains a client list and sends heartbeats or input events to every client. Swift receiver mode similarly stores multiple connections and broadcasts events. This is inconsistent with the application’s apparent single-peer model and increases confidentiality, CPU, and authorization complexity. An unauthenticated extra client can receive media or control traffic once connected.

**Fix.** Allow one authenticated media session per role by default. Reject or queue new clients until the current peer disconnects, and associate TCP and UDP channels with a session identifier and pinned endpoint. Never broadcast control events to every connected socket unless capability and identity checks have passed.

### BC-10 — Weak input-event validation — Medium

The input schema accepts numeric fields without an explicit finite-number, range, or event-type validation contract. Coordinates can be outside `[0,1]`, scroll deltas can be extreme, unknown event types can reach dispatch code, and command key codes are not capability-scoped. [4] [5] JSON parsers do not provide security policy by themselves.

**Fix.** Validate UTF-8 and JSON size first, require finite coordinates and deltas, clamp or reject coordinates outside the permitted range, whitelist event types, enforce platform key-code ranges, and accept command events only from the capability-authorized role. Rate-limit mouse-move and scroll events.

### BC-11 — Android UDP peer hijacking — Medium

`UdpClient` replaces `senderAddress` and `senderPort` whenever a datagram arrives from a new source. A spoofed or competing LAN sender can therefore redirect heartbeats and input traffic, and the receiver has no authenticated session to distinguish the intended peer. [6]

**Fix.** Pin the UDP peer to the authenticated TCP peer’s address/port and session identifier. Do not replace it merely because a new datagram arrives. Expire the session after an idle timeout and require a fresh authenticated handshake.

### BC-12 — Decode callback while holding reassembly lock — Medium

The Android UDP client invokes `onFrameReassembled` while inside `synchronized(frameBuffers)`. The Swift implementation similarly invokes the decoder while holding `udpLock`. Video decode is comparatively expensive and may re-enter callbacks or block network processing, increasing lock contention and making a malformed high-rate stream more effective as a denial-of-service input. [2] [6]

**Fix.** Remove the complete frame from the map and copy or transfer ownership while locked; release the lock; then invoke the decoder. Keep the critical section limited to metadata and bounded buffer mutation.

### BC-13 — ADB process-per-event and automatic wireless ADB — Medium

The macOS ADB injector launches a new `adb` process for each input event and waits synchronously for completion on a serial queue. Mouse/scroll bursts can create a long backlog and consume substantial CPU. The receiver can also enable wireless ADB by issuing `adb tcpip 5555` and `adb connect` after connection, which expands the device’s attack surface and changes device networking without a fresh user approval. [2]

**Fix.** Make wireless ADB an explicit, separately confirmed action; never enable it merely because a connection becomes ready. Add process timeouts and cancellation, coalesce mouse moves, rate-limit scrolls, and use a persistent controlled bridge where possible. Keep an explicit capability flag for ADB input.

### BC-14 — Missing or unclear backpressure policy — Medium

The Qt sender writes every video/audio payload directly to `QTcpSocket`, and the Android senders use buffered coroutine channels. Slow receivers can cause queued bytes or frame drops without a clear policy. Repeated critical input events are intentionally sent three times, but there is no session-level queue budget or distinction between stale video and control traffic. [10] [11]

**Fix.** Bound queue bytes and frames, measure `bytesToWrite`, and drop stale non-key video frames when the queue exceeds the budget. Control and keyframe requests should use a small priority queue. Surface a degraded-stream status instead of allowing memory growth.

### BC-15 — Lifecycle and performance churn — Low/Medium

The Android sender polls display dimensions every 500 ms and creates a new encoder when dimensions change. Rapid orientation or split-screen transitions can repeatedly allocate encoders and surfaces. The macOS receiver retries ADB up to 15 times and runs multiple background processes. Stale-frame watchdogs request keyframes and reset decoders, which can amplify load during network loss. The default Android encoder target of 1280 pixels, 8 Mbps, and 30 fps can be expensive on older devices and can saturate Wi-Fi uplinks.

**Fix.** Debounce dimension changes, ensure the old encoder is fully stopped before another replacement, rate-limit keyframe requests, expose bitrate/FPS presets, and use measured queue/decoder telemetry. Use cancellation-aware process execution and bounded retry backoff.

### BC-16 — Release governance and testing gaps — Low/Medium

The security policy is on a non-default branch, automated dependency monitoring was not enabled during review, and the project does not currently demonstrate a cross-platform parser fuzzing suite or protocol interoperability tests. These gaps make regressions likely and reduce the chance that a vulnerability is reported and fixed quickly.

**Fix.** Publish `SECURITY.md` on the default branch, enable Dependabot/CodeQL or equivalent scanning, add unit tests for every framing parser, add property-based malformed-input tests, run sanitizers for C++, and make release verification a required CI job.

## Performance and operational constraints

| Area | Constraint | User-visible effect | Recommended control |
|---|---|---|---|
| Video encoding | Android target is 1280 px, 8 Mbps, 30 fps | Battery, heat, and bandwidth usage | Presets, adaptive bitrate, measured encoder backpressure |
| TCP buffering | Qt writes directly and Kotlin queues frames | Latency or memory growth on slow links | Byte/frame budgets; drop stale video |
| UDP reassembly | Sorting and concatenating arbitrary chunks | CPU/memory spikes; frame latency | Chunk/frame/byte limits and per-peer rate limits |
| Decoder | Native H.264/AAC decoders process network input | Crash or CPU exhaustion if parser boundaries fail | Authenticate and validate before decode; isolate where possible |
| Input injection | ADB process per event | High latency and CPU spikes | Coalesce, rate-limit, timeout, persistent bridge |
| Reconnect | Repeated ADB commands and keyframe resets | Background churn and network noise | Exponential backoff, idle cancellation, request quotas |
| Logging | First frames and periodic events are logged; issue reporting includes logs | Privacy leakage and I/O overhead | Redact addresses/device names and make upload review explicit |

## Remediation architecture

The safest long-term architecture is a versioned authenticated session:

1. Discovery remains an unauthenticated hint only. It must never authorize a peer.
2. The user manually approves a displayed peer fingerprint or scans a pairing code. The approval binds to a persistent device public key.
3. TCP uses TLS 1.3 or a vetted Noise handshake. The handshake authenticates both identities and negotiates protocol version and capabilities.
4. The TCP control stream carries a session ID, bounded length-prefixed messages, and authenticated commands. Input control is disabled unless explicitly approved.
5. UDP carries an authenticated session ID, frame ID, chunk ID, total chunks, sequence/nonce, and AEAD tag. The sender endpoint is pinned to the paired session.
6. Every parser enforces size, count, age, rate, and queue limits before allocation or codec invocation.
7. Releases are signed and reproducible. CI verifies every downloaded dependency before packaging and publishes hashes/SBOM/provenance.

The Noise specification explains that authenticated static keys and encrypted transport messages are distinct from an unauthenticated ephemeral handshake; choosing the wrong handshake pattern would leave the central problem unresolved. [12] Libsodium’s documentation likewise requires unique nonces and separate directional keys, and it recommends `crypto_secretstream` for ordered reliable streams while requiring explicit sequence/nonce handling for UDP. [13]

## Remediation implemented on `security-hardening`

The branch now contains concrete hardening changes across the Qt/C++ desktop receiver and sender, both Swift receiver implementations, the macOS sender input path, Android TCP/UDP clients and sender, Windows installer/runtime firewall configuration, native CMake options, and release workflows.

| Area | Implemented change |
|---|---|
| TCP framing | Zero, oversized, empty, unknown-type, and truncated frames are rejected; offending peers are disconnected instead of leaving streams desynchronized. |
| UDP reassembly | Chunk IDs and counts are bounded; in-flight frame count and bytes are capped; duplicate chunks do not increase accounting; incomplete frames expire; peer address/port is pinned; complete frames are decoded after releasing the lock. |
| Session admission | Desktop/root/Swift listeners limit active peers and clear peer identity and reassembly state on disconnect. |
| Remote input | Swift and Android validate event IDs, event types, finite numeric values, coordinate ranges, delta limits, and command allow-lists. CoreGraphics input additionally requires verified display bounds. |
| ADB | Automatic wireless ADB enablement on connection-ready was removed from the Swift/root receiver paths. Existing explicit ADB helper code still requires a broader product-level permission/UX review. |
| Backpressure | Qt sender payloads and socket queues are bounded; Android video send queues are limited and report dropped frames. |
| Windows safety | Wildcard `.exe`/generic `.inf` fallback execution was removed; only the expected `MttVDD.inf` package is eligible; firewall rules are scoped to the Private profile and local subnet. |
| CI supply chain | Windows downloads require configured SHA-256 repository variables; Linux pins linuxdeploy to release `1-alpha-20251107-1` and verifies its published digest; Dependabot and a default-branch security policy were added. |
| Native testing | CMake now supports opt-in AddressSanitizer/UndefinedBehaviorSanitizer builds and strict warnings. Android JVM regression tests cover input validation. |

The branch was validated with five repository hardening regression tests, all passing. Git whitespace checks and workflow YAML parsing also pass. The Android Gradle test task reached project evaluation but could not run because the sandbox has no Android SDK. Native C++/Swift compilation could not be performed because the sandbox has no CMake, Qt, Swift compiler, Apple SDK, or Windows toolchain. No compiled application or driver was launched.

## Important fixes still required before a safe release

The most important architectural issue remains unresolved: transport is still plaintext and unauthenticated in the Swift, iOS, and root receiver `NWParameters(tls: nil, ...)` paths and in the Qt/Android protocol. The new limits reduce parser and denial-of-service risk but do not stop a LAN attacker from impersonating a peer or reading media. The branch also does not yet implement cryptographic pairing, certificate/fingerprint verification, capability negotiation, authenticated UDP datagrams, or signed project-owned Windows binaries. Those should be treated as release blockers for any deployment on a shared network.

The authenticated/encrypted session must be implemented as a coordinated protocol version upgrade using a standard TLS/Noise/libsodium implementation, an explicit pairing-code or fingerprint-confirmation flow, directional keys, replay protection, and interoperability tests across Swift, Kotlin, iOS, and Qt. It should not be replaced with an ad-hoc hash or a static password embedded in source.

## References

[1]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverDesktop/NetworkListener.cpp "BetterCast Qt network listener"

[2]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastSender/ReceiverNetworkListener.swift "BetterCast Swift receiver network listener and ADB bridge"

[3]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/network/TcpClient.kt "BetterCast Android TCP client"

[4]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/sender/TcpSender.kt "BetterCast Android TCP sender"

[5]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/input/InputEvent.kt "BetterCast Android input model"

[6]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/network/UdpClient.kt "BetterCast Android UDP client"

[7]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverDesktop/installer.nsi "BetterCast NSIS installer"

[8]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverDesktop/sender/VirtualDisplayVDD.cpp "BetterCast virtual display integration"

[9]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/.github/workflows/build-windows-receiver.yml "BetterCast Windows build workflow"

[10]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverDesktop/sender/NetworkSender.cpp "BetterCast Qt network sender"

[11]: https://github.com/StephenLovino/BetterCast/blob/9df66d541f493db4a03204aa7654411411c1586e/Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/sender/TcpSender.kt "BetterCast Android TCP sender queue"

[12]: https://noiseprotocol.org/noise.html "The Noise Protocol Framework"

[13]: https://doc.libsodium.org/secret-key_cryptography/encrypted-messages "Libsodium authenticated messages and transport guidance"

[14]: https://developer.apple.com/documentation/network "Apple Network framework"

[15]: https://developer.apple.com/documentation/network/nwprotocoltls/options "Apple NWProtocolTLS options"

[16]: https://doc.qt.io/qt-6/qsslsocket.html "Qt QSslSocket"

[17]: https://cheatsheetseries.owasp.org/cheatsheets/Input_Validation_Cheat_Sheet.html "OWASP Input Validation Cheat Sheet"

[18]: https://cheatsheetseries.owasp.org/cheatsheets/Denial_of_Service_Cheat_Sheet.html "OWASP Denial of Service Cheat Sheet"
