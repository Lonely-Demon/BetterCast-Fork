# BetterCast Connection Diagnosis

## Diagnosis

The screenshots show a **role-selection mismatch**, not evidence of a failed TCP or UDP connection.

The Windows application is on its `Receive Screen` page. Its own UI says `Listening for Senders`, and its manual connection card expects the IP address of a sender. The Android application is also on the `Receive` tab. Its screen is labeled `Android Receiver`, shows `Ready to receive`, and says to open BetterCast on a Mac and pick the phone. Therefore, both devices are receivers waiting for a third device to send; neither screenshot shows an active sender.

The Android app supports both modes. Its top toggle contains `Receive` and `Send`, and the source initializes in `RECEIVER` mode by default. The Android sender path is implemented by `SenderScreen` and `SenderViewModel`; it starts screen capture after the user taps `Start Casting`, then listens for a receiver connection. The Windows portable build is receiver-only (`ENABLE_SENDER=OFF`), so it cannot send the laptop screen, but it can receive a stream from an Android sender.

## Correct Android-to-Windows procedure

1. Start the Windows BetterCast Receiver.
2. On Android, tap **Send** at the top; do not remain on **Receive**.
3. Tap **Start Casting** and approve Android’s screen-capture permission.
4. If using Wi-Fi, enter the Android sender’s displayed IP address in the Windows receiver’s manual connection field and click **Connect**. Do not enter the Windows receiver’s own IP address.
5. If using USB, enable Developer Options and USB Debugging, connect the phone, approve the USB-debugging prompt, and click **Connect to Android (ADB)** on Windows.
6. The Windows receiver should then transition from `Listening for Senders` to a connected/video state.

For Android-to-Mac casting, the Android sender’s displayed ADB instructions can be used, but the receiving computer must be running BetterCast Receiver and connect to the forwarded local port.

## Implemented UX remediation

The fork now makes the role distinction explicit. Windows says `This PC is a receiver and is waiting for a sender`, tells the user to tap `Send` on Android, and states that the manual field requires the sender’s IP rather than the PC’s own IP. The Android Receive screen now states that the phone is in Receive mode and explains that the user must tap Send to cast the phone. Android sender text now says it can stream to a Windows, Linux, or Mac BetterCast Receiver instead of referring only to a Mac.

The change is in commit `5964bbb` on the fork’s `main` branch. Six standard-library regression tests pass, including a new sender/receiver-role regression test.

## Test artifact

The updated Windows portable build completed successfully in [GitHub Actions run 32170325127](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32170325127). Download `BetterCastReceiver-Windows-portable` from that run. It includes the software OpenGL fallback `opengl32sw.dll` from the earlier graphics remediation.

The primary remaining security limitation is unchanged: transport is still plaintext and unauthenticated, so testing should be performed only on a trusted network.
