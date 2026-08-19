#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include <QDateTime>
#include <cstdint>

#include "../secure/SecureSession.h"

// TCP client that sends video/audio data using BetterCast wire protocol.
// Format: [4B BE length][1B type (0x01=video, 0x02=audio)][payload]
class NetworkSender : public QObject {
    Q_OBJECT
public:
    explicit NetworkSender(QObject* parent = nullptr);
    ~NetworkSender() override;

    void connectTo(const QString& host, uint16_t port);
    void disconnect();
    bool isConnected() const;
    bool isSecureEstablished() const;
    bool approvePairing();

    void sendVideo(const QByteArray& payload);
    void sendAudio(const QByteArray& payload);
    void sendControlJson(const QByteArray& json);

signals:
    void connected();
    void pairingRequired(const QString& authenticationString, const QString& fingerprint);
    void disconnected();
    void error(const QString& message);

private slots:
    void onReadyRead();

private:
    void sendPacket(uint8_t type, const QByteArray& payload);
    void attemptConnect();
    bool sendFramed(const QByteArray& message);
    void processIncoming();
    void handleHandshakeMessage(const QByteArray& message);
    void handleSecureRecord(const QByteArray& record);
    QString identityPath() const;
    QString peerPath() const;
    void loadPeerTrust();
    void persistPeerTrust();

    QTcpSocket* m_socket = nullptr;
    SecureSession* m_secureSession = nullptr;
    QByteArray m_readBuffer;
    QByteArray m_pinnedPeerPublicKey;
    bool m_pairingPending = false;
    bool m_handshakeStarted = false;
    QString m_host;
    uint16_t m_port = 0;
    int m_retryCount = 0;
    static constexpr int MaxRetries = 4;
    static constexpr qint64 MaxQueuedBytes = 16 * 1024 * 1024;
    static constexpr qint64 MaxPayloadBytes = 8 * 1024 * 1024;
    QTimer m_retryTimer;
    QDateTime m_lastBackpressureLog;
};
