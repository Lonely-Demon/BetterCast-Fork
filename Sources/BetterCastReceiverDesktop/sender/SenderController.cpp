#include "SenderController.h"
#include "VirtualDisplayVDD.h"
#include "VideoEncoderFF.h"
#include "NetworkSender.h"
#include <QDebug>

#ifdef _WIN32
#include "ScreenCaptureWin.h"
#endif
// TODO: #include "ScreenCaptureLinux.h" for PipeWire support

SenderController::SenderController(QObject* parent)
    : QObject(parent)
{
#ifdef _WIN32
    m_vdd = new VirtualDisplayVDD(this);
    connect(m_vdd, &VirtualDisplayVDD::statusChanged,
            this, &SenderController::statusChanged);
    connect(m_vdd, &VirtualDisplayVDD::error,
            this, &SenderController::error);
#endif
}

void SenderController::setMonitorIndex(int adapterIndex, int outputIndex) {
    m_adapterIndex = adapterIndex;
    m_outputIndex = outputIndex;
}

SenderController::~SenderController() {
    stopSending();
}

bool SenderController::startSending(const QString& receiverHost, uint16_t port,
                                     int fps, int bitrateMbps) {
    if (m_sending || m_stopQueued) {
        qWarning() << "Sender: Already sending";
        return false;
    }

    m_fps = fps;
    m_bitrateMbps = bitrateMbps;
    m_controlOnly = false;

    // Create screen capture (targeting selected monitor)
#ifdef _WIN32
    auto* cap = new ScreenCaptureWin(fps, this);
    cap->setMonitorIndex(m_adapterIndex, m_outputIndex);
    cap->setDisplayName(m_displayName);
    m_capture = cap;
#else
    emit error("Screen capture not yet supported on this platform");
    emit statusChanged("Sender not available on this platform yet");
    return false;
#endif

    m_encoder = new VideoEncoderFF(this);
    m_network = new NetworkSender(this);

    // Wire signals
    connect(m_capture, &ScreenCapture::frameCaptured,
            this, &SenderController::onFrameCaptured);
    connect(m_capture, &ScreenCapture::error,
            this, &SenderController::error);

    connect(m_encoder, &VideoEncoderFF::encoded,
            this, &SenderController::onEncoded);
    connect(m_encoder, &VideoEncoderFF::error,
            this, &SenderController::error);

    connect(m_network, &NetworkSender::connected,
            this, &SenderController::onConnected);
    connect(m_network, &NetworkSender::pairingRequired,
            this, &SenderController::pairingRequired);
    connect(m_network, &NetworkSender::disconnected,
            this, &SenderController::onDisconnected);
    connect(m_network, &NetworkSender::error,
            this, &SenderController::error);

    // Connect to receiver first
    emit statusChanged("Connecting to receiver...");
    m_network->connectTo(receiverHost, port);

    m_sending = true;
    m_encoderReady = false;
    return true;
}

bool SenderController::startControlOnly(const QString& receiverHost, uint16_t port) {
    if (m_sending || m_stopQueued) {
        qWarning() << "Sender: Already sending or controlling";
        return false;
    }

    m_network = new NetworkSender(this);
    connect(m_network, &NetworkSender::connected,
            this, &SenderController::onConnected);
    connect(m_network, &NetworkSender::pairingRequired,
            this, &SenderController::pairingRequired);
    connect(m_network, &NetworkSender::disconnected,
            this, &SenderController::onDisconnected);
    connect(m_network, &NetworkSender::error,
            this, &SenderController::error);

    m_controlOnly = true;
    m_sending = true;
    m_encoderReady = false;
    emit statusChanged("Connecting for Phone Control...");
    m_network->connectTo(receiverHost, port);
    return true;
}

bool SenderController::approvePairing() {
    return m_network && m_network->approvePairing();
}

bool SenderController::isSecureEstablished() const {
    return m_network && m_network->isSecureEstablished();
}

void SenderController::sendControlJson(const QByteArray& json) {
    if (m_network && m_network->isConnected() && !json.isEmpty() && json.size() <= 16 * 1024) {
        m_network->sendControlJson(json);
    }
}

void SenderController::stopSending() {
    if (!m_sending) {
        m_stopQueued = false;
        return;
    }

    m_stopQueued = false;
    m_sending = false;
    m_controlOnly = false;
    m_encoderReady = false;

    if (m_capture) {
        m_capture->stop();
        delete m_capture;
        m_capture = nullptr;
    }
    if (m_encoder) {
        m_encoder->shutdown();
        delete m_encoder;
        m_encoder = nullptr;
    }
    if (m_network) {
        m_network->disconnect();
        delete m_network;
        m_network = nullptr;
    }

    emit stopped();
    emit statusChanged("Sender stopped");
}

void SenderController::onConnected() {
    qDebug() << "Sender: Connected to receiver";
    emit connected();
    emit statusChanged(m_controlOnly ? "Phone Control connected" : "Connected — starting screen capture...");

    if (m_capture && !m_capture->isRunning()) {
        if (!m_capture->start()) {
            emit error("Failed to start screen capture");
            stopSending();
        }
    }
}

void SenderController::onDisconnected() {
    qDebug() << "Sender: Disconnected from receiver";
    emit disconnected();
    if (!m_sending || m_stopQueued) return;

    // NetworkSender emits disconnected from inside its socket callback. Do
    // not delete the sender, capture, or encoder from that callback stack;
    // queue teardown after all socket signal handlers have returned.
    m_stopQueued = true;
    QMetaObject::invokeMethod(this, [this]() {
        if (m_sending) stopSending();
        else m_stopQueued = false;
    }, Qt::QueuedConnection);
}

void SenderController::onFrameCaptured(const QByteArray& nv12, int width, int height) {
    if (!m_encoder || !m_sending) return;

    // Lazy-init encoder on first frame (captures real resolution)
    if (!m_encoderReady) {
        if (!m_encoder->init(width, height, m_fps, m_bitrateMbps)) {
            emit error("Failed to initialize H.264 encoder");
            stopSending();
            return;
        }
        m_encoderReady = true;
        emit statusChanged(QString("Streaming %1x%2 via %3")
                               .arg(width).arg(height).arg(m_encoder->encoderName()));
        // Force first frame to be a keyframe
        m_encoder->requestKeyframe();
    }

    m_encoder->encode(nv12, width, height);
}

void SenderController::onEncoded(const QByteArray& payload) {
    if (m_network && m_network->isConnected()) {
        m_network->sendVideo(payload);
    }
}

QString SenderController::encoderInfo() const {
    if (m_encoder) {
        return m_encoder->encoderName();
    }
    return "Not initialized";
}
