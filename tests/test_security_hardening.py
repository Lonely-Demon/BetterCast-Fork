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
        self.assertIn("BETTERCAST_ADB_LINUX_SHA256", linux)


if __name__ == "__main__":
    unittest.main()
