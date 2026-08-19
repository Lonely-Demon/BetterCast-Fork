# BetterCast Secure Transport Design

## Security objective and limits

BetterCast must not expose unauthenticated screen, audio, clipboard, file, or input channels on public networks. The secure design therefore requires confidentiality, integrity, peer authentication, replay resistance, downgrade resistance, bounded resource use, and explicit authorization before Android Accessibility actions are accepted.

No network protocol can guarantee that a source-code-aware attacker can do nothing in every circumstance. An attacker who fully controls either endpoint, obtains an endpoint’s stored identity key, compromises the operating system, or receives an authorized AccessibilityService action can act as that endpoint. The achievable guarantee is narrower and concrete: a remote attacker who controls the network but does not possess an authorized device identity or pairing secret must not be able to read, forge, replay, inject, downgrade, or authorize BetterCast traffic.

## Current audit findings

The current Windows and Android primary paths use raw TCP and UDP. Windows uses `QTcpSocket`/`QTcpServer`, Android uses `ServerSocket`/`Socket`, and the Swift paths explicitly construct `NWParameters(tls: nil, ...)`. The TCP application frame is a four-byte big-endian length followed by a type byte and payload. The implementation has useful size limits, peer admission limits, rate limits, and malformed-frame handling, but it accepts plaintext and has a legacy auto-detection path.

The current Windows receiver emits `connectionEstablished` as soon as a TCP socket is accepted, before any peer authentication. It also auto-detects legacy versus type-byte framing from the first body byte. This is unsafe for a public-network release because an unauthenticated peer can trigger connection state, probe the decoder, and potentially select compatibility behavior. The Android receiver similarly transitions to `CONNECTED` immediately after accepting a socket and starts reading media/control data before a cryptographic handshake.

UDP is currently unauthenticated and is bound independently from the TCP session. Peer address pinning and reassembly budgets reduce abuse but do not establish that UDP packets belong to the authenticated TCP peer. The secure release must either carry all security-sensitive traffic over the authenticated TCP channel or bind UDP packets to an authenticated session identifier and per-direction AEAD keys derived by the TCP handshake.

## Selected cryptographic architecture

The primary Windows–Android release should use a versioned, fail-closed secure session protocol built from platform cryptographic APIs rather than handwritten cryptographic primitives. Windows should use CNG (`BCrypt`) for P-256 ECDH, ECDSA signatures, SHA-256/HMAC/HKDF, AES-256-GCM, and CSPRNG. Android should use the platform `java.security` and `javax.crypto` implementations for P-256 ECDH/ECDSA, HMAC-SHA-256/HKDF, AES-256-GCM, and `SecureRandom`, with long-term identity material protected by Android Keystore where the device supports it.

Microsoft’s SDL guidance requires TLS 1.3 for secure protocols, recommends AES, warns that AES-GCM nonce reuse is catastrophic, and recommends P-256 or stronger for ECDH/ECDSA [1]. The application record layer will use the same conservative properties: AES-256-GCM with unique 96-bit nonces, separate direction keys, and strict sequence numbers. HKDF-SHA-256 will derive independent handshake, transmit, receive, and key-confirmation material. RFC 5869 specifies the extract-then-expand structure and recommends context-specific `info` values for key separation [2].

A fully custom TLS replacement is not the goal. The protocol will use a small, fixed, documented handshake and platform AEAD wrappers. The cryptographic wrappers must be tested against published vectors and cross-platform vectors before media traffic is enabled. No unauthenticated compatibility mode may remain in the public-network configuration.

## Pairing and identity model

Each installation receives a long-term P-256 identity key. Windows stores its private key using DPAPI-protected local storage; Android stores its identity key in Android Keystore when available, with an encrypted fallback only where necessary. The public-key fingerprint is the device identity.

On first pairing, the Windows sender and Android receiver perform an ephemeral P-256 ECDH exchange and sign the complete transcript with their long-term identity keys. The UI displays a short authentication string derived from the transcript, and the user confirms that the value shown on both devices matches. The peer public key and fingerprint are then pinned. On subsequent sessions, the pinned key authenticates the peer automatically; a changed key requires explicit re-pairing and must never be silently accepted.

If a user chooses not to persist a peer identity, a fresh visual confirmation is required for every session. A six-digit code alone is not sufficient for strong public-network authentication unless it is used inside a PAKE with rate limiting. SPAKE2 is a standardized password-authenticated exchange that derives a shared key without disclosing the password, but it requires careful group-element validation and is not a substitute for endpoint identity management [3]. The first implementation will prefer high-entropy device identity keys plus user-confirmed fingerprints; a PAKE can be added later for a deliberate code-entry pairing flow.

## Handshake outline

The handshake is versioned and binds all negotiation data to the transcript. The initiator sends `HELLO` containing the protocol version, supported cipher suite list limited to the one approved suite, role, random nonce, ephemeral P-256 public key, long-term public identity key, and a session identifier. The responder sends the corresponding `HELLO_REPLY`. Both sides verify key sizes, point encoding, role, version, and transcript limits before expensive operations.

Each side signs the transcript containing the protocol label, version, roles, both nonces, both ephemeral public keys, both long-term public keys, and the exact suite identifier. The peer signature is verified against the pinned or newly paired identity key. The ECDH shared secret is passed through HKDF-SHA-256 with a transcript-bound salt and context. Both sides exchange key-confirmation MACs and must verify them before emitting `connectionEstablished`, accepting control packets, or passing media to decoders.

The server must not advertise itself as connected before authentication succeeds. Unknown peers receive a bounded failure and are disconnected. Handshake attempts are rate-limited per source address and globally, with a small bounded pending-session table. There is no fallback to legacy framing after a secure handshake fails.

## Secure record format

After handshake, every TCP record uses a fixed secure envelope:

`magic(2) | version(1) | direction(1) | type(1) | flags(1) | sequence(8) | ciphertext_length(4) | ciphertext | tag(16)`

The header is authenticated as AEAD associated data. The nonce is derived from a per-direction base nonce and the monotonically increasing 64-bit sequence number, with a construction specified in the implementation document and tested for uniqueness. A record is accepted only if its direction, type, length, and sequence are valid for the authenticated session. Duplicate, stale, skipped, or out-of-window sequence numbers cause the session to close; the implementation will not silently reorder control packets.

The record payload remains bounded by class: control packets remain small, media packets retain explicit caps, and file/audio channels receive independent budgets. Sequence counters are never reset on reconnect under the same key. Reconnects perform a fresh handshake and derive fresh traffic keys. Key updates occur before counters approach their cryptoperiod limit.

UDP, if retained for media performance, uses the authenticated TCP handshake to derive a session identifier, UDP direction keys, and a nonce domain. Every UDP packet carries the session identifier, direction, sequence, and authenticated fragment metadata. Packets without a currently authenticated session are discarded before reassembly. The first secure milestone may disable UDP and use encrypted TCP only until the UDP binding is validated.

## Downgrade and compatibility policy

The public-network secure mode is a new protocol version. Legacy plaintext frames are accepted only behind an explicit developer/test flag that is disabled in release artifacts and cannot be activated by network input. The secure handshake authenticates the complete supported-version and cipher-suite list, preventing a network attacker from forcing a weaker mode. TLS 1.3’s design similarly uses authenticated negotiation and AEAD record protection [1].

Existing LAN-only builds may retain a temporary legacy mode for migration, but the UI must label it insecure and refuse to enable it when the Windows network profile is Public or when the user selects Public Network mode. The eventual release should remove the fallback entirely after secure artifacts are available.

## Testing requirements

Security validation must include cross-platform cryptographic test vectors, handshake success and failure cases, wrong-key and wrong-fingerprint tests, active transcript modification, downgrade attempts, replayed records, duplicate and out-of-order sequences, nonce/counter boundary tests, truncated and oversized records, invalid elliptic-curve points, invalid signatures, resource-exhaustion attempts, reconnect key separation, and UDP packets from non-session peers. Fuzzing must target the record parser and handshake state machine with bounded execution and memory instrumentation.

A successful build is not a proof of security. The release gate requires source-level assertions, sanitizer builds where available, deterministic protocol tests, dependency and artifact hash review, and a written threat-model assessment. Public-network release remains blocked until plaintext acceptance is removed from the release configuration and the artifacts have passed the adversarial suite.

## References

[1]: https://learn.microsoft.com/en-us/security/engineering/cryptographic-recommendations "Microsoft SDL cryptographic recommendations"

[2]: https://datatracker.ietf.org/doc/html/rfc5869 "RFC 5869: HMAC-based Extract-and-Expand Key Derivation Function"

[3]: https://datatracker.ietf.org/doc/html/rfc9382 "RFC 9382: SPAKE2, a Password-Authenticated Key Exchange"

[4]: https://datatracker.ietf.org/doc/html/rfc8446 "RFC 8446: The Transport Layer Security Protocol Version 1.3"

[5]: https://www.rfc-editor.org/rfc/rfc8439 "RFC 8439: ChaCha20 and Poly1305 for IETF Protocols"

## Additional platform findings

Microsoft’s SDL guidance states that TLS 1.3 must be enabled for secure protocols, AES is the required block cipher family for new symmetric encryption, AES-GCM provides authenticated encryption but is fragile under nonce reuse, and P-256 is the minimum recommended NIST curve for ECDH/ECDSA [1]. The Windows CNG `BCRYPT_ECCKEY_BLOB` documentation states that P-256 public-key X and Y coordinates are unsigned big-endian integers, which matches the SEC1 uncompressed point representation used by the Android Java APIs when the `04 || X || Y` prefix is removed for CNG import [6].

[6]: https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/ns-bcrypt-bcrypt_ecckey_blob "Microsoft BCRYPT_ECCKEY_BLOB structure"

## Concrete protocol state machine

The secure transport uses protocol version `2` and no legacy fallback in release mode. A TCP connection begins in `TCP_CONNECTED`, then moves through `HELLO_EXCHANGED`, `AUTHENTICATED`, `CONFIRMED`, and finally `SECURE_ESTABLISHED`. Any malformed, oversized, unexpected, unauthenticated, replayed, or timeout-causing message moves the connection directly to `CLOSED`. Media, audio, control, heartbeat, and future file/clipboard records are rejected in every state before `SECURE_ESTABLISHED`.

The handshake messages are length-bounded and separate from secure records. `HELLO` carries the fixed protocol version, role, suite identifier, 32-byte random nonce, 65-byte uncompressed P-256 ephemeral public key, and 65-byte long-term identity public key. `AUTH` carries a fixed-size ECDSA-P256/SHA-256 signature over the canonical transcript of both HELLO messages, role labels, and protocol label. Both sides derive the ECDH secret only after validating the peer point and signature. HKDF-SHA-256 derives two independent direction keys, a direction-specific nonce base, and confirmation keys. `CONFIRM` is an HMAC over the complete transcript and derived-session context.

The session is not trusted until both confirmation messages verify and, on first pairing, both endpoint UIs approve the same short authentication string derived from the transcript. The first approved peer identity is stored as a pinned public-key fingerprint. A changed identity is a hard failure requiring explicit unpair/re-pair; it is never automatically replaced.

Secure records carry a two-byte magic, version, direction, type, flags, 64-bit sequence, four-byte ciphertext length, AES-GCM ciphertext, and a 16-byte tag. The associated data is the canonical header. Each direction has a separate AES-256 key and a unique nonce domain. Sequence `0` is reserved for no record; accepted records begin at `1` and must arrive exactly at the next expected sequence. Reconnects always generate new ephemeral keys and derive new traffic keys. Plaintext media/control records are not accepted after a secure session is selected.

## Release gating

The public-network release gate requires the secure protocol to be the only default path, a visible first-pair approval workflow, encrypted identity-key storage, no legacy plaintext fallback in release artifacts, and a disabled or session-bound UDP path. A LAN compatibility build may exist during migration but must be separately labeled, opt-in, and rejected on Public Windows network profiles.
