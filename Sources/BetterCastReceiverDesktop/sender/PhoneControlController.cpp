#include "PhoneControlController.h"
#include "SenderController.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>

#ifdef _WIN32
#include <windows.h>
#endif

PhoneControlController::PhoneControlController(SenderController* sender, QObject* parent)
    : QObject(parent), m_sender(sender)
{
    m_timer.setInterval(16); // approximately 60Hz pointer updates
    connect(&m_timer, &QTimer::timeout, this, &PhoneControlController::pollPointer);
}

bool PhoneControlController::start() {
    if (m_active) return true;
    if (!m_sender || !m_sender->isSending()) {
        emit error("Start a connected Phone Control session first");
        return false;
    }

#ifdef _WIN32
    m_screenWidth = GetSystemMetrics(SM_CXSCREEN);
    m_screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (m_screenWidth <= 0 || m_screenHeight <= 0) {
        emit error("Unable to determine the Windows screen bounds");
        return false;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        emit error("Unable to read the Windows pointer position");
        return false;
    }
    m_anchorX = m_screenWidth / 2;
    m_anchorY = m_screenHeight / 2;
    m_x = 0.5;
    m_y = 0.5;
    m_leftButtonDown = false;
    m_active = true;
    SetCursorPos(m_anchorX, m_anchorY);
    m_timer.start();
    emit statusChanged("Phone Control active — press Escape to stop");
    return true;
#else
    emit error("Phone Control is currently supported on Windows only");
    return false;
#endif
}

void PhoneControlController::stop() {
    if (!m_active) return;
    m_active = false;
    m_timer.stop();
    m_leftButtonDown = false;
#ifdef _WIN32
    SetCursorPos(m_anchorX, m_anchorY);
#endif
    emit statusChanged("Phone Control stopped");
}

void PhoneControlController::pollPointer() {
#ifdef _WIN32
    if (!m_active || !m_sender || !m_sender->isSending()) {
        stop();
        return;
    }

    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        stop();
        return;
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const int dx = cursor.x - m_anchorX;
    const int dy = cursor.y - m_anchorY;

    if (dx != 0 || dy != 0) {
        m_x = qBound(0.0, m_x + static_cast<double>(dx) / static_cast<double>(m_screenWidth), 1.0);
        m_y = qBound(0.0, m_y + static_cast<double>(dy) / static_cast<double>(m_screenHeight), 1.0);
        sendMove();
        SetCursorPos(m_anchorX, m_anchorY);
    }

    const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (leftDown && !m_leftButtonDown) sendTap();
    m_leftButtonDown = leftDown;
#endif
}

void PhoneControlController::sendMove() {
    if (!m_sender) return;
    QJsonObject object;
    object[QStringLiteral("command")] = QStringLiteral("move");
    object["x"] = m_x;
    object["y"] = m_y;
    m_sender->sendControlJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void PhoneControlController::sendTap() {
    if (!m_sender) return;
    QJsonObject object;
    object[QStringLiteral("command")] = QStringLiteral("tap");
    object["x"] = m_x;
    object["y"] = m_y;
    m_sender->sendControlJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
