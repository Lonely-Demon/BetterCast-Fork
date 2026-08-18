from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SecurityHardeningRegressionTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text()

    def test_desktop_tcp_udp_limits_and_peer_pinning_present(self):
        cpp = self.read("Sources/BetterCastReceiverDesktop/NetworkListener.cpp")
        header = self.read("Sources/BetterCastReceiverDesktop/NetworkListener.h")
        self.assertIn("kMaxUdpFramesInFlight", header)
        self.assertIn("kMaxUdpBytesInFlight", header)
        self.assertIn("length == 0", cpp)
        self.assertIn("socket->disconnectFromHost()", cpp)
        self.assertIn("chunkId >= totalChunks", cpp)
        self.assertIn("m_udpBytesInFlight", cpp)
        self.assertIn("senderPort != m_udpPeerPort", cpp)

    def test_root_receiver_has_bounded_transport_and_no_auto_adb(self):
        root = self.read("Sources/BetterCastReceiver/BetterCastReceiverApp.swift")
        self.assertIn("maxTCPFrameLength", root)
        self.assertIn("maxUDPBytesInFlight", root)
        self.assertIn("activeTCPConnectionID", root)
        self.assertIn("invalid frame length", root)
        self.assertNotIn("self.enableWirelessADB(adb: adb", root)

    def test_swift_and_android_control_validation_present(self):
        swift = self.read("Sources/BetterCastSender/InputEvent.swift")
        swift_handler = self.read("Sources/BetterCastSender/InputHandler.swift")
        kotlin = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/input/InputEvent.kt")
        self.assertIn("isValidForTransport", swift)
        self.assertIn("event.isValidForTransport", swift_handler)
        self.assertIn("isFinite", swift)
        self.assertIn("isValid(event", kotlin)
        self.assertIn("MAX_BYTES_IN_FLIGHT", self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/network/UdpClient.kt"))

    def test_receiver_sender_roles_are_explicit(self):
        desktop = self.read("Sources/BetterCastReceiverDesktop/MainWindow.cpp")
        android_receiver = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/ui/ReceiverScreen.kt")
        android_sender = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/sender/SenderScreen.kt")
        self.assertIn("This PC is a receiver and is waiting for a sender", desktop)
        self.assertIn("Enter the sender's IP address (not this PC's address)", desktop)
        self.assertIn("tap Send at the top", desktop)
        self.assertIn("In Second Display mode it shows the Windows display", android_receiver)
        self.assertIn("Phone Control requires the user-enabled BetterCast Accessibility Service", android_receiver)
        self.assertIn("Windows, Linux, or Mac BetterCast Receiver", android_sender)
        self.assertNotIn("On your Mac, run:", android_sender)

    def test_android_phone_control_is_explicit_and_bounded(self):
        manifest = self.read("Sources/BetterCastReceiverAndroid/app/src/main/AndroidManifest.xml")
        service = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/control/BetterCastAccessibilityService.kt")
        tcp = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/network/TcpClient.kt")
        xml = self.read("Sources/BetterCastReceiverAndroid/app/src/main/res/xml/bettercast_accessibility_service.xml")
        self.assertIn("BIND_ACCESSIBILITY_SERVICE", manifest)
        self.assertIn("canPerformGestures", xml)
        self.assertIn("MAX_CONTROL_BYTES", service)
        self.assertIn("dispatchGlobalAction", service)
        self.assertIn("typeByte == 0x03", tcp)
        self.assertIn("setSessionArmed", service)
        self.assertNotIn("canRetrieveWindowContent=\"true\"", xml)

    def test_installer_has_no_wildcard_executable_fallback(self):
        installer = self.read("Sources/BetterCastReceiverDesktop/installer.nsi")
        self.assertNotIn("FindFirst", installer)
        self.assertNotIn("try_exe", installer)
        self.assertNotIn("try_generic_inf", installer)
        self.assertIn("MttVDD.inf", installer)
        self.assertIn("profile=private", installer)

    def test_ci_downloads_are_verified_or_pinned(self):
        windows = self.read(".github/workflows/build-windows-receiver.yml")
        linux = self.read(".github/workflows/build-linux-receiver.yml")
        self.assertIn("Assert-Sha256", windows)
        self.assertIn("LINUXDEPLOY_VERSION", linux)
        self.assertIn("sha256sum --check", linux)
        self.assertIn("ADB_LINUX_SHA256:", linux)
        self.assertIn("d230f13842f60f782a8645f9c813f8f845bf36089ea7289f28c48f17979313f1", linux)


if __name__ == "__main__":
    unittest.main()
