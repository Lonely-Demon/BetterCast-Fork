#include "NetworkListener.h"
#include "MainWindow.h"  // for LogManager
#include "VideoDecoder.h"
#include "VideoRenderer.h"
#include "AudioDecoder.h"

#include <QHostAddress>
#include <QtEndian>
#include <QDebug>

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
        emit statusChanged("TCP connected; secure authentication required");
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

        // The legacy auto-detection path is intentionally disabled. A raw
        // TCP peer must complete the secure v2 handshake before any body can
        // reach a decoder or control path.
        if (!m_secureSessionEstablished.value(socket, false)) {
            qWarning() << "Rejecting unauthenticated TCP frame from" << socket->peerAddress().toString();
            buffer.clear();
            socket->disconnectFromHost();
            return;
        }

        // Secure sessions do not use legacy auto-detection.
        // Type-byte format (Mac sender): [0x01=video|0x02=audio][payload]
        // Legacy format (Android/Swift): [8-byte PTS][NALUs] — first frame PTS=0 so byte[0]=0x00
        int& format = m_connectionFormat[socket];
        if (format < 0 && body.size() > 1) {
            uint8_t firstByte = static_cast<uint8_t>(body[0]);
            if (firstByte == 0x01 || firstByte == 0x02) {
                format = 1; // type-byte framing
                LogManager::instance().log("Detected type-byte framing (desktop sender)");
            } else {
                format = 0; // legacy framing
                LogManager::instance().log("Detected legacy framing (Android/Swift sender)");
            }
        }

        if (format == 1 && body.size() > 1) {
            uint8_t typeByte = static_cast<uint8_t>(body[0]);
            if (typeByte == 0x01) {
                handleVideoData(body.mid(1), false);  // type-byte framing: no PTS prefix
            } else if (typeByte == 0x02) {
                handleAudioData(body.mid(1));
            } else {
                qWarning() << "TCP framing error: unknown type — disconnecting";
                socket->disconnectFromHost();
                return;
            }
        } else if (body.size() >= 8) {
            // Legacy: has 8-byte PTS prefix
            handleVideoData(body, true);
        } else {
            qWarning() << "TCP framing error: legacy body too short — disconnecting";
            socket->disconnectFromHost();
            return;
        }
    }

    // Remove all consumed bytes at once (avoids repeated O(n) shifts)
    if (consumed > 0) {
        buffer.remove(0, consumed);
    }
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
    InputEvent heartbeat(InputEventType::Command, 0, 0, kHeartbeatKeyCode);
    QByteArray packet = heartbeat.toPacket();

    for (auto* client : m_clients) {
        client->write(packet);
    }
}

void NetworkListener::sendInputEvent(const InputEvent& event) {
    // No plaintext reverse-control path is permitted. Secure v2 receiver
    // support must set this gate only after authenticated session setup.
    if (m_clients.isEmpty() || !m_secureSessionEstablished.value(m_clients.first(), false)) return;
    bool isCritical = (event.type == InputEventType::LeftMouseDown ||
                       event.type == InputEventType::LeftMouseUp ||
                       event.type == InputEventType::RightMouseDown ||
                       event.type == InputEventType::RightMouseUp ||
                       event.type == InputEventType::KeyDown ||
                       event.type == InputEventType::KeyUp ||
                       event.type == InputEventType::Command);

    int repeatCount = isCritical ? 3 : 1;
    QByteArray packet = event.toPacket();

    for (auto* client : m_clients) {
        for (int i = 0; i < repeatCount; i++) {
            client->write(packet);
        }
    }
}
