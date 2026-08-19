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

The runtime now uses Windows `ShellExecuteExW` with the `runas` verb to request an explicit UAC approval before invoking the bundled signed `devcon.exe`. If the device is still not visible, it uses an elevated `pnputil /add-driver ... /install` fallback and retries elevated device creation. The application no longer silently attempts an unelevated driver install. If the UAC prompt is cancelled, the UI reports that the prompt must be approved and the user can retry Create Virtual Display.

## Validated build

The corrected Windows all-in-one build completed successfully:

- [GitHub Actions run 32210739218](https://github.com/Lonely-Demon/BetterCast-Fork/actions/runs/32210739218)
- Artifact: `BetterCast-Windows-Android-All-in-One`
- Artifact ID: `9351213529`
- Artifact size: approximately 138.7 MB
- Code commit built: `5a281df13ce5471e32c500a042cf3cde8da88781`
- VDD driver package SHA-256: `e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a`
- VDD Control package SHA-256: `a701f2272e9fcf382849b24f913c6dd07597b3b1116525f2e90182f019609154`

## Black-box test sequence

Download the artifact from the Actions run and extract it. Start `BetterCast.exe`, open **Send Screen**, and click **Create Virtual Display**. Windows should display a UAC prompt. Approve it. The first installation may take several seconds while Windows creates the device node. After the operation completes, click **Create Virtual Display** again only if the UI has not refreshed automatically.

The expected successful logs should include messages similar to `Requesting administrator approval to install via devcon`, `Elevated devcon created the device node`, and `Driver installed successfully`. The monitor list should then contain a virtual display. Select that display, enter the Android receiver IP if mDNS discovery does not populate it, and start sending.

On Android, **Waiting for sender…** should change to a connected state after Windows completes the driver step and starts the sender. If the UAC prompt is cancelled or blocked by policy, restart BetterCast and approve the prompt. If installation still fails after approval, preserve the new logs, especially the elevated `devcon` and `pnputil` exit codes; those will distinguish a policy/signature issue from a device-enumeration issue.

The package contains a signed third-party VDD driver. BetterCast verifies the downloaded release archives during CI, but Windows remains responsible for its own driver-signature and administrator-consent decisions.
