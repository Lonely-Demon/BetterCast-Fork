#include "SecureSession.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace {
constexpr int kP256Bytes = 32;
constexpr int kP256PublicBytes = 65;
constexpr int kNonceBytes = 32;
constexpr int kGcmNonceBytes = 12;
constexpr int kGcmTagBytes = 16;
constexpr int kSignatureBytes = 64;
constexpr uint16_t kMagic = 0x4243; // BC
constexpr uint8_t kHello = 0x01;
constexpr uint8_t kHelloReply = 0x02;
constexpr uint8_t kAuthentication = 0x03;
constexpr uint8_t kConfirmation = 0x04;
constexpr uint8_t kRecordVersion = 2;

QByteArray u32be(uint32_t value) {
    QByteArray out(4, Qt::Uninitialized);
    qToBigEndian(value, reinterpret_cast<uchar*>(out.data()));
    return out;
}

QByteArray u64be(uint64_t value) {
    QByteArray out(8, Qt::Uninitialized);
    qToBigEndian(value, reinterpret_cast<uchar*>(out.data()));
    return out;
}

uint32_t readU32be(const char* data) {
    return qFromBigEndian<uint32_t>(reinterpret_cast<const uchar*>(data));
}

uint64_t readU64be(const char* data) {
    return qFromBigEndian<uint64_t>(reinterpret_cast<const uchar*>(data));
}

QString ntStatus(const char* operation, NTSTATUS status) {
    return QString("%1 failed with NTSTATUS 0x%2")
        .arg(operation)
        .arg(static_cast<uint32_t>(status), 8, 16, QChar('0'));
}

bool isExpectedPublicKey(const QByteArray& key) {
    return key.size() == kP256PublicBytes && static_cast<uint8_t>(key[0]) == 0x04;
}

QByteArray derEncodeEcdsa(const QByteArray& raw) {
    if (raw.size() != 64) return {};
    auto integer = [](QByteArray value) {
        while (value.size() > 1 && value[0] == 0) value.remove(0, 1);
        if (static_cast<uint8_t>(value[0]) & 0x80) value.prepend('\0');
        return QByteArray(1, static_cast<char>(0x02)) + QByteArray(1, static_cast<char>(value.size())) + value;
    };
    const QByteArray body = integer(raw.left(32)) + integer(raw.mid(32, 32));
    return QByteArray(1, static_cast<char>(0x30)) + QByteArray(1, static_cast<char>(body.size())) + body;
}

bool constantTimeEqual(const QByteArray& left, const QByteArray& right) {
    if (left.size() != right.size()) return false;
    uint8_t difference = 0;
    for (int i = 0; i < left.size(); ++i) {
        difference |= static_cast<uint8_t>(left[i]) ^ static_cast<uint8_t>(right[i]);
    }
    return difference == 0;
}

QByteArray derDecodeEcdsa(const QByteArray& der) {
    if (der.size() < 8 || static_cast<uint8_t>(der[0]) != 0x30 ||
        static_cast<uint8_t>(der[1]) != der.size() - 2) return {};
    int offset = 2;
    QByteArray raw(64, '\0');
    for (int part = 0; part < 2; ++part) {
        if (offset + 2 > der.size() || static_cast<uint8_t>(der[offset]) != 0x02) return {};
        const int length = static_cast<uint8_t>(der[offset + 1]);
        offset += 2;
        if (length <= 0 || length > 33 || offset + length > der.size()) return {};
        const QByteArray value = der.mid(offset, length);
        if (length == 33 && value[0] != '\0') return {};
        const QByteArray magnitude = length == 33 ? value.mid(1) : value;
        if (magnitude.size() > 32) return {};
        memcpy(raw.data() + part * 32 + 32 - magnitude.size(), magnitude.constData(), magnitude.size());
        offset += length;
    }
    return offset == der.size() ? raw : QByteArray();
}
}

SecureSession::KeyHandle::~KeyHandle() {
    if (value) BCryptDestroyKey(value);
    value = nullptr;
}

SecureSession::SecureSession(Role role) : m_role(role) {}

SecureSession::~SecureSession() = default;

bool SecureSession::fail(QString* error, const QString& message) {
    m_state = State::Failed;
    if (error) *error = message;
    return false;
}

bool SecureSession::sha256(const QByteArray& input, QByteArray* output, QString* error) const {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    QByteArray object;
    ULONG objectLength = 0;
    ULONG resultLength = 0;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        if (error) *error = ntStatus("BCryptOpenAlgorithmProvider(SHA-256)", status);
        return false;
    }

    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                               &resultLength, 0);
    if (status >= 0) object.resize(static_cast<int>(objectLength));
    if (status >= 0) {
        status = BCryptCreateHash(algorithm, &hash,
                                  reinterpret_cast<PUCHAR>(object.data()), objectLength,
                                  nullptr, 0, 0);
    }
    if (status >= 0) {
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.constData())),
                                static_cast<ULONG>(input.size()), 0);
    }
    QByteArray digest(32, Qt::Uninitialized);
    if (status >= 0) {
        status = BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                                  static_cast<ULONG>(digest.size()), 0);
    }

    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        if (error) *error = ntStatus("SHA-256", status);
        return false;
    }
    *output = digest;
    return true;
}

bool SecureSession::hmacSha256(const QByteArray& key, const QByteArray& message,
                               QByteArray* output, QString* error) const {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    QByteArray object;
    ULONG objectLength = 0;
    ULONG resultLength = 0;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                  BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                    reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                                    &resultLength, 0);
    }
    if (status >= 0) object.resize(static_cast<int>(objectLength));
    if (status >= 0) {
        status = BCryptCreateHash(algorithm, &hash,
                                  reinterpret_cast<PUCHAR>(object.data()), objectLength,
                                  reinterpret_cast<PUCHAR>(const_cast<char*>(key.constData())),
                                  static_cast<ULONG>(key.size()), 0);
    }
    if (status >= 0) {
        status = BCryptHashData(hash,
                                reinterpret_cast<PUCHAR>(const_cast<char*>(message.constData())),
                                static_cast<ULONG>(message.size()), 0);
    }
    QByteArray digest(32, Qt::Uninitialized);
    if (status >= 0) {
        status = BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                                  static_cast<ULONG>(digest.size()), 0);
    }

    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        if (error) *error = ntStatus("HMAC-SHA-256", status);
        return false;
    }
    *output = digest;
    return true;
}

QByteArray SecureSession::hkdfExpand(const QByteArray& prk, const QByteArray& info,
                                     int length, QString* error) const {
    if (length < 0 || length > 255 * 32 || prk.size() != 32) return {};
    QByteArray result;
    QByteArray previous;
    for (uint8_t counter = 1; result.size() < length; ++counter) {
        QByteArray message = previous + info + QByteArray(1, static_cast<char>(counter));
        QByteArray block;
        if (!hmacSha256(prk, message, &block, error)) return {};
        result.append(block);
        previous = block;
    }
    result.truncate(length);
    return result;
}

bool SecureSession::generateKeyPair(KeyHandle* key, QByteArray* publicKey,
                                     QByteArray* privateBlob, bool signing, QString* error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const wchar_t* algorithmId = signing ? BCRYPT_ECDSA_P256_ALGORITHM : BCRYPT_ECDH_P256_ALGORITHM;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, algorithmId, nullptr, 0);
    if (status >= 0) {
        status = BCryptGenerateKeyPair(algorithm, &key->value, 256, 0);
    }
    if (status >= 0) status = BCryptFinalizeKeyPair(key->value, 0);
    if (status < 0) {
        if (error) *error = ntStatus("P-256 key generation", status);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    if (!exportPublicKey(key->value, publicKey, error)) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    ULONG blobSize = 0;
    status = BCryptExportKey(key->value, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &blobSize, 0);
    if (status >= 0) privateBlob->resize(static_cast<int>(blobSize));
    if (status >= 0) {
        status = BCryptExportKey(key->value, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                 reinterpret_cast<PUCHAR>(privateBlob->data()), blobSize,
                                 &blobSize, 0);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        if (error) *error = ntStatus("P-256 private-key export", status);
        return false;
    }
    return true;
}

bool SecureSession::exportPublicKey(BCRYPT_KEY_HANDLE key, QByteArray* publicKey, QString* error) {
    ULONG blobSize = 0;
    NTSTATUS status = BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &blobSize, 0);
    QByteArray blob;
    if (status >= 0) blob.resize(static_cast<int>(blobSize));
    if (status >= 0) {
        status = BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                 reinterpret_cast<PUCHAR>(blob.data()), blobSize,
                                 &blobSize, 0);
    }
    if (status < 0 || blob.size() < 8 + 2 * kP256Bytes) {
        if (error) *error = ntStatus("P-256 public-key export", status);
        return false;
    }
    const auto* header = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.constData());
    if (header->cbKey != kP256Bytes) {
        if (error) *error = "Unexpected P-256 public-key size";
        return false;
    }
    *publicKey = QByteArray(1, static_cast<char>(0x04));
    publicKey->append(blob.constData() + 8, 2 * kP256Bytes);
    return true;
}

bool SecureSession::importPrivateKey(const QByteArray& blob, KeyHandle* key, bool signing, QString* error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const wchar_t* algorithmId = signing ? BCRYPT_ECDSA_P256_ALGORITHM : BCRYPT_ECDH_P256_ALGORITHM;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, algorithmId, nullptr, 0);
    if (status >= 0) {
        status = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                     &key->value, reinterpret_cast<PUCHAR>(const_cast<char*>(blob.constData())),
                                     static_cast<ULONG>(blob.size()), 0);
    }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        if (error) *error = ntStatus("P-256 private-key import", status);
        return false;
    }
    return true;
}

bool SecureSession::importPublicKey(const QByteArray& publicKey, KeyHandle* key, bool signing, QString* error) {
    if (!isExpectedPublicKey(publicKey)) {
        if (error) *error = "Invalid P-256 public key encoding";
        return false;
    }
    QByteArray blob(8 + 2 * kP256Bytes, Qt::Uninitialized);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = signing ? BCRYPT_ECDSA_PUBLIC_P256_MAGIC : BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = kP256Bytes;
    memcpy(blob.data() + 8, publicKey.constData() + 1, 2 * kP256Bytes);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const wchar_t* algorithmId = signing ? BCRYPT_ECDSA_P256_ALGORITHM : BCRYPT_ECDH_P256_ALGORITHM;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, algorithmId, nullptr, 0);
    if (status >= 0) {
        status = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                     &key->value, reinterpret_cast<PUCHAR>(blob.data()),
                                     static_cast<ULONG>(blob.size()), 0);
    }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        if (error) *error = ntStatus("P-256 public-key import", status);
        return false;
    }
    return true;
}

bool SecureSession::deriveSecret(BCRYPT_KEY_HANDLE privateKey, BCRYPT_KEY_HANDLE peerPublicKey,
                                 QByteArray* secret, QString* error) {
    BCRYPT_SECRET_HANDLE agreement = nullptr;
    NTSTATUS status = BCryptSecretAgreement(privateKey, peerPublicKey, &agreement, 0);
    ULONG size = 0;
    if (status >= 0) {
        status = BCryptDeriveKey(agreement, BCRYPT_KDF_RAW_SECRET, nullptr, nullptr, 0, &size, 0);
    }
    if (status >= 0) secret->resize(static_cast<int>(size));
    if (status >= 0) {
        status = BCryptDeriveKey(agreement, BCRYPT_KDF_RAW_SECRET, nullptr,
                                 reinterpret_cast<PUCHAR>(secret->data()), size, &size, 0);
    }
    if (agreement) BCryptDestroySecret(agreement);
    if (status < 0) {
        if (error) *error = ntStatus("P-256 ECDH", status);
        return false;
    }
    return true;
}

bool SecureSession::protectIdentity(const QByteArray& privateBlob, const QString& path, QString* error) {
    DATA_BLOB input{static_cast<DWORD>(privateBlob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(privateBlob.constData()))};
    DATA_BLOB protectedBlob{};
    if (!CryptProtectData(&input, L"BetterCast identity key", nullptr, nullptr, nullptr, 0, &protectedBlob)) {
        if (error) *error = QString("CryptProtectData failed with Win32 error %1").arg(GetLastError());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(reinterpret_cast<const char*>(protectedBlob.pbData),
                                                       protectedBlob.cbData) != protectedBlob.cbData || !file.commit()) {
        LocalFree(protectedBlob.pbData);
        if (error) *error = "Unable to write protected identity key";
        return false;
    }
    LocalFree(protectedBlob.pbData);
    return true;
}

bool SecureSession::unprotectIdentity(const QString& path, QByteArray* privateBlob, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
        if (error) *error = "Unable to read protected identity key";
        return false;
    }
    QByteArray encrypted = file.readAll();
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(encrypted.data())};
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &plain)) {
        if (error) *error = QString("CryptUnprotectData failed with Win32 error %1").arg(GetLastError());
        return false;
    }
    *privateBlob = QByteArray(reinterpret_cast<const char*>(plain.pbData), static_cast<int>(plain.cbData));
    LocalFree(plain.pbData);
    return true;
}

bool SecureSession::loadOrCreateIdentity(const QString& path, QString* error) {
    m_identityPath = path;
    QByteArray privateBlob;
    if (QFile::exists(path)) {
        if (!unprotectIdentity(path, &privateBlob, error)) return false;
        if (!importPrivateKey(privateBlob, &m_identityKey, true, error)) return false;
        return exportPublicKey(m_identityKey.value, &m_identityPublic, error);
    }

    if (!generateKeyPair(&m_identityKey, &m_identityPublic, &privateBlob, true, error)) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    return protectIdentity(privateBlob, path, error);
}

QByteArray SecureSession::buildHello(uint8_t type, uint8_t role, const QByteArray& nonce,
                                     const QByteArray& ephemeralPublic, const QByteArray& identityPublic) const {
    QByteArray message;
    message.append('B');
    message.append('C');
    message.append(static_cast<char>(kProtocolVersion));
    message.append(static_cast<char>(type));
    message.append(static_cast<char>(role));
    message.append(static_cast<char>(kSuiteAes256GcmP256));
    message.append(nonce);
    message.append(ephemeralPublic);
    message.append(identityPublic);
    return message;
}

bool SecureSession::validateHello(const QByteArray& message, uint8_t expectedType, QString* error) const {
    if (message.size() != 2 + 1 + 1 + 1 + 1 + kNonceBytes + kP256PublicBytes + kP256PublicBytes ||
        message[0] != 'B' || message[1] != 'C' || static_cast<uint8_t>(message[2]) != kProtocolVersion ||
        static_cast<uint8_t>(message[3]) != expectedType || static_cast<uint8_t>(message[5]) != kSuiteAes256GcmP256) {
        if (error) *error = "Invalid secure-session hello";
        return false;
    }
    if (!isExpectedPublicKey(message.mid(6 + kNonceBytes, kP256PublicBytes)) ||
        !isExpectedPublicKey(message.mid(6 + kNonceBytes + kP256PublicBytes, kP256PublicBytes))) {
        if (error) *error = "Invalid secure-session public key";
        return false;
    }
    return true;
}

QByteArray SecureSession::makeHello(QString* error) {
    if (m_state != State::New) return fail(error, "Secure hello already sent"), QByteArray();
    QByteArray privateBlob;
    if (m_identityPublic.size() != kP256PublicBytes || !generateKeyPair(&m_ephemeralKey,
                                                                         &m_localEphemeralPublic,
                                                                         &privateBlob, false, error)) {
        m_state = State::Failed;
        return {};
    }
    m_localNonce.resize(kNonceBytes);
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(m_localNonce.data()), kNonceBytes,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        fail(error, "Secure random generation failed");
        return {};
    }
    const uint8_t type = m_role == Role::Initiator ? kHello : kHelloReply;
    m_localHello = buildHello(type, static_cast<uint8_t>(m_role), m_localNonce,
                              m_localEphemeralPublic, m_identityPublic);
    m_state = State::HelloExchanged;
    return m_localHello;
}

bool SecureSession::receiveHello(const QByteArray& message, QByteArray* response, QString* error) {
    if (m_role != Role::Responder || m_state != State::New || !validateHello(message, kHello, error)) {
        return fail(error, error && !error->isEmpty() ? *error : "Unexpected secure hello");
    }
    const uint8_t peerRole = static_cast<uint8_t>(message[4]);
    if (peerRole != static_cast<uint8_t>(Role::Initiator)) return fail(error, "Invalid secure hello role");

    m_peerHello = message;
    m_peerNonce = message.mid(6, kNonceBytes);
    m_peerEphemeralPublic = message.mid(6 + kNonceBytes, kP256PublicBytes);
    m_peerIdentityPublic = message.mid(6 + kNonceBytes + kP256PublicBytes, kP256PublicBytes);
    m_localNonce.resize(kNonceBytes);
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(m_localNonce.data()), kNonceBytes,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return fail(error, "Secure random generation failed");

    QByteArray privateBlob;
    if (!generateKeyPair(&m_ephemeralKey, &m_localEphemeralPublic, &privateBlob, false, error)) return false;
    m_localHello = buildHello(kHelloReply, static_cast<uint8_t>(m_role), m_localNonce,
                              m_localEphemeralPublic, m_identityPublic);
    m_state = State::HelloExchanged;
    *response = m_localHello;
    return true;
}

bool SecureSession::receiveHelloReply(const QByteArray& message, QByteArray* authentication, QString* error) {
    if (m_role != Role::Initiator || m_state != State::HelloExchanged ||
        !validateHello(message, kHelloReply, error)) {
        return fail(error, error && !error->isEmpty() ? *error : "Unexpected secure hello reply");
    }
    const uint8_t peerRole = static_cast<uint8_t>(message[4]);
    if (peerRole != static_cast<uint8_t>(Role::Responder)) return fail(error, "Invalid secure hello reply role");
    m_peerHello = message;
    m_peerNonce = message.mid(6, kNonceBytes);
    m_peerEphemeralPublic = message.mid(6 + kNonceBytes, kP256PublicBytes);
    m_peerIdentityPublic = message.mid(6 + kNonceBytes + kP256PublicBytes, kP256PublicBytes);
    if (!deriveSessionKeys(error)) return false;
    *authentication = makeAuthentication(error);
    return !authentication->isEmpty();
}

QByteArray SecureSession::transcript() const {
    if (m_role == Role::Initiator) return m_localHello + m_peerHello;
    return m_peerHello + m_localHello;
}

bool SecureSession::deriveSessionKeys(QString* error) {
    KeyHandle peerKey;
    if (!importPublicKey(m_peerEphemeralPublic, &peerKey, false, error)) return false;
    QByteArray sharedSecret;
    if (!deriveSecret(m_ephemeralKey.value, peerKey.value, &sharedSecret, error)) return false;

    const QByteArray canonicalTranscript = transcript();
    QByteArray transcriptHash;
    if (!sha256(canonicalTranscript, &transcriptHash, error)) return false;
    const QByteArray initiatorNonce = m_role == Role::Initiator ? m_localNonce : m_peerNonce;
    const QByteArray responderNonce = m_role == Role::Initiator ? m_peerNonce : m_localNonce;
    QByteArray salt;
    if (!sha256(QByteArray("BetterCast/v2/salt") + initiatorNonce + responderNonce, &salt, error)) return false;
    QByteArray prk;
    if (!hmacSha256(salt, sharedSecret, &prk, error)) return false;

    auto expand = [&](const QByteArray& label, int length) {
        return hkdfExpand(prk, QByteArray("BetterCast/v2/") + label + transcriptHash, length, error);
    };
    const QByteArray initiatorTx = expand("initiator-tx", 32);
    const QByteArray responderTx = expand("responder-tx", 32);
    const QByteArray initiatorNonceBase = expand("initiator-nonce", kGcmNonceBytes);
    const QByteArray responderNonceBase = expand("responder-nonce", kGcmNonceBytes);
    m_confirmationKeyInitiator = expand("confirm-initiator", 32);
    m_confirmationKeyResponder = expand("confirm-responder", 32);
    if (initiatorTx.size() != 32 || responderTx.size() != 32 || initiatorNonceBase.size() != kGcmNonceBytes ||
        responderNonceBase.size() != kGcmNonceBytes || m_confirmationKeyInitiator.size() != 32 ||
        m_confirmationKeyResponder.size() != 32) {
        return fail(error, "Secure key derivation failed");
    }
    if (m_role == Role::Initiator) {
        m_txKey = initiatorTx; m_rxKey = responderTx;
        m_txNonceBase = initiatorNonceBase; m_rxNonceBase = responderNonceBase;
    } else {
        m_txKey = responderTx; m_rxKey = initiatorTx;
        m_txNonceBase = responderNonceBase; m_rxNonceBase = initiatorNonceBase;
    }
    m_transcript = canonicalTranscript;
    if (!m_pinnedPeerPublicKey.isEmpty() && m_pinnedPeerPublicKey != m_peerIdentityPublic) {
        return fail(error, "Pinned peer identity changed");
    }
    m_peerFingerprint = QCryptographicHash::hash(m_peerIdentityPublic, QCryptographicHash::Sha256);
    m_shortAuth = QCryptographicHash::hash(QByteArray("BetterCast/v2/sas") + m_transcript, QCryptographicHash::Sha256).left(6);
    return true;
}

bool SecureSession::signTranscript(QByteArray* signature, QString* error) {
    QByteArray digest;
    if (!sha256(m_transcript, &digest, error)) return false;
    ULONG signatureLength = 0;
    NTSTATUS status = BCryptSignHash(m_identityKey.value, nullptr,
                                     reinterpret_cast<PUCHAR>(digest.data()), digest.size(),
                                     nullptr, 0, &signatureLength, 0);
    if (status >= 0) signature->resize(static_cast<int>(signatureLength));
    if (status >= 0) {
        status = BCryptSignHash(m_identityKey.value, nullptr,
                                reinterpret_cast<PUCHAR>(digest.data()), digest.size(),
                                reinterpret_cast<PUCHAR>(signature->data()), signatureLength,
                                &signatureLength, 0);
    }
    if (status < 0 || signature->size() != kSignatureBytes) {
        if (error) *error = ntStatus("ECDSA transcript signature", status);
        return false;
    }
    return true;
}

bool SecureSession::verifyTranscriptSignature(const QByteArray& signature, const QByteArray& publicKey,
                                              QString* error) {
    const QByteArray rawSignature = derDecodeEcdsa(signature);
    if (rawSignature.size() != kSignatureBytes) return fail(error, "Invalid transcript signature encoding");
    KeyHandle peerKey;
    if (!importPublicKey(publicKey, &peerKey, true, error)) return false;
    QByteArray digest;
    if (!sha256(m_transcript, &digest, error)) return false;
    NTSTATUS status = BCryptVerifySignature(peerKey.value, nullptr,
                                            reinterpret_cast<PUCHAR>(digest.data()), digest.size(),
                                            reinterpret_cast<PUCHAR>(const_cast<char*>(rawSignature.constData())),
                                            rawSignature.size(), 0);
    if (status < 0) return fail(error, ntStatus("ECDSA transcript verification", status));
    return true;
}

QByteArray SecureSession::makeAuthentication(QString* error) {
    if (m_state != State::HelloExchanged && m_state != State::HandshakeConfirmed) {
        fail(error, "Unexpected authentication state");
        return {};
    }
    if (m_transcript.isEmpty() && !deriveSessionKeys(error)) return {};
    QByteArray rawSignature;
    if (!signTranscript(&rawSignature, error)) return {};
    const QByteArray signature = derEncodeEcdsa(rawSignature);
    if (signature.isEmpty()) { fail(error, "ECDSA signature encoding failed"); return {}; }
    QByteArray message(1, static_cast<char>(kAuthentication));
    message.append(u32be(static_cast<uint32_t>(signature.size())));
    message.append(signature);
    return message;
}

bool SecureSession::receiveAuthentication(const QByteArray& message, QByteArray* response, QString* error) {
    if (message.size() < 1 + 4 + 8 || message.size() > 1 + 4 + 80 ||
        static_cast<uint8_t>(message[0]) != kAuthentication) {
        return fail(error, "Invalid secure authentication message");
    }
    if (m_transcript.isEmpty() && !deriveSessionKeys(error)) return false;
    const uint32_t signatureLength = readU32be(message.constData() + 1);
    if (signatureLength < 8 || signatureLength > 80 || message.size() != 5 + static_cast<int>(signatureLength)) {
        return fail(error, "Invalid secure authentication signature length");
    }
    const QByteArray signature = message.mid(5, static_cast<int>(signatureLength));
    if (!verifyTranscriptSignature(signature, m_peerIdentityPublic, error)) return false;
    if (m_role == Role::Responder) {
        *response = makeAuthentication(error);
        if (response->isEmpty()) return false;
    }
    m_state = State::HandshakeConfirmed;
    return true;
}

QByteArray SecureSession::makeConfirmation(bool initiatorConfirmation, QString* error) {
    if (m_transcript.isEmpty() && !deriveSessionKeys(error)) return {};
    const QByteArray key = initiatorConfirmation ? m_confirmationKeyInitiator : m_confirmationKeyResponder;
    QByteArray mac;
    if (!hmacSha256(key, QByteArray("BetterCast/v2/confirm") + m_transcript, &mac, error)) return {};
    QByteArray message(1, static_cast<char>(kConfirmation));
    message.append(mac);
    return message;
}

QByteArray SecureSession::makeConfirmationMessage(bool initiatorConfirmation, QString* error) {
    if (m_state != State::HandshakeConfirmed && m_state != State::AwaitingApproval) {
        fail(error, "Secure confirmation is not valid in the current state");
        return {};
    }
    return makeConfirmation(initiatorConfirmation, error);
}

bool SecureSession::verifyConfirmation(const QByteArray& message, bool initiatorConfirmation, QString* error) {
    if (message.size() != 1 + 32 || static_cast<uint8_t>(message[0]) != kConfirmation) {
        return fail(error, "Invalid secure confirmation message");
    }
    const QByteArray expected = makeConfirmation(initiatorConfirmation, error).mid(1);
    const QByteArray received = message.mid(1);
    if (expected.size() != 32 || received.size() != 32 || !constantTimeEqual(expected, received)) {
        return fail(error, "Secure key confirmation failed");
    }
    return true;
}

bool SecureSession::receiveConfirmation(const QByteArray& message, QByteArray* response, QString* error) {
    const bool expectedInitiator = m_role == Role::Responder;
    if (!verifyConfirmation(message, expectedInitiator, error)) return false;
    if (m_role == Role::Responder) {
        *response = makeConfirmation(false, error);
        if (response->isEmpty()) return false;
    }
    m_state = State::AwaitingApproval;
    return true;
}

bool SecureSession::approvePeer(QString* error) {
    if (m_state != State::AwaitingApproval && m_state != State::HandshakeConfirmed) {
        return fail(error, "Secure session is not awaiting approval");
    }
    if (m_peerIdentityPublic.size() != kP256PublicBytes) {
        return fail(error, "Peer identity is unavailable");
    }
    m_pinnedPeerPublicKey = m_peerIdentityPublic;
    m_peerPinned = true;
    m_state = State::Established;
    return true;
}

void SecureSession::setPinnedPeerPublicKey(const QByteArray& publicKey) {
    if (publicKey.size() == kP256PublicBytes) m_pinnedPeerPublicKey = publicKey;
}

QByteArray SecureSession::peerFingerprint() const {
    return m_peerFingerprint.toHex();
}

QString SecureSession::shortAuthenticationString() const {
    if (m_shortAuth.size() < 6) return {};
    const QByteArray hex = m_shortAuth.toHex().toUpper();
    return QString::fromLatin1(hex.mid(0, 4) + "-" + hex.mid(4, 4) + "-" + hex.mid(8, 4));
}

QByteArray SecureSession::nonceForSequence(const QByteArray& base, uint64_t sequence) const {
    QByteArray nonce = base;
    const QByteArray counter = u64be(sequence);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<char>(nonce[4 + i] ^ counter[i]);
    return nonce;
}

bool SecureSession::aesGcm(bool encrypt, const QByteArray& key, const QByteArray& nonce,
                           const QByteArray& aad, const QByteArray& input,
                           QByteArray* output, QByteArray* tag, QString* error) const {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    QByteArray object;
    ULONG objectLength = 0;
    ULONG resultLength = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                                   reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                   sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    }
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                                   &resultLength, 0);
    }
    if (status >= 0) object.resize(static_cast<int>(objectLength));
    if (status >= 0) {
        status = BCryptGenerateSymmetricKey(algorithm, &keyHandle,
                                            reinterpret_cast<PUCHAR>(object.data()), objectLength,
                                            reinterpret_cast<PUCHAR>(const_cast<char*>(key.constData())),
                                            static_cast<ULONG>(key.size()), 0);
    }

    QByteArray localTag(kGcmTagBytes, Qt::Uninitialized);
    if (status >= 0) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.constData()));
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(aad.constData()));
        info.cbAuthData = static_cast<ULONG>(aad.size());
        info.pbTag = reinterpret_cast<PUCHAR>(localTag.data());
        info.cbTag = static_cast<ULONG>(localTag.size());
        output->resize(input.size());
        ULONG outputLength = 0;
        if (encrypt) {
            status = BCryptEncrypt(keyHandle,
                                    reinterpret_cast<PUCHAR>(const_cast<char*>(input.constData())),
                                    static_cast<ULONG>(input.size()), &info,
                                    nullptr, 0,
                                    reinterpret_cast<PUCHAR>(output->data()),
                                    static_cast<ULONG>(output->size()), &outputLength, 0);
        } else {
            status = BCryptDecrypt(keyHandle,
                                    reinterpret_cast<PUCHAR>(const_cast<char*>(input.constData())),
                                    static_cast<ULONG>(input.size()), &info,
                                    nullptr, 0,
                                    reinterpret_cast<PUCHAR>(output->data()),
                                    static_cast<ULONG>(output->size()), &outputLength, 0);
        }
        output->resize(static_cast<int>(outputLength));
    }
    if (keyHandle) BCryptDestroyKey(keyHandle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        if (error) *error = ntStatus(encrypt ? "AES-GCM encryption" : "AES-GCM authentication/decryption", status);
        return false;
    }
    if (encrypt) *tag = localTag;
    return true;
}

QByteArray SecureSession::encryptRecord(uint8_t type, const QByteArray& plaintext, QString* error) {
    if (!isEstablished()) { fail(error, "Secure session is not established"); return {}; }
    if (type == 0 || plaintext.size() > kMaxRecordPlaintext || m_txSequence == 0) {
        fail(error, "Invalid secure record"); return {};
    }
    const uint64_t sequence = m_txSequence++;
    QByteArray header;
    header.append(static_cast<char>((kMagic >> 8) & 0xFF));
    header.append(static_cast<char>(kMagic & 0xFF));
    header.append(static_cast<char>(kRecordVersion));
    header.append(static_cast<char>(m_role == Role::Initiator ? 1 : 2));
    header.append(static_cast<char>(type));
    header.append(static_cast<char>(0));
    header.append(u64be(sequence));
    header.append(u32be(static_cast<uint32_t>(plaintext.size())));
    QByteArray ciphertext;
    QByteArray tag;
    if (!aesGcm(true, m_txKey, nonceForSequence(m_txNonceBase, sequence), header,
                plaintext, &ciphertext, &tag, error)) return {};
    return header + ciphertext + tag;
}

bool SecureSession::decryptRecord(const QByteArray& record, uint8_t* type,
                                  QByteArray* plaintext, QString* error) {
    constexpr int kHeaderSize = 2 + 1 + 1 + 1 + 1 + 8 + 4;
    if (!isEstablished() || record.size() < kHeaderSize + kGcmTagBytes) {
        return fail(error, "Secure session is not established or record is truncated");
    }
    const uint16_t magic = (static_cast<uint16_t>(static_cast<uint8_t>(record[0])) << 8) |
                           static_cast<uint8_t>(record[1]);
    const uint8_t direction = static_cast<uint8_t>(record[3]);
    const uint8_t actualType = static_cast<uint8_t>(record[4]);
    const uint64_t sequence = readU64be(record.constData() + 6);
    const uint32_t length = readU32be(record.constData() + 14);
    const uint8_t expectedDirection = m_role == Role::Initiator ? 2 : 1;
    if (magic != kMagic || static_cast<uint8_t>(record[2]) != kRecordVersion || direction != expectedDirection ||
        actualType == 0 || length > static_cast<uint32_t>(kMaxRecordPlaintext) ||
        record.size() != kHeaderSize + static_cast<int>(length) + kGcmTagBytes || sequence != m_rxSequence) {
        return fail(error, "Invalid, replayed, or out-of-order secure record");
    }
    const QByteArray header = record.left(kHeaderSize);
    const QByteArray ciphertext = record.mid(kHeaderSize, static_cast<int>(length));
    const QByteArray tag = record.right(kGcmTagBytes);
    QByteArray combined = ciphertext;
    QByteArray decrypted;

    // BCryptDecrypt consumes the tag through the authenticated mode structure.
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    QByteArray object;
    ULONG objectLength = 0;
    ULONG resultLength = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (status >= 0) status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0);
    if (status >= 0) object.resize(static_cast<int>(objectLength));
    if (status >= 0) status = BCryptGenerateSymmetricKey(algorithm, &keyHandle,
        reinterpret_cast<PUCHAR>(object.data()), objectLength,
        reinterpret_cast<PUCHAR>(const_cast<char*>(m_rxKey.constData())), static_cast<ULONG>(m_rxKey.size()), 0);
    if (status >= 0) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        const QByteArray nonce = nonceForSequence(m_rxNonceBase, sequence);
        info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.constData()));
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(header.constData()));
        info.cbAuthData = static_cast<ULONG>(header.size());
        info.pbTag = reinterpret_cast<PUCHAR>(const_cast<char*>(tag.constData()));
        info.cbTag = static_cast<ULONG>(tag.size());
        decrypted.resize(ciphertext.size());
        ULONG outputLength = 0;
        status = BCryptDecrypt(keyHandle,
            reinterpret_cast<PUCHAR>(const_cast<char*>(combined.constData())), static_cast<ULONG>(combined.size()),
            &info, nullptr, 0, reinterpret_cast<PUCHAR>(decrypted.data()),
            static_cast<ULONG>(decrypted.size()), &outputLength, 0);
        decrypted.resize(static_cast<int>(outputLength));
    }
    if (keyHandle) BCryptDestroyKey(keyHandle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return fail(error, ntStatus("AES-GCM authentication/decryption", status));

    ++m_rxSequence;
    *type = actualType;
    *plaintext = decrypted;
    return true;
}

#else

SecureSession::SecureSession(Role role) : m_role(role) { m_state = State::Failed; }
SecureSession::~SecureSession() = default;
bool SecureSession::loadOrCreateIdentity(const QString&, QString* error) { return fail(error, "SecureSession is Windows-only in this milestone"); }
QByteArray SecureSession::makeHello(QString* error) { fail(error, "SecureSession is Windows-only in this milestone"); return {}; }
#endif
