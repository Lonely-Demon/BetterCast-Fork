#pragma once

#include <QObject>
#include <QTimer>

class SenderController;

// First Phone Control milestone. The controller keeps the native Android UI
// visible and sends bounded pointer gestures over the existing sender session.
// A later milestone will replace explicit activation with true edge handoff and
// add negotiated keyboard/text capabilities.
class PhoneControlController : public QObject {
    Q_OBJECT
public:
    explicit PhoneControlController(SenderController* sender, QObject* parent = nullptr);

    bool start();
    void stop();
    bool isActive() const { return m_active; }

signals:
    void statusChanged(const QString& status);
    void error(const QString& message);

private slots:
    void pollPointer();

private:
    void sendMove();
    void sendTap();

    SenderController* m_sender = nullptr;
    QTimer m_timer;
    bool m_active = false;
    bool m_leftButtonDown = false;
    double m_x = 0.5;
    double m_y = 0.5;
    int m_screenWidth = 0;
    int m_screenHeight = 0;
    int m_anchorX = 0;
    int m_anchorY = 0;
};
