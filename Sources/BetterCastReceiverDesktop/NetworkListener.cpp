#include "NetworkListener.h"
#include "MainWindow.h"  // for LogManager
#include "VideoDecoder.h"
#include "VideoRenderer.h"
#include "AudioDecoder.h"
#include "secure/SecureSession.h"

#include <QHostAddress>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>
#include <QDebug>

namespace {
constexpr uint8_t kAuthenticationType = 0x03;
constexpr uint8_t kConfirmationType = 0x04;
constexpr uint8_t kVideoType = 0x01;
constexpr uint8_t kAudioType = 0x02;
constexpr uint8_t kControlType = 0x03;
constexpr int kMaxHandshakeBytes = SecureSession::kMaxHandshakeMessage;
constexpr int kMaxFrameBytes = SecureSession::kMaxRecordPlaintext + 1024;
}

NetworkListener::NetworkListener(QObject* parent)
    : QObject(parent)
    , m_lastKeyframeRequest(QDateTime::fromMSecsSinceEpoch(0))
    , m_lastStatsTime(QDateTime::currentDateTime())
{
}

NetworkListener::~NetworkListener() {
    for (auto* client : m_clients) {
        client->disconnectFromHost();
    }
    qDeleteAll(m_secureSessions);
    m_secureSessions.clear();
}

void NetworkListener::setup(VideoDecoder* decoder, VideoRenderer* renderer, AudioDecoder* audioDecoder) {
    m_decoder = decoder;
    m_renderer = renderer;
    m_audioDecoder = audioDecoder;
}

uint16_t NetworkListener::actualTcpPort() const {
    if (m_tcpServer && m_tcpServer->isListening())
        return m_tcpServer->serverPort();
    return kDefaultTcpPort;
}

QString NetworkListener::identityPath() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath("identity.key");
}

QString NetworkListener::peerPath() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath("peer.identity");
}

void NetworkListener::loadPeerTrust(SecureSession* session) {
    if (!session) return;
    QFile file(peerPath());
    if (file.open(QIODevice::ReadOnly) && file.size() == 65) {
        session->setPinnedPeerPublicKey(file.readAll());
    }
}

void NetworkListener::persistPeerTrust(SecureSession* session) {
    if (!session || session->peerIdentityPublicKey().size() != 65) return;
    const QString path = peerPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(session->peerIdentityPublicKey());
        file.commit();
    }
}

bool NetworkListener::sendFramed(QTcpSocket* socket, const QByteArray& message) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState ||
        message.isEmpty() || message.size() > kMaxFrameBytes || socket->bytesToWrite() > kMaxBufferSize) {
        return false;
    }
    const uint32_t lengthBe = qToBigEndian(static_cast<uint32_t>(message.size()));
    socket->write(reinterpret_cast<const char*>(&lengthBe), 4);
    socket->write(message);
    return true;
}

bool NetworkListener::initializeSecureSession(QTcpSocket* socket, bool initiator) {
    if (!socket) return false;
    auto* session = new SecureSession(initiator ? SecureSession::Role::Initiator
                                                : SecureSession::Role::Responder);
    QString error;
    if (!session->loadOrCreateIdentity(identityPath(), &error)) {
        LogManager::instance().log(QString("Secure identity initialization failed: %1").arg(error));
        delete session;
        return false;
    }
    loadPeerTrust(session);
    m_secureSessions.insert(socket, session);
    if (initiator) {
        const QByteArray hello = session->makeHello(&error);
        if (hello.isEmpty() || !sendFramed(socket, hello)) {
            LogManager::instance().log(QString("Secure handshake start failed: %1").arg(error));
            delete m_secureSessions.take(socket);
            return false;
        }
    }
    return true;
}

void NetworkListener::start() {
    // Start TCP server
    m_tcpServer = new QTcpServer(this);
    connect(m_tcpServer, &QTcpServer::newConnection, this, &NetworkListener::onNewTcpConnection);

    if (m_tcpServer->listen(QHostAddress::Any, kDefaultTcpPort)) {
        LogManager::instance().log(QString("TCP listening on port %1").arg(m_tcpServer->serverPort()));
        emit statusChanged(QString("Listening on port %1").arg(m_tcpServer->serverPort()));
    } else {
        LogManager::instance().log(QString("TCP port %1 unavailable: %2 — trying system-assigned port")
                                       .arg(kDefaultTcpPort).arg(m_tcpServer->errorString()));
        // Try any available port if default is taken
        if (m_tcpServer->listen(QHostAddress::Any, 0)) {
            LogManager::instance().log(QString("TCP listening on fallback port %1").arg(m_tcpServer->serverPort()));
            emit statusChanged(QString("Listening on port %1 (fallback)").arg(m_tcpServer->serverPort()));
        } else {
            qWarning() << "TCP listen failed:" << m_tcpServer->errorString();
            emit statusChanged("TCP listen failed: " + m_tcpServer->errorString());
        }
    }

    // UDP is disabled in secure v2 mode. The previous channel accepted
    // unauthenticated datagrams and must not be exposed on public networks.
    m_udpSocket = nullptr;

    // Heartbeat timer (every 500ms, matching Swift receiver)
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &NetworkListener::onHeartbeatTick);
    m_heartbeatTimer->start(500);
}

void NetworkListener::disconnectAll() {
    for (auto* client : m_clients) {
        client->disconnect(); // disconnect signals
        client->abort();
        client->deleteLater();
    }
    m_clients.clear();
    m_tcpBuffers.clear();
    m_connectionFormat.clear();
    m_secureSessionEstablished.clear();
    qDeleteAll(m_secureSessions);
    m_secureSessions.clear();
    {
        QMutexLocker lock(&m_udpMutex);
        m_udpBuffer.clear();
        m_udpBytesInFlight = 0;
        m_udpPacketsSinceCleanup = 0;
        m_udpPeerSet = false;
        m_udpPeerAddress.clear();
        m_udpPeerPort = 0;
    }
    // Reset decoder so next connection starts fresh
    if (m_decoder) {
        m_decoder->reset();
    }
}

void NetworkListener::connectTo(const QString& host, uint16_t port) {
    // Disconnect any existing outgoing connections to avoid duplicates
    disconnectAll();

    auto* socket = new QTcpSocket(this);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

    connect(socket, &QTcpSocket::connected, this, [this, socket]() {
        LogManager::instance().log("Connected to " + socket->peerAddress().toString());
        m_clients.append(socket);
        m_tcpBuffers[socket] = QByteArray();
        m_connectionFormat[socket] = -1; // secure v2 handshake required
        m_secureSessionEstablished[socket] = false;
        if (!initializeSecureSession(socket, true)) {
            socket->abort();
            return;
        }
        emit statusChanged("TCP connected; secure handshake started");
    });

    connect(socket, &QTcpSocket::readyRead, this, &NetworkListener::onTcpReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &NetworkListener::onTcpDisconnected);

    emit statusChanged(QString("Connecting to %1:%2...").arg(host).arg(port));
    socket->connectToHost(host, port);
}

void NetworkListener::onNewTcpConnection() {
    while (m_tcpServer->hasPendingConnections()) {
        auto* socket = m_tcpServer->nextPendingConnection();
        if (m_clients.size() >= kMaxClients) {
            qWarning() << "Rejecting additional TCP client from" << socket->peerAddress().toString();
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

        qDebug() << "New TCP connection from" << socket->peerAddress().toString();
        m_clients.append(socket);
        m_tcpBuffers[socket] = QByteArray();
        m_connectionFormat[socket] = -1; // secure v2 handshake required
        m_secureSessionEstablished[socket] = false;
        if (!initializeSecureSession(socket, false)) {
            socket->abort();
            socket->deleteLater();
            m_clients.removeAll(socket);
            m_tcpBuffers.remove(socket);
            m_connectionFormat.remove(socket);
            m_secureSessionEstablished.remove(socket);
            continue;
        }
        if (!m_udpPeerSet) {
            m_udpPeerAddress = socket->peerAddress();
            m_udpPeerSet = true;
        }

        connect(socket, &QTcpSocket::readyRead, this, &NetworkListener::onTcpReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &NetworkListener::onTcpDisconnected);

        emit statusChanged("TCP connected; secure authentication required");
    }
}

void NetworkListener::onTcpReadyRead() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    QByteArray& buffer = m_tcpBuffers[socket];
    buffer.append(socket->readAll());

    // Safety: if buffer grows beyond 32MB, framing is likely desynced — reset
    if (buffer.size() > kMaxBufferSize) {
        qWarning() << "TCP buffer exceeded" << (kMaxBufferSize / (1024*1024))
                    << "MB — likely framing desync, resetting";
        buffer.clear();
        return;
    }

    processTcpBuffer(socket);
}

void NetworkListener::processTcpBuffer(QTcpSocket* socket) {
    QByteArray& buffer = m_tcpBuffers[socket];
    int consumed = 0;

    // Length-prefixed framing: [uint32_be length][body]
    while (buffer.size() - consumed >= 4) {
        uint32_t length = qFromBigEndian<uint32_t>(
            reinterpret_cast<const uchar*>(buffer.constData() + consumed));

        // Zero-length frames would otherwise permit a busy loop.
        if (length == 0) {
            qWarning() << "TCP framing error: zero-length packet — disconnecting";
            buffer.clear();
            socket->disconnectFromHost();
            return;
        }

        // Sanity check: single frame should never exceed 8MB.
        if (length > kMaxPacketSize) {
            qWarning() << "TCP framing error: packet length" << length
                        << "exceeds max" << kMaxPacketSize << "— disconnecting";
            buffer.clear();
            socket->disconnectFromHost();
            return;
        }

        int totalNeeded = 4 + static_cast<int>(length);
        if (buffer.size() - consumed < totalNeeded) {
            break; // Wait for more data
        }

        QByteArray body = buffer.mid(consumed + 4, static_cast<int>(length));
        consumed += totalNeeded;

        if (body.isEmpty()) {
            qWarning() << "TCP framing error: empty body — disconnecting";
            socket->disconnectFromHost();
            return;
        }

        if (!m_secureSessionEstablished.value(socket, false)) {
            if (!processSecureHandshake(socket, body)) {
                qWarning() << "Secure handshake failed for" << socket->peerAddress().toString();
                buffer.clear();
                socket->disconnectFromHost();
                return;
            }
            continue;
        }

        if (!processSecureRecord(socket, body)) {
            qWarning() << "Secure record rejected from" << socket->peerAddress().toString();
            buffer.clear();
            socket->disconnectFromHost();
            return;
        }
    }

    // Remove all consumed bytes at once (avoids repeated O(n) shifts)
    if (consumed > 0) {
        buffer.remove(0, consumed);
    }
}

bool NetworkListener::processSecureHandshake(QTcpSocket* socket, const QByteArray& message) {
    auto* session = m_secureSessions.value(socket, nullptr);
    if (!session || message.isEmpty() || message.size() > kMaxHandshakeBytes) return false;

    QString error;
    QByteArray response;
    bool ok = false;
    const bool isHello = message.size() >= 2 && message[0] == 'B' && message[1] == 'C';
    if (isHello) {
        if (session->state() == SecureSession::State::New) {
            ok = session->receiveHello(message, &response, &error);
        } else {
            ok = session->receiveHelloReply(message, &response, &error);
        }
    } else if (static_cast<uint8_t>(message[0]) == kAuthenticationType) {
        ok = session->receiveAuthentication(message, &response, &error);
        if (ok && session->role() == SecureSession::Role::Initiator &&
            session->state() == SecureSession::State::HandshakeConfirmed) {
            response = session->makeConfirmationMessage(true, &error);
            ok = !response.isEmpty();
        }
    } else if (static_cast<uint8_t>(message[0]) == kConfirmationType) {
        ok = session->receiveConfirmation(message, &response, &error);
    } else {
        error = "Unexpected secure handshake message";
    }
    if (!ok) {
        LogManager::instance().log(QString("Secure handshake failed: %1").arg(error));
        return false;
    }
    if (!response.isEmpty() && !sendFramed(socket, response)) return false;

    if (session->needsPeerApproval()) {
        if (session->hasPinnedPeer()) {
            if (!session->approvePeer(&error)) return false;
            persistPeerTrust(session);
            activateSecureSession(socket);
        } else {
            emit pairingRequired(session->shortAuthenticationString(),
                                 QString::fromLatin1(session->peerFingerprint()));
            emit statusChanged(QString("Secure pairing required; compare %1 and approve")
                               .arg(session->shortAuthenticationString()));
            LogManager::instance().log(QString("Secure pairing required; authentication code %1")
                                       .arg(session->shortAuthenticationString()));
        }
    }
    return true;
}

bool NetworkListener::processSecureRecord(QTcpSocket* socket, const QByteArray& record) {
    auto* session = m_secureSessions.value(socket, nullptr);
    if (!session || !session->isEstablished() || record.isEmpty() || record.size() > kMaxFrameBytes) {
        return false;
    }
    uint8_t type = 0;
    QByteArray payload;
    QString error;
    if (!session->decryptRecord(record, &type, &payload, &error)) {
        LogManager::instance().log(QString("Secure record authentication failed: %1").arg(error));
        return false;
    }
    if (type == kVideoType) {
        handleVideoData(payload, false);
        return true;
    }
    if (type == kAudioType) {
        handleAudioData(payload);
        return true;
    }
    if (type == kControlType && payload.size() <= 16 * 1024) {
        // Authenticated control records are currently reserved for future
        // reverse-control features; do not execute arbitrary commands.
        return true;
    }
    return false;
}

void NetworkListener::activateSecureSession(QTcpSocket* socket) {
    if (!socket || !m_secureSessions.contains(socket)) return;
    if (m_secureSessionEstablished.value(socket, false)) return;
    m_secureSessionEstablished[socket] = true;
    m_connectionFormat[socket] = 1;
    LogManager::instance().log("Secure session established");
    emit connectionEstablished();
    emit statusChanged("Secure session established");
}

bool NetworkListener::approvePairing() {
    for (auto* socket : m_clients) {
        auto* session = m_secureSessions.value(socket, nullptr);
        if (!session || !session->needsPeerApproval()) continue;
        QString error;
        if (!session->approvePeer(&error)) {
            LogManager::instance().log(QString("Secure pairing approval failed: %1").arg(error));
            return false;
        }
        persistPeerTrust(session);
        activateSecureSession(socket);
        return true;
    }
    return false;
}

void NetworkListener::handleVideoData(const QByteArray& data, bool hasPtsPrefix) {
    static int frameCount = 0;
    frameCount++;
    if (frameCount <= 5 || frameCount % 300 == 0) {
        // Log first few bytes for debugging framing issues
        QString hexPreview;
        int previewLen = qMin(data.size(), 16);
        for (int i = 0; i < previewLen; i++) {
            hexPreview += QString("%1 ").arg(static_cast<uint8_t>(data[i]), 2, 16, QChar('0'));
        }
        LogManager::instance().log(QString("Video: frame %1, %2 bytes, pts=%3 [%4]")
                                   .arg(frameCount).arg(data.size()).arg(hasPtsPrefix).arg(hexPreview.trimmed()));
    }
    if (m_decoder) {
        m_decoder->decode(data, hasPtsPrefix);
    }
}

void NetworkListener::handleAudioData(const QByteArray& data) {
    static int audioCount = 0;
    audioCount++;
    if (audioCount <= 3 || audioCount % 200 == 0) {
        qDebug() << "NetworkListener: Received audio data" << data.size() << "bytes (packet" << audioCount << ")";
    }
    if (m_audioDecoder) {
        m_audioDecoder->decode(data);
    }
}

void NetworkListener::onTcpDisconnected() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    qDebug() << "TCP client disconnected:" << socket->peerAddress().toString();
    m_clients.removeAll(socket);
    m_tcpBuffers.remove(socket);
    m_connectionFormat.remove(socket);
    m_secureSessionEstablished.remove(socket);
    delete m_secureSessions.take(socket);
    if (m_clients.isEmpty()) {
        QMutexLocker lock(&m_udpMutex);
        m_udpBuffer.clear();
        m_udpBytesInFlight = 0;
        m_udpPacketsSinceCleanup = 0;
        m_udpPeerSet = false;
        m_udpPeerAddress.clear();
        m_udpPeerPort = 0;
    }
    socket->deleteLater();

    if (m_clients.isEmpty()) {
        // Reset decoder so next connection starts fresh
        if (m_decoder) {
            m_decoder->reset();
        }
        emit connectionLost();
        emit statusChanged("Waiting for connection...");
    }
}

void NetworkListener::onUdpReadyRead() {
    while (m_udpSocket->hasPendingDatagrams()) {
        const qint64 pendingSize = m_udpSocket->pendingDatagramSize();
        if (pendingSize <= 8 || pendingSize > (kMaxUdpPayloadSize + 8)) {
            char discard = 0;
            m_udpSocket->readDatagram(&discard, 1);
            qWarning() << "Dropping invalid UDP datagram size" << pendingSize;
            continue;
        }

        QByteArray datagram;
        datagram.resize(static_cast<int>(pendingSize));
        QHostAddress senderAddress;
        uint16_t senderPort = 0;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &senderAddress, &senderPort);

        if (!datagram.isEmpty()) {
            handleUdpPacket(datagram, senderAddress, senderPort);
        }
    }
}

void NetworkListener::handleUdpPacket(const QByteArray& data, const QHostAddress& senderAddress, uint16_t senderPort) {
    if (data.size() <= 8 || data.size() > kMaxUdpPayloadSize + 8) return;

    const uchar* raw = reinterpret_cast<const uchar*>(data.constData());
    uint32_t frameId = qFromBigEndian<uint32_t>(raw);
    uint16_t chunkId = qFromBigEndian<uint16_t>(raw + 4);
    uint16_t totalChunks = qFromBigEndian<uint16_t>(raw + 6);

    QByteArray payload = data.mid(8);

    QMutexLocker lock(&m_udpMutex);

    if (m_udpPeerSet && (senderAddress != m_udpPeerAddress ||
                          (m_udpPeerPort != 0 && senderPort != m_udpPeerPort))) {
        return;
    }
    if (!m_udpPeerSet) {
        m_udpPeerAddress = senderAddress;
        m_udpPeerPort = senderPort;
        m_udpPeerSet = true;
    }

    if (totalChunks == 0 || totalChunks > kMaxUdpChunks || chunkId >= totalChunks) {
        qWarning() << "Dropping invalid UDP chunk metadata" << chunkId << totalChunks;
        return;
    }

    if (m_lastDecodedFrameId == 0) {
        m_lastDecodedFrameId = frameId - 1;
    }

    m_udpPacketsReceived++;

    // Stats logging every 3 seconds
    auto now = QDateTime::currentDateTime();
    if (m_lastStatsTime.msecsTo(now) > 3000) {
        qDebug() << "UDP Stats (3s): Pkts:" << m_udpPacketsReceived
                 << "Frames:" << m_udpFramesReassembled;
        m_udpPacketsReceived = 0;
        m_udpFramesReassembled = 0;
        m_lastStatsTime = now;
    }

    if (!m_udpBuffer.contains(frameId)) {
        if (m_udpBuffer.size() >= kMaxUdpFramesInFlight ||
            m_udpBytesInFlight + payload.size() > kMaxUdpBytesInFlight) {
            qWarning() << "Dropping UDP frame: reassembly budget exhausted";
            return;
        }
        UdpFrameEntry entry;
        entry.totalChunks = totalChunks;
        entry.timestamp = now;
        m_udpBuffer[frameId] = entry;
    }

    UdpFrameEntry& entry = m_udpBuffer[frameId];
    if (entry.totalChunks != totalChunks) {
        qWarning() << "Dropping UDP frame with inconsistent chunk count";
        m_udpBytesInFlight -= entry.totalBytes;
        m_udpBuffer.remove(frameId);
        return;
    }

    if (!entry.chunks.contains(chunkId)) {
        if (m_udpBytesInFlight + payload.size() > kMaxUdpBytesInFlight) return;
        entry.totalBytes += payload.size();
        m_udpBytesInFlight += payload.size();
        entry.chunks.insert(chunkId, payload);
    }

    bool complete = entry.chunks.size() == entry.totalChunks;
    if (complete) {
        for (uint16_t expected = 0; expected < totalChunks; ++expected) {
            if (!entry.chunks.contains(expected)) {
                complete = false;
                break;
            }
        }
    }

    if (complete) {
        m_udpFramesReassembled++;

        // Gap detection — request IDR if frames were skipped
        int diff = static_cast<int>(frameId) - static_cast<int>(m_lastDecodedFrameId);
        if (diff > 1 && diff < 1000) {
            if (m_lastKeyframeRequest.msecsTo(now) > 2000) {
                qDebug() << "Frame gap detected" << m_lastDecodedFrameId << "->" << frameId << "requesting IDR";
                sendInputEvent(InputEvent(InputEventType::Command, 0, 0, kIDRRequestKeyCode));
                m_lastKeyframeRequest = now;
            }
        }
        m_lastDecodedFrameId = frameId;

        // Reassemble in validated chunk order.
        QByteArray fullData;
        fullData.reserve(entry.totalBytes);
        for (uint16_t expected = 0; expected < totalChunks; ++expected) {
            fullData.append(entry.chunks.value(expected));
        }

        m_udpBytesInFlight -= entry.totalBytes;
        m_udpBuffer.remove(frameId);

        // Unlock before decode (decode may be slow)
        lock.unlock();
        handleVideoData(fullData);
        return;
    }

    // Periodic cleanup of stale incomplete frames.
    if (++m_udpPacketsSinceCleanup >= 100) {
        QList<uint32_t> staleKeys;
        for (auto it = m_udpBuffer.cbegin(); it != m_udpBuffer.cend(); ++it) {
            if (it->timestamp.msecsTo(now) > 1000) {
                staleKeys.append(it.key());
            }
        }
        for (uint32_t key : staleKeys) {
            m_udpBytesInFlight -= m_udpBuffer.value(key).totalBytes;
            m_udpBuffer.remove(key);
        }
        m_udpPacketsSinceCleanup = 0;
    }
}

void NetworkListener::onHeartbeatTick() {
    sendInputEvent(InputEvent(InputEventType::Command, 0, 0, kHeartbeatKeyCode));
}

void NetworkListener::sendInputEvent(const InputEvent& event) {
    const QByteArray payload = event.toJson();
    if (payload.isEmpty() || payload.size() > 16 * 1024) return;

    const bool isCritical = (event.type == InputEventType::LeftMouseDown ||
                             event.type == InputEventType::LeftMouseUp ||
                             event.type == InputEventType::RightMouseDown ||
                             event.type == InputEventType::RightMouseUp ||
                             event.type == InputEventType::KeyDown ||
                             event.type == InputEventType::KeyUp ||
                             event.type == InputEventType::Command);
    const int repeatCount = isCritical ? 3 : 1;

    for (auto* client : m_clients) {
        auto* session = m_secureSessions.value(client, nullptr);
        if (!session || !m_secureSessionEstablished.value(client, false)) continue;
        for (int i = 0; i < repeatCount; i++) {
            QString error;
            const QByteArray record = session->encryptRecord(kControlType, payload, &error);
            if (record.isEmpty() || !sendFramed(client, record)) {
                LogManager::instance().log(QString("Secure control send failed: %1").arg(error));
                break;
            }
        }
    }
}
