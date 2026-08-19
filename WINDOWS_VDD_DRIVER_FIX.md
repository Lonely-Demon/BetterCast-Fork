# Windows Virtual Display Driver Fix

## Diagnosis

The user’s Windows log showed that BetterCast could find the VDD files but could not find an installed Windows device/service:

> `VDD: Driver files found but driver not loaded in Windows — attempting install...`
>
> `VDD driver files exist but the driver isn't installed in Windows.`

The Android screenshot showing **Waiting for sender…** is an expected idle state. Windows failed before the sender could complete its normal connection and capture startup sequence.

The defect had two parts. First, the all-in-one Windows workflow packaged the application and DLLs but did not bundle the verified VDD driver/control payload. The receiver-only workflow had VDD download and packaging logic, but the sender-enabled all-in-one workflow did not. Second, the runtime attempted `devcon` and `pnputil` through a normal unelevated child process. Installing a root indirect-display device requires administrator approval; a normal `QProcess` invocation could therefore fail even when the files were present.

## Fix implemented

The all-in-one workflow now downloads the upstream Virtual Display Driver 25.7.23 driver-only package and VDD Control package, verifies both with pinned SHA-256 values, verifies that `MttVDD.inf`, `VDD Control.exe`, and `devcon.exe` exist, and places the complete payload in `VirtualDisplayDriver/` inside the Windows artifact.

The runtime now uses Windows `ShellExecuteExW` with the `runas` verb to request an explicit UAC approval before invoking the bundled signed `devcon.exe`. It then polls the actual present Display-class device and Config Manager problem code because the UMDF package may not expose a service key named `MttVDD`. If the device is still not visible, it uses elevated `pnputil /add-driver ... /install`, interprets Microsoft’s documented exit code 259 as “no matching device or a better driver is already selected” rather than asynchronous success, waits again, and retries the explicitly identified `Root\\MttVDD` device with the verified `devcon.exe`. The application no longer silently attempts an unelevated driver install. If the UAC prompt is cancelled, the UI reports that the prompt must be approved and the user can retry Create Virtual Display.

## Validated build

The corrected Windows all-in-one build completed successfully:

- [GitHub Actions run 32285401293](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32285401293)
- Artifact: `BetterCast-Windows-Android-All-in-One`
- Artifact ID: `9378403398`
- Artifact size: 138,735,060 bytes
- Code commit built: `937ddf2e324daaaf5ae351b73b5ef46fb25db78f`
- Downloaded ZIP SHA-256: `8ff1cd958a438d02f29ed4ec66f2dedac5def497af9b32836a238c9938df32f0`
- Downloaded `BetterCast.exe` SHA-256: `08f91d0f3ea36a4caaaf6f27dfcc0c0ac75c8c9f154076a00d46903670e52666`
- VDD driver package SHA-256: `e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a`
- VDD Control package SHA-256: `a701f2272e9fcf382849b24f913c6dd07597b3b1116525f2e90182f019609154`

## Black-box test sequence

Download the corrected artifact from the Actions run and extract it. Start `BetterCast.exe`, open **Send Screen**, and click **Create Virtual Display**. Windows should display a UAC prompt. Approve it. The first installation may take several seconds while Windows creates and starts the UMDF device; the corrected build waits for delayed visibility and performs a verified retry rather than immediately failing.

The expected successful logs should include messages similar to `Requesting administrator approval to install via devcon`, `Driver became visible after devcon installation`, or `Driver became visible after the post-pnputil devcon retry`. If PnPUtil reports exit code 259, the log now explains that this means no matching device was updated or a better driver is already selected; it is not treated as success unless the actual device becomes ready. The monitor list should then contain a virtual display.

The corrected sender accepts either a plain Android address such as `192.168.29.14` or an endpoint such as `192.168.29.14:51820`; the previous build passed the combined string to DNS and produced `Host not found`. If mDNS remains blocked by the Wi-Fi network, use the manual endpoint field.

On Android, **Waiting for sender…** should change to a connected state after Windows completes the driver step and starts the sender. If the UAC prompt is cancelled or blocked by policy, restart BetterCast and approve the prompt. If installation still fails after approval, preserve the new logs, especially the elevated `devcon` and `pnputil` exit codes; those will distinguish a policy/signature issue from a device-enumeration issue.

The package contains a signed third-party VDD driver. BetterCast verifies the downloaded release archives during CI, but Windows remains responsible for its own driver-signature and administrator-consent decisions. The Windows firewall rules now use all network profiles but remain restricted to `remoteip=localsubnet`; authenticated secure transport remains mandatory and no plaintext fallback is enabled.

Microsoft documents PnPUtil exit code 259 as `ERROR_NO_MORE_ITEMS`: no devices match the supplied driver or the target device already has a better/newer driver. See [PnPUtil Return Values](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-return-values) and [PnPUtil Command Syntax](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax).
