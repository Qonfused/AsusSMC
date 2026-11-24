# Fan monitoring and control

AsusSMC supplies fan-speed readings to VirtualSMC so macOS monitoring applications can show them in revolutions per minute (RPM). On supported laptops, its `fanctl` command also lets you select Automatic or Full Speed through the laptop's firmware.

## Reading fan speeds

After building `fanctl` with the [README instructions](../README.md#fan-control), run this command from the repository root to see fan speeds and whether mode control is available:

```sh
build/fanctl status
```

Note that AsusSMC refreshes speeds once per second, so a query immediately after a mode change can still show the previous sample. A reading of `0 RPM` means firmware reported a stopped fan, whereas `unavailable` means AsusSMC could not obtain a valid reading from your firmware. Failed reads are reported to monitoring applications as sensor errors so they cannot be mistaken for stopped fans.

`Fan control: available` indicates that you can request mode changes with `fanctl`. Speed monitoring can still work when mode control is unavailable, since reading a sensor and changing a fan's operating mode use separate firmware routines.

## Selecting a mode

In Automatic, the laptop's firmware adjusts fan speed according to temperature. Full Speed overrides that adjustment by asking firmware to run the fan at maximum speed. Firmware determines which fans respond to this command, so a laptop with two readable fans may still provide Full Speed control of only one. This interface does not offer independent fan controls or a way to choose a particular RPM.

On ZenBook Duo models with ScreenPad controls, press the Switch Windows button to toggle between Automatic and Full Speed, even with the lower panel off. AsusSMCDaemon briefly shows the selected mode on your primary display. You can also read it with `fanctl status`.

Changing the mode from Terminal requires administrator privileges. To request Full Speed, run:

```sh
sudo build/fanctl full
```

To return speed adjustment to firmware, select Automatic:

```sh
sudo build/fanctl auto
```

Both commands print status after submitting the request. The `Mode` field records the last request accepted by firmware; it does not read back the fan controller's current mode. Check RPM readings to see how the fans respond, allowing time for them to accelerate or slow down after a change.

AsusSMC treats Full Speed as a temporary override and requests Automatic at startup, on sleep and wake, during shutdown, and when the driver stops. This allows firmware to resume temperature-based control across those transitions without retaining Full Speed as a saved preference.

## Configuring fan readings

### Fan count

AsusSMC reports one fan by default. For a laptop with two fans, add `asussmcfans=2` to your boot arguments to make the second reading available. The count cannot be determined reliably from a startup reading because firmware can return the same value for a stopped fan and a fan index it does not support.

### Incorrect RPM readings

AsusSMC normally obtains RPM through the firmware's `TACH` routine. Some ASUS implementations call a lower-level routine, `RTAH`, and convert its result as though it were the time between tachometer pulses. If that result has already been converted to RPM, `TACH` converts it again, making reported speeds fall as the fans accelerate.

On firmware with this error, add `-asussmcrawfan` to your boot arguments to bypass `TACH` and read `RTAH` directly. This is appropriate only when `RTAH` returns RPM: firmware that returns a pulse interval still needs a conversion, so enabling the option there would report that interval as a speed. The method's name alone does not tell you which units it returns.

The [UX481FL firmware analysis](https://github.com/Qonfused/ASUS-ZenBook-Duo-14-UX481-Hackintosh/blob/main/docs/resources/fan-control.md#rpm-reporting) documents one affected implementation, where approximately 8,400 RPM becomes 256 RPM after a second conversion. It verifies the units by tracing the conversion inside the embedded controller and following the result into ACPI, then compares that calculation with measured fan speeds.

## Firmware and sensor interfaces

ACPI lets the operating system call routines defined in the laptop's firmware. AsusSMC uses those routines to read sensors and access ASUS's WMI interface, which provides commands for querying device capabilities and changing settings. It checks for CPU fan-control device `0x00110013` before enabling mode selection, then calls `DEVS` with `0` for Automatic or `1` for Full Speed. This follows the [Automatic/Full Speed implementation in Linux's ASUS WMI driver](https://github.com/torvalds/linux/blob/master/drivers/platform/x86/asus-wmi.c). If `TACH` is absent, AsusSMC also uses WMI to read fan speeds.

The firmware's `ECAV` routine reports whether the embedded controller (EC), which manages the fans and supplies their readings, is ready for access. With `-asussmcrawfan`, firmware must provide both `RTAH` and `ECAV` so AsusSMC can check availability before each direct read. Mode changes also check `ECAV` when that method exists, because some firmware reports success even when it skips a write to an unavailable controller. AsusSMC updates the reported mode only after firmware returns its success value of `1`.

An IOKit command gate submits mode changes to the work loop that also reads the sensors, so those firmware operations run one at a time. Monitoring applications read cached speeds through VirtualSMC, allowing the driver to answer sensor requests without waiting for ACPI.

VirtualSMC exposes readings through the System Management Controller (SMC) interface expected by macOS monitoring applications. AsusSMC provides these keys:

| SMC key | Reading |
| --- | --- |
| `FNum` | Configured fan count |
| `F0Ac`, `F1Ac` | Fan 1 and Fan 2 speeds in RPM |
| `F0ID`, `F1ID` | Fan 1 and Fan 2 names |

Speeds use SMC's `fpe2` encoding, a 16-bit unsigned format with two fractional bits. AsusSMC accepts integer readings up to 16,383 RPM and reports larger values as sensor errors because they cannot fit in that format. Mode changes use `fanctl`; this implementation does not provide SMC keys for setting target speeds in fan-control applications.
