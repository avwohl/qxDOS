# About This App

This app runs DOS inside an emulated 386 PC powered by DOSBox Staging. You can choose which DOS to run: FreeDOS, MS-DOS, or DOSBox's built-in DOS.

## The Emulated PC

The hardware emulation (provided by DOSBox Staging) includes:

- **CPU** — 386/486 with FPU and DPMI (protected mode for games like DOOM)
- **Graphics** — VGA/SVGA (S3 Trio64), all standard video modes
- **Sound** — Sound Blaster 16 with FM synthesis and digital audio
- **Mouse** — PS/2 mouse, works with DOS mouse drivers
- **Ethernet** — NE2000 with SLIRP (virtual NAT for internet access)
- **Drives** — floppy (A:, B:), hard disk (C:, D:), CD-ROM

This hardware layer is the same regardless of which DOS you choose.

## DOS Types

### FreeDOS (default)
FreeDOS is a free, open-source DOS compatible with MS-DOS. The app boots the real FreeDOS kernel from your disk image. The pre-built disk images come with KERNEL.SYS, COMMAND.COM, and utilities in `C:\FREEDOS\BIN`. You can also install FreeDOS from scratch using the official boot floppy and LiveCD ISO from the disk catalog. License: GPL v2+. Website: freedos.org.

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
