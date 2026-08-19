#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

class SecureSession {
public:
    enum class Role : uint8_t {
        Initiator = 1,
        Responder = 2,
    };

    enum class State {
        New,
        HelloExchanged,
        HandshakeConfirmed,
        AwaitingApproval,
        Established,
        Failed,
    };

    explicit SecureSession(Role role);
    ~SecureSession();

    SecureSession(const SecureSession&) = delete;
    SecureSession& operator=(const SecureSession&) = delete;

    bool loadOrCreateIdentity(const QString& path, QString* error = nullptr);
    QByteArray makeHello(QString* error = nullptr);
    bool receiveHello(const QByteArray& message, QByteArray* response, QString* error = nullptr);
    bool receiveHelloReply(const QByteArray& message, QByteArray* authentication, QString* error = nullptr);
    QByteArray makeAuthentication(QString* error = nullptr);
    bool receiveAuthentication(const QByteArray& message, QByteArray* response, QString* error = nullptr);
    bool receiveConfirmation(const QByteArray& message, QByteArray* response, QString* error = nullptr);
    QByteArray makeConfirmationMessage(bool initiatorConfirmation, QString* error = nullptr);

    State state() const { return m_state; }
    Role role() const { return m_role; }
    bool isHandshakeConfirmed() const { return m_state == State::HandshakeConfirmed || isEstablished(); }
    bool isEstablished() const { return m_state == State::Established; }
    bool needsPeerApproval() const { return m_state == State::AwaitingApproval; }
    bool approvePeer(QString* error = nullptr);
    void setPinnedPeerPublicKey(const QByteArray& publicKey);
    bool hasPinnedPeer() const { return !m_pinnedPeerPublicKey.isEmpty(); }
    QByteArray peerIdentityPublicKey() const { return m_peerIdentityPublic; }

    QByteArray peerFingerprint() const;
    QString shortAuthenticationString() const;

    QByteArray encryptRecord(uint8_t type, const QByteArray& plaintext, QString* error = nullptr);
    bool decryptRecord(const QByteArray& record, uint8_t* type, QByteArray* plaintext, QString* error = nullptr);

    static constexpr uint8_t kProtocolVersion = 2;
    static constexpr uint8_t kSuiteAes256GcmP256 = 1;
    static constexpr int kMaxHandshakeMessage = 512;
    static constexpr int kMaxRecordPlaintext = 8 * 1024 * 1024;

private:
#ifdef _WIN32
    struct KeyHandle {
        BCRYPT_KEY_HANDLE value = nullptr;
        ~KeyHandle();
        KeyHandle() = default;
        KeyHandle(const KeyHandle&) = delete;
        KeyHandle& operator=(const KeyHandle&) = delete;
    };

    bool generateKeyPair(KeyHandle* key, QByteArray* publicKey, QByteArray* privateBlob, bool signing, QString* error);
    bool importPrivateKey(const QByteArray& blob, KeyHandle* key, bool signing, QString* error);
    bool importPublicKey(const QByteArray& publicKey, KeyHandle* key, bool signing, QString* error);
    bool exportPublicKey(BCRYPT_KEY_HANDLE key, QByteArray* publicKey, QString* error);
    bool deriveSecret(BCRYPT_KEY_HANDLE privateKey, BCRYPT_KEY_HANDLE peerPublicKey,
                      QByteArray* secret, QString* error);
    bool protectIdentity(const QByteArray& privateBlob, const QString& path, QString* error);
    bool unprotectIdentity(const QString& path, QByteArray* privateBlob, QString* error);
    bool hmacSha256(const QByteArray& key, const QByteArray& message, QByteArray* output, QString* error) const;
    bool sha256(const QByteArray& input, QByteArray* output, QString* error) const;
    bool aesGcm(bool encrypt, const QByteArray& key, const QByteArray& nonce,
                const QByteArray& aad, const QByteArray& input,
                QByteArray* output, QByteArray* tag, QString* error) const;
#endif

    bool fail(QString* error, const QString& message);
    bool deriveSessionKeys(QString* error);
    bool signTranscript(QByteArray* signature, QString* error);
    bool verifyTranscriptSignature(const QByteArray& signature, const QByteArray& publicKey, QString* error);
    bool validateHello(const QByteArray& message, uint8_t expectedType, QString* error) const;
    QByteArray buildHello(uint8_t type, uint8_t role, const QByteArray& nonce,
                          const QByteArray& ephemeralPublic, const QByteArray& identityPublic) const;
    QByteArray transcript() const;
    QByteArray hkdfExpand(const QByteArray& prk, const QByteArray& info, int length, QString* error) const;
    QByteArray makeConfirmation(bool initiatorConfirmation, QString* error);
    bool verifyConfirmation(const QByteArray& message, bool initiatorConfirmation, QString* error);
    QByteArray nonceForSequence(const QByteArray& base, uint64_t sequence) const;

    Role m_role;
    State m_state = State::New;
    QString m_identityPath;
    QByteArray m_identityPublic;
    QByteArray m_peerIdentityPublic;
    QByteArray m_localNonce;
    QByteArray m_peerNonce;
    QByteArray m_localEphemeralPublic;
    QByteArray m_peerEphemeralPublic;
    QByteArray m_localHello;
    QByteArray m_peerHello;
    QByteArray m_transcript;
    QByteArray m_peerFingerprint;
    QByteArray m_shortAuth;
    QByteArray m_txKey;
    QByteArray m_rxKey;
    QByteArray m_txNonceBase;
    QByteArray m_rxNonceBase;
    QByteArray m_confirmationKeyInitiator;
    QByteArray m_confirmationKeyResponder;
    uint64_t m_txSequence = 1;
    uint64_t m_rxSequence = 1;
    bool m_peerPinned = false;
    QByteArray m_pinnedPeerPublicKey;

#ifdef _WIN32
    KeyHandle m_identityKey;
    KeyHandle m_ephemeralKey;
#endif
};
