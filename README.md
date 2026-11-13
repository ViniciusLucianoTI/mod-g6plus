<h1 align="center">iMoto G6 Plus</h1>

<p align="center">
  <b>Apple's iOS 16 on a Moto G6 Plus</b><br>
  Motorola <code>evert</code> · Qualcomm Snapdragon 630 · Adreno 508 · iOS 16.7.16 (20H392)
</p>

<p align="center">
  <img alt="iOS 16.7.16" src="https://img.shields.io/badge/iOS-16.7.16-0a84ff?style=flat-square">
  <img alt="SpringBoard" src="https://img.shields.io/badge/SpringBoard-booting-34c759?style=flat-square">
  <img alt="GPU" src="https://img.shields.io/badge/Adreno%20508-compositing-ff9f0a?style=flat-square">
  <img alt="Kernel" src="https://img.shields.io/badge/kernel-xnu--8796-161616?style=flat-square">
  <img alt="Install" src="https://img.shields.io/badge/install-none%20(RAM%20boot)-555?style=flat-square">
  <img alt="Host" src="https://img.shields.io/badge/host-macOS-555?style=flat-square">
</p>

<p align="center"><sub>A fork of the <a href="https://github.com/ry-diffusion/Bergamota">Bergamota Project</a> (iOS 18 on the Poco F1), ported to the Moto G6 Plus.</sub></p>

---

The Moto G6 Plus boots the **unmodified iOS 16.7.16 userland of the iPhone X** (`iPhone10,3`) to the lock screen and the home screen, with touch, the side buttons and the Adreno 508 drawing the interface.

**Nothing is installed on the phone.** Android stays exactly as it is. The kernel is loaded into RAM with `fastboot boot`, and the iOS root file system is an image on the Mac, served to the phone over the USB cable. Pull the cable or hold Power and the phone is an Android phone again.

| | Part | What it does here | Where |
| --- | --- | --- | --- |
| 🌿 | **Enxerto** | The bootloader, built for `BOARD=evert`. Motorola's ABL starts it from RAM; it inflates the kernel, hands it an Apple Device Tree and boots it. | [`enxerto/`](enxerto) |
| 🪵 | **Kernel** | Apple's open-source XNU, the iOS 16 generation (xnu-8796), plus the patches for the Snapdragon 630 hardware. Built locally. | [`xnu/patches`](xnu/patches) |
| 🔌 | **USB root** | The Mac serves the root image, the kernel console and the input events over one USB link. | [`tools/g6plus`](tools/g6plus) |
| 🎨 | **GPU stack** | A kernel driver for the Adreno 508, Mesa's freedreno (a5xx) built for iOS, and an OpenGL ES framework that CoreAnimation's own renderer runs on. | [`gpu/`](gpu) |

> [!WARNING]
> This is an experimental bring-up, not a usable phone OS. Apple's security model is mostly switched off, so do not put anything personal on the root image. You need an **unlocked bootloader**, and you should keep a backup of your partitions before trying anything.

## The boot chain

```
Motorola ABL (stock, unlocked)            fastboot boot: RAM only, the eMMC is never written
  └─ Enxerto (BOARD=evert)                inflates the kernel, builds the boot arguments
      └─ XNU 8796 + Apple Device Tree     iPhone X identity, Moto G6 Plus hardware
          └─ HFS+ root over USB           iOS 16.7.16, an image file on the Mac (rd=usbroot0)
              └─ dyld → launchd → daemons → backboardd → SpringBoard
```

The kernel resets the SoC by itself after a set time (`sweetselfreset=N` seconds), so an unattended boot always ends back in Android.

## Status

| Area | State |
| --- | --- |
| iOS | 16.7.16 (20H392) to the **lock screen and the home screen**; Settings and the stock apps open |
| CPU | 8× Kryo 260 (ARMv8.0, no LSE, no pointer authentication). iOS 16 for the iPhone X is plain arm64, so no binary rewriting is needed |
| Memory | 3.2 GB of the 4 GB given to the kernel, 4 KiB pages |
| Display | Tianma 1080×2160 panel; iOS renders the iPhone X layout (1125×2436), scaled with thin black bars |
| Touch | Goodix GT9xx over I²C, powered through our own RPM client |
| Buttons | Power and both volume keys (PM660 / PM660L over SPMI) |
| USB | One link for the root disk, the kernel log, input from the Mac, and SSH at `root@192.168.7.2` |
| GPU | **Adreno 508 composites the UI**: zap shader authenticated by TrustZone, command processor running, Mesa freedreno a5xx (OpenGL ES 3.1), CoreAnimation's GLES renderer in `backboardd` |
| Working on | Vibrant text on the GPU path (lock screen clock, tab bar glyphs), rendering without the forced-GMEM workaround, GPU clock above 240 MHz |
| Not working | Wi-Fi, cellular, Bluetooth, audio, camera, sensors, battery gauge, CPU frequency scaling, Metal |

Measured on the phone, CoreAnimation scenes drawn by the GPU instead of the CPU (small offscreen scenes, GPU at 240 MHz):

| Scene | CPU (softpipe) | Adreno 508 |
| --- | --- | --- |
| Backdrop blur, 192×192 | 492 ms | **30 ms** |
| Colour saturation filter | 597 ms | **26 ms** |
| 300 translucent rounded layers | 1627 ms | **34 ms** |

## Hardware

| | |
| --- | --- |
| Phone | Moto G6 Plus, `evert` (tested on XT1926-8, 4 GB / 64 GB), bootloader MBM-3.0, unlocked |
| SoC | Qualcomm SDM630: 8× Kryo 260, GICv3, PM660 + PM660L |
| GPU | Adreno 508 (chip id `0x05000800`), firmware `a530_pm4.fw`, `a530_pfp.fw`, `a508_zap` |
| Storage | 64 GB eMMC, **not used by iOS** |
| Android side | Any Android with root (tested with LineageOS 22 / Android 15 + Magisk), only to read the device tree and the GPU firmware once |

Everything we know about the board is in [`docs/G6PLUS_HARDWARE.md`](docs/G6PLUS_HARDWARE.md).

## Getting started

You need a Mac with Xcode, the phone with an unlocked bootloader and USB debugging authorised, and about 40 GB of free disk space.

```sh
brew install --cask android-platform-tools
brew install aarch64-elf-gcc dtc libusb blacktop/tap/ipsw
```

**0. Collect from your phone (read only).** The stock device tree and the GPU firmware belong to your phone, so they are not in git:

```sh
tools/g6plus/collect.sh                                  # device facts + boot image into backup/evert/ (needs root in Android)
python3 tools/extract-dtb.py backup/evert/*/boot.img dtb/evert --live dtb/evert/live.dtb
# a530_pm4.fw, a530_pfp.fw, a508_zap.mdt/.b00-.b02 from /vendor/firmware -> research/g6-session/gpufw/
```

**1. The kernel.** Built on the Mac from Apple's sources plus `xnu/patches`:

```sh
XNU_LOCAL_DIR=$PWD/research/xnu-local-13 USE_KDK=1 tools/xnu-local-build.sh 13.5
# -> research/xnu-local-13/build/xnu.obj/kernel.development.sweet
```

**2. The Apple Device Tree.** The iPhone X identity comes from an IPSW, the hardware description from this repository, the GPU firmware from step 0:

```sh
python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt           # any iPhone X (iPhone10,3) IPSW
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
python3 tools/mkadt.py research/g6-session/boot/evert-ios.adt --board evert
python3 tools/adt-gpufw.py research/g6-session/boot/evert-ios.adt research/g6-session/boot/evert-ios-gpu.adt \
    --a508 research/g6-session/gpufw
```

**3. The iOS root image.** From the iPhone X IPSW of iOS 16.7.16 (`iPhone10,3`, build 20H392); the three disk images are the OS and its two cryptexes:

```sh
sudo tools/mkiosroot-macos.sh OS.dmg SYSTEMOS.dmg APPOS.dmg research/ipsw-x/ios16-root-g6.img
cp -c research/ipsw-x/ios16-root-g6.img research/ipsw-x/ios16-root-g6-work.img   # iOS writes to the working copy
sudo bash tools/g6plus/install-ssh.sh research/ipsw-x/ios16-root-g6-work.img      # optional: SSH server
```

**4. The boot image.** Kernel, device tree and boot arguments packed into an Enxerto image:

```sh
python3 -I tools/mkpayload.py research/xnu-local-13/build/xnu.obj/kernel.development.sweet \
    research/g6-session/boot/evert-ios-gpu.adt payload.bin --compress \
    --cmdline "$(cat tools/g6plus/bootargs-ios16.txt)"
(cd enxerto && BOARD=evert ./build.sh ../payload.bin)        # -> enxerto/out/enxerto-boot.img
```

**5. Boot.** With the phone in Android and plugged in. The script reboots it to fastboot, loads the image into RAM and serves the root:

```sh
tools/g6plus/ios_usb_boot.sh enxerto/out/enxerto-boot.img research/ipsw-x/ios16-root-g6-work.img 960 /tmp/g6-ios
tail -f /tmp/g6-ios.log                                  # the kernel console
ssh root@192.168.7.2                                     # once iOS is up
```

The Apple logo appears after about a minute and a half, the lock screen a few minutes later. After `sweetselfreset` seconds the phone goes back to Android.

## Driving it from the Mac

| Tool | What it does |
| --- | --- |
| [`ios_usb_boot.sh`](tools/g6plus/ios_usb_boot.sh) | One unattended boot: fastboot, root server, console log |
| [`usbdisk_server.py`](tools/g6plus/usbdisk_server.py) | The Mac side of the USB link (disk blocks, log, input) |
| [`g6input.py`](tools/g6plus/g6input.py) | Keys and touches from the Mac: `home`, `power`, `tap X Y`, `swipe`, or a web pad with `pad` |
| [`disable_jobs.py`](tools/g6plus/disable_jobs.py) | Turns off launchd jobs that cannot work here and burn CPU |
| [`bglrender.py`](tools/g6plus/bglrender.py) | Points `backboardd` at the GPU launcher, or back |
| [`bglshot.sh`](tools/g6plus/bglshot.sh) | A screenshot of what the GPU compositor drew, over SSH |

## The GPU

The Adreno 508 has no open Vulkan driver, so the path is OpenGL ES: iOS 16's CoreAnimation still carries a GLES renderer, and it runs on Mesa.

```
backboardd (CoreAnimation, its own OpenGL ES renderer)
  └─ OpenGLES.framework replacement      gpu/eagl: EAGLContext, IOSurface-backed textures, zero-copy display
      └─ Mesa 25.0.7, freedreno a5xx     OpenGL ES 3.1, built for iOS arm64
          └─ libdrm stand-in             gpu/freedreno-sweet, the drm/msm ioctls over an IOKit user client
              └─ kernel driver           xnu/patches 1044-1055: clocks, GDSCs, zap, SMMU, ring buffer, IOSurface import
                  └─ Adreno 508
```

To turn it on in a running system (the launcher falls back to the CPU compositor after three failed starts):

```sh
TARGET=ios16-arm64 GPU_EAGL_WORK=$PWD/research/mesa-ios16 gpu/eagl/make-payload.sh   # -> research/bergamota-gl/
# copy research/bergamota-gl/ to /usr/local/lib/bergamota-gl/ on the phone, then:
python3 tools/g6plus/bglrender.py install
# boot with bglrender=es in the boot arguments
```

The plan, the phases and what each one proved are in [`docs/G6PLUS_GPU_PLAN.md`](docs/G6PLUS_GPU_PLAN.md).

## Repository layout

| Path | What |
| --- | --- |
| `xnu/patches/` | The kernel patch series for xnu-8796. The Moto G6 Plus work is 1021 onwards: ARMv8.0 baseline, 4 KiB pages, USB root, touch, keys, RPM, Adreno 508 |
| `enxerto/` | The bootloader; `BOARD=evert` selects the Moto G6 Plus (and refuses to write the eMMC) |
| `tools/g6plus/` | Everything that runs on the Mac for this phone: boot, USB server, input, boot arguments |
| `tools/mkadt.py`, `mkpayload.py`, `adt-gpufw.py` | Apple Device Tree generator (`--board evert`), payload packer, GPU firmware into the device tree |
| `tools/xnu-local-build.sh` | The local kernel build |
| `gpu/eagl/` | The OpenGL ES framework replacement, the `backboardd` launcher and the Mesa build |
| `gpu/freedreno-sweet/` | The libdrm stand-in over the kernel's user client |
| `gpu/kernel-test/` | On-device tests: user client, triangle, lines, render to texture, IOSurface copies |
| `docs/G6PLUS_HARDWARE.md`, `docs/G6PLUS_GPU_PLAN.md` | Hardware facts and the GPU plan |
| `NOTES.md` | The engineering log: every boot, crash and decision |

Not in git: partition backups, device trees, GPU firmware, Apple firmware and root images, or anything with serial numbers.

## Credits

iMoto G6 Plus is a fork of the [Bergamota Project](https://github.com/ry-diffusion/Bergamota), which brought iOS 18 to the Poco F1 and wrote Enxerto, the device tree and payload tools, and the GPU stack this port builds on. Its original README is kept in [`docs/BERGAMOTA_README.md`](docs/BERGAMOTA_README.md). Bergamota itself started from [ios-sweet](https://github.com/dipa-studio/ios-sweet) (iOS 16 on the Redmi Note 10 Pro).

The kernel is Apple's open-source [XNU](https://github.com/apple-oss-distributions/xnu). The graphics driver is [Mesa](https://www.mesa3d.org)'s freedreno. iOS, iPhone and the Apple logo belong to Apple; Moto G6 Plus belongs to Motorola. This project ships none of their firmware or system software.
