# About This App

qxDOS provides the iOS/Mac user interface, touch controls, disk management, and the infrastructure to boot different DOS operating systems. All PC hardware emulation is provided by [DOSBox Staging](https://dosbox-staging.github.io/), an independent open-source project. The operating systems — [FreeDOS](https://www.freedos.org/), [MS-DOS](https://github.com/microsoft/MS-DOS), etc. — are independent projects with their own developers and licenses. qxDOS did not write or contribute code to any of them.

## The Emulated PC

The hardware emulation (provided by [DOSBox Staging](https://dosbox-staging.github.io/)) includes:

- **CPU** — 386/486 with FPU and DPMI (protected mode for games like DOOM)
- **Graphics** — VGA/SVGA (S3 Trio64), all standard video modes
- **Sound** — Sound Blaster 16 with FM synthesis and digital audio
- **Mouse** — PS/2 mouse, works with DOS mouse drivers
- **Ethernet** — NE2000 with SLIRP (virtual NAT for internet access)
- **Drives** — floppy (A:, B:), hard disk (C:, D:), CD-ROM

This hardware layer is the same regardless of which DOS you choose.

## DOS Types

### FreeDOS (default)
FreeDOS is a free, open-source DOS compatible with MS-DOS, developed by the [FreeDOS Project](https://www.freedos.org/). The app boots the real FreeDOS kernel from your disk image. The pre-built disk images come with KERNEL.SYS, COMMAND.COM, and utilities in `C:\FREEDOS\BIN`, all written and maintained by the FreeDOS Project. You can also install FreeDOS from scratch using the official boot floppy and LiveCD ISO from the disk catalog. License: GPL v2+. Source: [github.com/FDOS](https://github.com/FDOS).

### MS-DOS
MS-DOS is the original Microsoft operating system. The app boots the real MS-DOS kernel from your disk image. MS-DOS 4.0 (1988) was released under the MIT license by Microsoft in 2024. You can also use your own MS-DOS install media (5.0, 6.22, etc.).

### DOSBox DOS
DOSBox's built-in kernel and COMMAND.COM. No operating system on disk is needed — DOSBox provides ~30 built-in utilities on the Z: drive (MEM, MORE, MOUSE, MIXER, etc.). This mode is the simplest: mount any FAT disk and start using it immediately. However, it lacks the full utility set of FreeDOS or MS-DOS (no FORMAT, FDISK, EDIT, etc.).

## What's on the FreeDOS Disk

The FreeDOS disk images come with the FreeDOS kernel, COMMAND.COM, and a set of utilities in `C:\FREEDOS\BIN`:

| Command | What it does |
|---------|-------------|
| `EDIT` | Full-screen text editor |
| `MEM` | Show memory usage |
| `XCOPY` | Copy files and directory trees |
| `FORMAT` | Format a floppy disk |
| `FDISK` | Partition a hard disk |
| `DELTREE` | Delete a directory and everything in it |
| `MORE` | Page through long output |
| `LABEL` | Set a disk volume label |

The starter disk also includes:

| Command | What it does |
|---------|-------------|
| `R` | Copy a file from your device into DOS |
| `W` | Copy a file from DOS to your device |
| `CWSDPMI` | DPMI server for protected-mode programs (DOOM, etc.) |

## Networking Tools (FreeDOS)

In `C:\NET` (type `FDNET` to activate):

| Command | What it does |
|---------|-------------|
| `FTP` | Transfer files to/from FTP servers |
| `TELNET` | Remote terminal sessions |
| `PING` | Test network connectivity |
| `HTGET` | Download files via HTTP |
| `DHCP` | Get an IP address (called by NET automatically) |

## CPU Speed

The emulated CPU speed is configurable in machine settings. Presets range from 8088 (4.77 MHz) to Pentium speed. Use higher speeds for protected-mode games. The "Max" setting runs as fast as your device allows.

## DPMI

DPMI (DOS Protected Mode Interface) lets programs use extended memory beyond the 640K conventional limit. CWSDPMI is included on the disk and loads automatically when a DPMI program runs. Games compiled with DJGPP (like DJDOOM) use this.

## FreeDOS vs. MS-DOS

FreeDOS is compatible with MS-DOS at the application level. Most DOS programs, games, and utilities work without modification. FreeDOS is free software (GPL) and actively maintained at freedos.org. You can switch between FreeDOS and MS-DOS in the Machine settings — both boot from real disk images using the same hardware emulation.

## Disk Images

The app supports several disk image formats:

- **Hard disk images** (.img) — bootable FAT16 partitioned disks
- **Floppy images** (.img) — 1.44MB floppy disks
- **CD-ROM images** (.iso) — read-only CD-ROM

You can download ready-made images from the in-app disk catalog, import your own, or create blank disks.

## Open Source Licenses

This app includes software from many independent open-source projects. Complete source code is available three ways:

1. **On every bundled FreeDOS disk image** under `C:\SOURCE\` and per-package `SOURCE/` directories. This is the GPL §3(a) form of compliance and is the qxDOS default.
2. **From the upstream projects** at the links below.
3. **By written request** via [GitHub Issues](https://github.com/avwohl/qxDOS/issues) — valid for at least three years from each release.

- **qxDOS** (GPL v3+) — [github.com/avwohl/qxDOS](https://github.com/avwohl/qxDOS)
- **DOSBox Staging** (GPL v2+) — [github.com/dosbox-staging/dosbox-staging](https://github.com/dosbox-staging/dosbox-staging)
- **emu88** alternate hardware backend (GPL v3+, qxDOS-internal)
- **FreeDOS kernel** (GPL v2+) — [github.com/FDOS/kernel](https://github.com/FDOS/kernel)
- **FreeCom (COMMAND.COM)** (GPL v2+) — [github.com/FDOS/freecom](https://github.com/FDOS/freecom)
- **FreeDOS utilities** (GPL v2+ / BSD) — [github.com/FDOS](https://github.com/FDOS)
- **CWSDPMI** (modified GPL v2) — [sandmann.dotster.com/cwsdpmi](http://sandmann.dotster.com/cwsdpmi/)
- **mTCP** (GPL v3) by Michael Brutman — [brutman.com/mTCP](https://www.brutman.com/mTCP/)
- **NE2000 packet driver** (Crynwr, GPL v2+) — [crynwr.com/drivers](http://crynwr.com/drivers/)
- **libslirp** (BSD 3-Clause) — [gitlab.freedesktop.org/slirp/libslirp](https://gitlab.freedesktop.org/slirp/libslirp)
- **ZIPFoundation** (MIT) — [github.com/weichsel/ZIPFoundation](https://github.com/weichsel/ZIPFoundation)
- **MS-DOS** (MIT, © IBM and Microsoft Corporation) — [github.com/microsoft/MS-DOS](https://github.com/microsoft/MS-DOS)

qxDOS did not write FreeDOS, MS-DOS, DOSBox Staging, mTCP, libslirp, or any of the other operating systems or libraries listed above. All credit for those projects belongs to their respective authors. qxDOS only repackages their work into a SwiftUI app.

"FreeDOS" is a trademark of Jim Hall and the FreeDOS Project. "MS-DOS", "Microsoft", and "Windows" are trademarks of Microsoft Corporation. "IBM" and "PC-DOS" are trademarks of IBM. qxDOS is not affiliated with, endorsed by, or sponsored by any of these projects or companies.
