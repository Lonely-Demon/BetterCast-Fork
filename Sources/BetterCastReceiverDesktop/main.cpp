#include <QApplication>
#include <QSurfaceFormat>
#include <QCoreApplication>
#include <QIcon>
#include <QProcess>
#include <QStandardPaths>
#include <QFile>
#include <QDebug>
#include "MainWindow.h"

#ifdef _WIN32
// Add Windows Firewall exceptions for mDNS and streaming
static void ensureFirewallRule() {
    // Check if our firewall rules already exist
    QProcess check;
    check.start("netsh", {"advfirewall", "firewall", "show", "rule", "name=BetterCast mDNS In"});
    check.waitForFinished(3000);
    QString output = QString::fromUtf8(check.readAllStandardOutput());
    if (output.contains("BetterCast mDNS In")) {
        qDebug() << "Firewall: Rules already exist";
        return;
    }

    qDebug() << "Firewall: Adding rules (requires admin)...";

    // Inbound UDP 5353 — receive mDNS queries from Mac/other devices
    QProcess addIn;
    addIn.start("netsh", {"advfirewall", "firewall", "add", "rule",
                          "name=BetterCast mDNS In",
                          "dir=in", "action=allow", "protocol=UDP",
                          "localport=5353",
                          "profile=private", "remoteip=localsubnet",
                          "description=Allow inbound mDNS for BetterCast auto-discovery"});
    addIn.waitForFinished(3000);

    // Outbound UDP 5353 — send mDNS announcements to multicast
    QProcess addOut;
    addOut.start("netsh", {"advfirewall", "firewall", "add", "rule",
                           "name=BetterCast mDNS Out",
                           "dir=out", "action=allow", "protocol=UDP",
                           "remoteport=5353",
                           "profile=private", "remoteip=localsubnet",
                           "description=Allow outbound mDNS for BetterCast auto-discovery"});
    addOut.waitForFinished(3000);

    // Inbound TCP 51820 — accept streaming connections
    QProcess addTcp;
    addTcp.start("netsh", {"advfirewall", "firewall", "add", "rule",
                            "name=BetterCast Receiver",
                            "dir=in", "action=allow", "protocol=TCP",
                            "localport=51820",
                            "profile=private", "remoteip=localsubnet",
                            "description=Allow BetterCast screen streaming"});
    addTcp.waitForFinished(3000);

    // Log firewall status — this runs before LogManager UI is set up,
    // so we store the result and MainWindow will log it later
    if (addIn.exitCode() == 0) {
        qputenv("BETTERCAST_FW_STATUS", "ok");
        qDebug() << "Firewall: Rules added successfully";
    } else {
        qputenv("BETTERCAST_FW_STATUS", "failed");
        qDebug() << "Firewall: Could not add rules (needs admin)";
    }
}
#endif

int main(int argc, char* argv[]) {
    bool softwareOpenGL = false;
#ifdef _WIN32
    // Qt must choose the software OpenGL loader before QApplication is created.
    // This is useful on legacy Intel drivers and can be enabled without changing
    // the default hardware-accelerated path: BETTERCAST_SOFTWARE_OPENGL=1 or
    // BetterCastReceiver.exe --software-opengl.
    softwareOpenGL = qEnvironmentVariableIntValue("BETTERCAST_SOFTWARE_OPENGL") == 1;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--software-opengl") {
            softwareOpenGL = true;
        }
    }
    if (softwareOpenGL) {
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
        qputenv("QT_OPENGL", "software");
        qputenv("QSG_INFO", "1");
    }
#endif

    // Use Compatibility Profile for GL_LUMINANCE/GL_LUMINANCE_ALPHA support
    // Core Profile removes these, breaking NV12 texture uploads on Windows
    QSurfaceFormat format;
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setSwapInterval(1); // VSync
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    qInfo() << "BetterCast startup: Qt" << qVersion()
            << "softwareOpenGL=" << (softwareOpenGL ? "yes" : "no");
    app.setApplicationName("BetterCast");
    app.setOrganizationName("BetterCast");
    app.setApplicationVersion("1.0.0");
    app.setWindowIcon(QIcon(":/appicon.png"));

#ifdef _WIN32
    ensureFirewallRule();
#endif

    MainWindow window;
    window.show();

    return app.exec();
}
