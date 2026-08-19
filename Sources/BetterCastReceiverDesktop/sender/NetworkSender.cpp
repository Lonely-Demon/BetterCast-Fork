#include "NetworkSender.h"

#include "../MainWindow.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>
#include <cstring>

namespace {
constexpr int kMaxFrameBytes = 8 * 1024 * 1024 + 1024;
constexpr int kMaxHandshakeBytes = 512;
constexpr uint8_t kControlType = 0x03;
constexpr uint8_t kAuthenticationType = 0x03;
constexpr uint8_t kConfirmationType = 0x04;
}

NetworkSender::NetworkSender(QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_secureSession(new SecureSession(SecureSession::Role::Initiator))
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &NetworkSender::attemptConnect);
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkSender::onReadyRead);

    connect(m_socket, &QTcpSocket::connected, this, [this]() {
        m_retryCount = 0;
        m_readBuffer.clear();
        loadPeerTrust();
        QString secureError;
        if (!m_secureSession->loadOrCreateIdentity(identityPath(), &secureError)) {
            emit error(QString("Secure identity initialization failed: %1").arg(secureError));
            m_socket->abort();
            return;
        }
        m_secureSession->setPinnedPeerPublicKey(m_pinnedPeerPublicKey);
        m_handshakeStarted = false;
        const QByteArray hello = m_secureSession->makeHello(&secureError);

        if (hello.isEmpty() || !sendFramed(hello)) {
            m_handshakeStarted = false;
            emit error(QString("Secure handshake start failed: %1").arg(secureError));
            m_socket->abort();
            return;
        }
        m_handshakeStarted = true;
        LogManager::instance().log("Sender: TCP connected; secure handshake started");
    });

    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        const bool closedDuringHandshake = m_handshakeStarted && !m_pairingPending;
        if (closedDuringHandshake) {
            LogManager::instance().log("Sender: Remote closed during secure handshake; Android may have rejected the peer identity or transcript");
        }
        m_pairingPending = false;
        m_handshakeStarted = false;
        m_readBuffer.clear();
        delete m_secureSession;
        m_secureSession = new SecureSession(SecureSession::Role::Initiator);
        qDebug() << "Sender: TCP disconnected";
        emit disconnected();
    });

    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        if (err == QAbstractSocket::ConnectionRefusedError && m_retryCount < MaxRetries) {
            m_retryCount++;
            const int delayMs = m_retryCount * 1000;
            LogManager::instance().log(QString("Sender: Connection refused, retry %1/%2 in %3s...")
                .arg(m_retryCount).arg(MaxRetries).arg(delayMs / 1000));
            m_retryTimer.start(delayMs);
            return;
        }
        const QString errMsg = m_socket->errorString();
        qWarning() << "Sender: TCP error:" << errMsg;
        LogManager::instance().log(QString("Sender error: %1").arg(errMsg));
        emit error(errMsg);
    });
}

NetworkSender::~NetworkSender() {
    disconnect();
    delete m_secureSession;
    m_secureSession = nullptr;
}

QString NetworkSender::identityPath() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath("identity.key");
}

QString NetworkSender::peerPath() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath("peer.identity");
}

void NetworkSender::loadPeerTrust() {
    QFile file(peerPath());
    if (file.open(QIODevice::ReadOnly) && file.size() == 65) m_pinnedPeerPublicKey = file.readAll();
}

void NetworkSender::persistPeerTrust() {
    if (!m_secureSession || m_secureSession->peerIdentityPublicKey().size() != 65) return;
    const QString path = peerPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(m_secureSession->peerIdentityPublicKey());
        file.commit();
    }
}

void NetworkSender::connectTo(const QString& host, uint16_t port) {
    m_host = host;
    m_port = port;
    m_retryCount = 0;
    attemptConnect();
}

void NetworkSender::attemptConnect() {
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->abort();
    LogManager::instance().log(QString("Sender: Connecting to %1:%2").arg(m_host).arg(m_port));
    m_socket->connectToHost(m_host, m_port);
}

void NetworkSender::disconnect() {
    m_retryTimer.stop();
    m_retryCount = MaxRetries;
    m_pairingPending = false;
    m_handshakeStarted = false;
    m_readBuffer.clear();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->abort();
    delete m_secureSession;
    m_secureSession = new SecureSession(SecureSession::Role::Initiator);
}

bool NetworkSender::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

bool NetworkSender::isSecureEstablished() const {
    return isConnected() && m_secureSession && m_secureSession->isEstablished();
}

bool NetworkSender::sendFramed(const QByteArray& message) {
    if (!isConnected() || message.isEmpty() || message.size() > kMaxFrameBytes) return false;
    const uint32_t length = static_cast<uint32_t>(message.size());
    const uint32_t lengthBe = qToBigEndian(length);
    if (m_socket->bytesToWrite() > MaxQueuedBytes) return false;
    m_socket->write(reinterpret_cast<const char*>(&lengthBe), 4);
    m_socket->write(message);
    return true;
}

void NetworkSender::onReadyRead() {
    m_readBuffer.append(m_socket->readAll());
    if (m_readBuffer.size() > 32 * 1024 * 1024) {
        emit error("Secure transport buffer exceeded limit");
        m_socket->abort();
        return;
    }
    processIncoming();
}

void NetworkSender::processIncoming() {
    while (m_readBuffer.size() >= 4) {
        const uint32_t length = qFromBigEndian<uint32_t>(reinterpret_cast<const uchar*>(m_readBuffer.constData()));
        if (length == 0 || length > static_cast<uint32_t>(kMaxFrameBytes)) {
            emit error("Invalid secure transport frame length");
            m_socket->abort();
            return;
        }
        const int total = 4 + static_cast<int>(length);
        if (m_readBuffer.size() < total) return;
        const QByteArray message = m_readBuffer.mid(4, static_cast<int>(length));
        m_readBuffer.remove(0, total);
        if (m_secureSession && m_secureSession->isEstablished()) {
            handleSecureRecord(message);
        } else if (message.size() >= 2 && message[0] == 'B' && message[1] == 'C') {
            handleHandshakeMessage(message);
        } else if (message.size() >= 1 &&
                   (static_cast<uint8_t>(message[0]) == kAuthenticationType ||
                    static_cast<uint8_t>(message[0]) == kConfirmationType)) {
            handleHandshakeMessage(message);
        } else {
            emit error("Unexpected plaintext or malformed secure handshake message");
            m_socket->abort();
            return;
        }
        if (m_socket->state() == QAbstractSocket::UnconnectedState) return;
    }
}

void NetworkSender::handleHandshakeMessage(const QByteArray& message) {
    QString secureError;
    QByteArray response;
    const uint8_t type = static_cast<uint8_t>(message[0]);
    bool ok = false;
    if (message.size() >= 2 && message[0] == 'B' && message[1] == 'C') {
        ok = m_secureSession->receiveHelloReply(message, &response, &secureError);
    } else if (type == kAuthenticationType) {
        ok = m_secureSession->receiveAuthentication(message, &response, &secureError);
        if (ok && m_secureSession->state() == SecureSession::State::HandshakeConfirmed) {
            response = m_secureSession->makeConfirmationMessage(true, &secureError);
        }
    } else if (type == kConfirmationType) {
        ok = m_secureSession->receiveConfirmation(message, &response, &secureError);
        if (ok && m_secureSession->needsPeerApproval()) {
            if (m_secureSession->hasPinnedPeer()) {
                ok = approvePairing();
            } else {
                m_pairingPending = true;
                emit pairingRequired(m_secureSession->shortAuthenticationString(),
                                     QString::fromLatin1(m_secureSession->peerFingerprint()));
                LogManager::instance().log(QString("Secure pairing required; compare code %1")
                                           .arg(m_secureSession->shortAuthenticationString()));
            }
        }
    } else {
        ok = false;
        secureError = "Unexpected secure handshake message";
    }
    if (!ok) {
        emit error(secureError.isEmpty() ? "Secure handshake failed" : secureError);
        m_socket->abort();
        return;
    }
    if (!response.isEmpty() && !sendFramed(response)) {
        emit error("Unable to send secure handshake response");
        m_socket->abort();
        return;
    }
}

void NetworkSender::handleSecureRecord(const QByteArray& record) {
    if (!m_secureSession || !m_secureSession->isEstablished()) {
        emit error("Received application data before secure session establishment");
        m_socket->abort();
        return;
    }
    uint8_t type = 0;
    QByteArray payload;
    QString secureError;
    if (!m_secureSession->decryptRecord(record, &type, &payload, &secureError)) {
        emit error(secureError.isEmpty() ? "Secure record authentication failed" : secureError);
        m_socket->abort();
        return;
    }
    if (type == kControlType && payload.size() <= 16 * 1024) {
        // Android input/keyframe events are authenticated here. The existing
        // sender orchestration handles the media path; unsupported control
        // commands are intentionally ignored rather than executed implicitly.
        return;
    }
    emit error("Unexpected secure record type");
    m_socket->abort();
}

bool NetworkSender::approvePairing() {
    if (!m_secureSession || !m_secureSession->approvePeer()) return false;
    persistPeerTrust();
    m_pairingPending = false;
    m_handshakeStarted = false;
    emit connected();
    LogManager::instance().log("Sender: secure session established");
    return true;
}

void NetworkSender::sendPacket(uint8_t type, const QByteArray& payload) {
    if (!isSecureEstablished() || payload.isEmpty()) return;
    if (payload.size() > MaxPayloadBytes) {
        qWarning() << "Sender: dropping oversized payload" << payload.size();
        return;
    }
    if (m_socket->bytesToWrite() > MaxQueuedBytes) {
        const auto now = QDateTime::currentDateTime();
        if (!m_lastBackpressureLog.isValid() || m_lastBackpressureLog.msecsTo(now) > 2000) {
            qWarning() << "Sender: socket queue exceeds" << MaxQueuedBytes << "bytes; dropping packet";
            m_lastBackpressureLog = now;
        }
        return;
    }
    QString secureError;
    const QByteArray record = m_secureSession->encryptRecord(type, payload, &secureError);
    if (record.isEmpty()) {
        emit error(secureError.isEmpty() ? "Secure record encryption failed" : secureError);
        return;
    }
    if (!sendFramed(record)) emit error("Unable to queue secure record");
}

void NetworkSender::sendVideo(const QByteArray& payload) { sendPacket(0x01, payload); }
void NetworkSender::sendAudio(const QByteArray& payload) { sendPacket(0x02, payload); }

void NetworkSender::sendControlJson(const QByteArray& json) {
    if (json.isEmpty() || json.size() > 16 * 1024) return;
    sendPacket(kControlType, json);
}
