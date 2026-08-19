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
        self.assertIn("Windows Phone Control", android_receiver)
        self.assertIn("Windows Wi-Fi setup", android_receiver)
        self.assertIn("Accessibility", android_receiver)
        self.assertIn("Windows BetterCast Receiver", android_sender)
        self.assertNotIn("Mac", android_sender)
        self.assertNotIn("Linux", android_sender)
        self.assertNotIn("adb forward", android_sender)

    def test_android_ui_restores_original_structure_and_windows_wording(self):
        receiver = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/ui/ReceiverScreen.kt")
        sender = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/sender/SenderScreen.kt")
        self.assertIn('text = "BetterCast"', receiver)
        self.assertIn('text = "Android Receiver"', receiver)
        self.assertIn('text = "Connect manually:"', receiver)
        self.assertIn('text = "Windows Wi-Fi setup"', receiver)
        self.assertIn('text = "Android Sender"', sender)
        self.assertIn("Windows BetterCast Receiver", sender)
        for source in (receiver, sender):
            self.assertNotIn("Mac", source)
            self.assertNotIn("macOS", source)
            self.assertNotIn("ADB Setup", source)
            self.assertNotIn("adb forward", source)

    def test_android_phone_control_is_explicit_and_bounded(self):
        manifest = self.read("Sources/BetterCastReceiverAndroid/app/src/main/AndroidManifest.xml")
        service = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/control/BetterCastAccessibilityService.kt")
        tcp = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/network/TcpClient.kt")
        xml = self.read("Sources/BetterCastReceiverAndroid/app/src/main/res/xml/bettercast_accessibility_service.xml")
        self.assertIn("BIND_ACCESSIBILITY_SERVICE", manifest)
        self.assertIn("canPerformGestures", xml)
        self.assertIn("MAX_CONTROL_BYTES", service)
        self.assertIn("dispatchGlobalAction", service)
        self.assertIn('"move"', service)
        self.assertIn("RemotePointerOverlay", service)
        self.assertIn("CONTROL_TYPE = 0x03", tcp)
        self.assertIn("encryptRecord(CONTROL_TYPE", tcp)
        self.assertIn("setSessionArmed", service)
        self.assertNotIn("canRetrieveWindowContent=\"true\"", xml)

    def test_android_control_rate_limit_is_present(self):
        tcp = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/network/TcpClient.kt")
        self.assertIn("android.os.SystemClock.elapsedRealtime()", tcp)
        self.assertIn("controlWindowStartMs", tcp)
        self.assertIn("controlCountInWindow > MAX_CONTROL_PER_SECOND", tcp)
        self.assertIn("Secure record authentication or sequence failure", tcp)
        self.assertIn("SecureSession", tcp)
        self.assertIn("approvePendingPairing", tcp)

    def test_secure_transport_is_authenticated_and_fail_closed(self):
        sender = self.read("Sources/BetterCastReceiverDesktop/sender/NetworkSender.cpp")
        sender_header = self.read("Sources/BetterCastReceiverDesktop/sender/NetworkSender.h")
        listener = self.read("Sources/BetterCastReceiverDesktop/NetworkListener.cpp")
        secure_header = self.read("Sources/BetterCastReceiverDesktop/secure/SecureSession.h")
        secure_cpp = self.read("Sources/BetterCastReceiverDesktop/secure/SecureSession.cpp")
        receiver = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/network/TcpClient.kt")
        self.assertIn("SecureSession::Role::Initiator", sender)
        self.assertIn("isSecureEstablished", sender)
        self.assertIn("pairingRequired", sender_header)
        self.assertIn("encryptRecord", sender)
        self.assertIn("decryptRecord", sender)
        self.assertIn("AES-GCM", secure_cpp)
        self.assertIn("BCryptSecretAgreement", secure_cpp)
        self.assertIn("BCryptSignHash", secure_cpp)
        self.assertIn("CryptProtectData", secure_cpp)
        self.assertIn("kSuiteAes256GcmP256", secure_header)
        self.assertIn("SecureSession", receiver)
        self.assertIn("Secure record authentication or sequence failure", receiver)
        self.assertIn("compare this code with windows", self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/ui/ReceiverScreen.kt").lower())
        self.assertNotIn("Legacy: no type byte", receiver)
        self.assertIn("secure authentication required", listener)
        self.assertIn("m_secureSessionEstablished.value(socket, false)", listener)
        self.assertIn("m_udpSocket = nullptr", listener)

    def test_secure_udp_and_android_sender_plaintext_paths_are_disabled(self):
        viewmodel = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/viewmodel/ReceiverViewModel.kt")
        android_sender = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/sender/TcpSender.kt")
        self.assertIn("UDP is intentionally disabled in secure v2 mode", viewmodel)
        self.assertIn("Android sender mode is disabled until secure v2 transport is available", android_sender)
        self.assertNotIn("Start UDP client", viewmodel)

    def test_display_suspend_resume_is_explicit_and_safe(self):
        desktop = self.read("Sources/BetterCastReceiverDesktop/MainWindow.cpp")
        android = self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/viewmodel/ReceiverViewModel.kt")
        self.assertIn("Ctrl+Alt+Shift+B", desktop)
        self.assertIn("display_suspend", desktop)
        self.assertIn("display_resume", desktop)
        self.assertIn("moveTaskToBack", self.read("Sources/BetterCastReceiverAndroid/app/src/main/java/com/bettercast/receiver/MainActivity.kt"))
        self.assertIn('"display_suspend"', android)
        self.assertIn('"display_resume"', android)
        self.assertIn("requestKeyframe", android)
        self.assertIn("ConnectionState.IDLE", android)
        self.assertIn("BetterCastAccessibilityService.setSessionArmed(false)", android)
        self.assertIn("override fun onCleared()", android)

    def test_installer_has_no_wildcard_executable_fallback(self):
        installer = self.read("Sources/BetterCastReceiverDesktop/installer.nsi")
        self.assertNotIn("FindFirst", installer)
        self.assertNotIn("try_exe", installer)
        self.assertNotIn("try_generic_inf", installer)
        self.assertIn("MttVDD.inf", installer)
        self.assertIn("profile=private", installer)

    def test_ci_downloads_are_verified_or_pinned(self):
        all_in_one = self.read(".github/workflows/build-windows-all-in-one.yml")
        self.assertIn("ffmpeg[openh264]", all_in_one)
        self.assertNotIn("ffmpeg[x264]", all_in_one)
        self.assertIn("VDD_VERSION: '25.7.23'", all_in_one)
        self.assertIn("VDD_DRIVER_SHA256:", all_in_one)
        self.assertIn("VDD_CONTROL_SHA256:", all_in_one)
        self.assertIn("VirtualDisplayDriver-x86.Driver.Only.zip", all_in_one)
        self.assertIn("VDD.Control.$env:VDD_VERSION.zip", all_in_one)
        self.assertIn("Test-Path 'vdd/MttVDD.inf'", all_in_one)
        self.assertIn("artifact/VirtualDisplayDriver", all_in_one)
        windows = self.read(".github/workflows/build-windows-receiver.yml")
        linux = self.read(".github/workflows/build-linux-receiver.yml")
        self.assertIn("Assert-Sha256", windows)
        self.assertIn("LINUXDEPLOY_VERSION", linux)
        self.assertIn("sha256sum --check", linux)
        self.assertIn("ADB_LINUX_SHA256:", linux)
        self.assertIn("d230f13842f60f782a8645f9c813f8f845bf36089ea7289f28c48f17979313f1", linux)

    def test_windows_vdd_install_requests_uac_and_creates_device(self):
        vdd = self.read("Sources/BetterCastReceiverDesktop/sender/VirtualDisplayVDD.cpp")
        self.assertIn("ShellExecuteExW", vdd)
        self.assertIn('executeInfo.lpVerb = L"runas"', vdd)
        self.assertIn("Root\\\\MttVDD", vdd)
        self.assertIn("VDD: Requesting administrator approval", vdd)
        self.assertIn("VDD: Elevated devcon created the device node", vdd)
        self.assertNotIn("QProcess proc;\n        proc.setProgram(devconExe)", vdd)


if __name__ == "__main__":
    unittest.main()
