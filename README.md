# AsusSMC

[![Github release](https://img.shields.io/github/release/Qonfused/AsusSMC.svg?color=blue)](https://github.com/Qonfused/AsusSMC/releases/latest)
[![Github downloads](https://img.shields.io/github/downloads/hieplpvip/AsusSMC/total.svg?color=blue)](https://github.com/Qonfused/AsusSMC/releases)
[![Build and release](https://github.com/Qonfused/AsusSMC/actions/workflows/build.yml/badge.svg)](https://github.com/Qonfused/AsusSMC/actions/workflows/build.yml)
[![Gitter chat](https://img.shields.io/gitter/room/nwjs/nw.js.svg?colorB=ed1965)](https://gitter.im/hieplpvip/AsusSMC)
[![Donate with PayPal](https://img.shields.io/badge/paypal-donate-red.svg)](https://paypal.me/lebhiep)

A VirtualSMC plugin provides native support for ALS, keyboard backlight and Fn keys for Asus laptops on macOS.

#### Features

- Full Fn keys support
- Native ALS support
- Native keyboard backlight support (16 levels, smooth transition, auto adjusting, auto turning off)
- ScreenPad Plus brightness and power controls
- Battery Health Charging
- Fan speed monitoring and Automatic/Full Speed selection

#### Requirements

- Asus laptop with ATK device
- Knowing how to patch DSDT (if not, read [this](https://www.tonymacx86.com/threads/guide-patching-laptop-dsdt-ssdts.152573/))

#### Boot arguments

- Add `-asussmcdbg` to enable debug printing (available in DEBUG binaries).
- Add `asussmcfans=2` to publish readings for two fans. The default is one fan.
- Add `-asussmcrawfan` to read RPM directly from `RTAH` on firmware that already converts tachometer counts to RPM.

#### How to install

- Instruction is available in the Wiki.

#### Release packages

Pushing a tag such as `v1.4.2` sets the kext version to `1.4.2` and publishes both DEBUG and RELEASE ZIPs. Each archive contains `AsusSMC.kext` for OpenCore, the matching daemon and installer for pointer-based ScreenPad brightness, and `fanctl` for fan monitoring and control. Pull requests and pushes to `master` build the same packages as GitHub Actions artifacts.

#### ScreenPad Plus

On ZenBook Duo models with ScreenPad controls in ASUS WMI, the Switch Windows button cycles the lower display through five brightness steps and reverses direction at either end. The adjacent ScreenPad button turns the display off and restores its brightness when you turn it back on. AsusSMC remembers the brightness you choose across sleep and restart. It restores that level before login if the ScreenPad is on, or when you next turn it on. Until you choose a level, firmware supplies the startup brightness.

With AsusSMCDaemon running, move the pointer onto the ScreenPad and press the usual brightness keys (F4/F5 on the UX481) to dim or brighten it in 16 steps. The brightness indicator appears on the lower panel. Moving the pointer back to the primary display returns those keys to macOS. Before login or when the displays are mirrored, macOS handles them as usual.

Install the matching AsusSMCDaemon to enable pointer-based brightness control. The dedicated Switch Windows and ScreenPad buttons also work before login. If your EFI handles these buttons in ACPI, restore the firmware’s original `_Q31` and `_Q32` methods so AsusSMC can receive their notifications.

AsusSMC handles Fn Lock when ASUS WMI reports that BIOS permits it. On ScreenPad models with Fn Lock support, AsusSMC starts in unlocked mode so the brightness keys work without holding Fn. The first Fn+Esc locks the function row. Further presses toggle it, and AsusSMC restores the last selection after sleep. On keyboards with a single illumination key, AsusSMC uses the existing `SKBV` brightness control and reverses direction at either end of the macOS brightness range.

To use the MyASUS key (Fn+F12 on the UX481) to switch between 80% and full charging, declare `Name (BKEY, One)` under `\_SB.ATKD`. This binding requires firmware charge-limit support.

#### Fan control

AsusSMC can select Automatic or Full Speed when firmware provides the ASUS WMI CPU fan-control interface. Automatic lets firmware adjust speed according to temperature, while Full Speed requests maximum speed. Which fans respond depends on the firmware implementation.

From an extracted release, run `./fanctl status` to read fan speeds. To build the tool from source, run:

```sh
clang -std=c11 -Wall -Wextra -Werror -mmacosx-version-min=10.11 \
  Tools/fanctl.c -framework IOKit -framework CoreFoundation -o build/fanctl
```

Read fan speeds and the last mode accepted by firmware, or change the mode as an administrator:

```sh
build/fanctl status
sudo build/fanctl full
sudo build/fanctl auto
```

AsusSMC requests Automatic during startup, sleep, wake, and shutdown. See [Fan monitoring and control](docs/fan-control.md) for firmware requirements, RPM configuration, and hardware validation.

#### Credits

- [Apple](https://www.apple.com) for macOS
- [@vit9696](https://github.com/vit9696) for [Lilu](https://github.com/acidanthera/Lilu) and [VirtualSMC](https://github.com/acidanthera/VirtualSMC)
- [@lvs1974](https://github.com/lvs1974) and [@usr-sse2](https://github.com/usr-sse2) for developing ambient light sensor support
- [@EMlyDinEsHMG](https://osxlatitude.com/profile/7370-emlydinesh/) for [AsusNBFnKeys source code](https://github.com/EMlyDinEsHMG/AsusNBFnKeys)
- [Objective-See](https://objective-see.com) for [kernel-userspace communication](https://objective-see.com/blog/blog_0x0B.html)
- [@tekezo](https://github.com/tekezo) for [VirtualHIDKeyboard](https://github.com/pqrs-org/Karabiner-VirtualHIDDevice/)
- [@williambj1](https://github.com/williambj1) for testing HID support
