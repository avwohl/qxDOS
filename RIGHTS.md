# Software Rights and Licensing

This document describes the licensing and legal status of software
distributed with qxDOS.

## Source Code Offer (GPL Compliance)

qxDOS distributes binaries of several GPL-licensed programs. In
accordance with the GNU General Public License, the complete
corresponding source code for all GPL-licensed components is available
in three independent ways:

1. **On every disk image we ship.** Each FreeDOS bootable disk built
   by `scripts/build_hdd.sh` and `scripts/build_starter_disk.sh`
   contains the full source code under `C:\SOURCE\` and the FreeDOS
   package source trees under `C:\FREEDOS\<PKG>\SOURCE\`. This is
   the GPL §3(a) "accompany it with the corresponding source code,
   on a medium customarily used for software interchange" form of
   compliance, and is the form qxDOS prefers.

2. **From the upstream projects directly:**
   - **qxDOS app:** https://github.com/avwohl/qxDOS
   - **DOSBox Staging:** https://github.com/dosbox-staging/dosbox-staging
   - **FreeDOS kernel:** https://github.com/FDOS/kernel
   - **FreeCom (COMMAND.COM):** https://github.com/FDOS/freecom
   - **FreeDOS utilities:** https://github.com/FDOS
   - **mTCP:** https://www.brutman.com/mTCP/
   - **Crynwr packet drivers:** http://crynwr.com/drivers/
   - **CWSDPMI:** http://sandmann.dotster.com/cwsdpmi/
   - **libslirp:** https://gitlab.freedesktop.org/slirp/libslirp
   - **MS-DOS (Microsoft):** https://github.com/microsoft/MS-DOS

3. **By written request to the developer.** Open an issue at
   https://github.com/avwohl/qxDOS/issues and a fresh disk image
   with corresponding source will be built and uploaded free of
   charge. This offer is valid for at least three years from the
   date of each release.

If anything is missing from a shipped disk image that should be
present under the GPL, that is a bug — please file an issue and a
corrected image will be published.

## DOSBox Staging

The emulator core is DOSBox Staging, a modernized fork of DOSBox.

- **License:** GNU General Public License (GPL) v2+
- **Website:** https://dosbox-staging.github.io/
- **Source:** https://github.com/dosbox-staging/dosbox-staging

## FreeDOS

FreeDOS is the operating system included in the bootable disk images.

- **License:** GNU General Public License (GPL) v2+
- **Copyright:** Copyright 1995-2012 Pasquale J. Villani and The FreeDOS Project
- **Website:** https://www.freedos.org/
- **Source on disk:** Each shipped FreeDOS image contains the kernel,
  FreeCom, and mTCP source under `C:\SOURCE\`, and the original
  FreeDOS package source trees under `C:\FREEDOS\<PKG>\SOURCE\`.
- **Source upstream:** https://github.com/FDOS
- **Components used:**
  - FreeDOS Kernel 2043 (GPL v2+) — https://github.com/FDOS/kernel
  - FreeCom 0.86 (COMMAND.COM) (GPL v2+) — https://github.com/FDOS/freecom
  - FreeDOS utilities (various GPL v2+ and BSD licenses) — https://github.com/FDOS

qxDOS did not write any FreeDOS code. All credit for FreeDOS belongs
to the FreeDOS Project and its contributors.

## CWSDPMI

DPMI server for protected-mode DOS programs (used by DJGPP-compiled
applications such as DJDOOM). Bundled inside the FreeDOS LiveCD
package set.

- **License:** Modified GNU GPL v2 (with embedding/no-fee clause)
- **Author:** Charles W Sandmann
- **Source:** http://sandmann.dotster.com/cwsdpmi/
- **Source on disk:** ships inside `C:\FREEDOS\CWSDPMI\SOURCE\` when
  the LiveCD package is installed.

## MS-DOS (Microsoft)

MS-DOS source and binaries released by Microsoft Corporation under
the MIT License in April 2024. qxDOS uses the v4.0-ozzie binary
layout (the 1985-86 Multitasking MS-DOS 4.0 BETA, the only complete
bootable layout in the open-source release) for its bundled MS-DOS
disk images, and ships the v4.0/src tree (the 1988 production
MS-DOS 4.00/4.01 source) as `C:\SOURCE\MSDOS40.ZIP` on those disks.

- **License:** MIT License
- **Copyright:** Copyright (c) IBM and Microsoft Corporation
- **Website:** https://github.com/microsoft/MS-DOS
- **Source on disk:** `C:\SOURCE\MSDOS40.ZIP` and
  `C:\SOURCE\MDOS4OZ.ZIP`

qxDOS did not write any MS-DOS code. "MS-DOS", "Microsoft", and
"IBM" are trademarks of their respective owners. qxDOS is not
affiliated with, endorsed by, or sponsored by Microsoft or IBM;
the use of these names is descriptive and refers solely to the
MIT-licensed source release at github.com/microsoft/MS-DOS .

## emu88 (alternate hardware backend)

Custom 8088/286/386 interpreter written for the qxDOS project as an
alternative hardware backend to DOSBox Staging. Lives in `emu88/`
in the qxDOS source tree and ships as part of the qxDOS app, not
on any disk image.

- **License:** GNU General Public License (GPL) v3 or later
  (inherited from the qxDOS project)
- **Author:** Aaron Wohl
- **Source:** https://github.com/avwohl/qxDOS (emu88/ directory)

## libslirp

Userspace TCP/IP stack providing NE2000 networking (NAT).

- **License:** BSD 3-Clause
- **Version:** 4.7.0
- **Source:** https://gitlab.freedesktop.org/slirp/libslirp

## mTCP

TCP/IP applications for DOS: FTP, Telnet, DHCP, Ping, HTGET.
Included on the default FreeDOS disk in `C:\NET`.

- **License:** GNU General Public License (GPL) v3
- **Author:** Michael Brutman
- **Website:** https://www.brutman.com/mTCP/

## NE2000 Packet Driver

Crynwr NE2000 packet driver for DOS (NE2000.COM).
Included on the default FreeDOS disk in `C:\NET`.

- **License:** Open Source (Crynwr Software)
- **Source:** http://crynwr.com/drivers/

## Disk Catalog — External Downloads

The disk catalog links to external archives hosted by third parties.
These files are not bundled with the app; they are downloaded by the
user on demand.

### FreeDOS Official ISOs
- **Source:** freedos.org / ibiblio.org
- **License:** GPL v2+

### Simtel MS-DOS Archive
- **Source:** archive.org
- **License:** Shareware / Public Domain collection
- **Contents:** Thousands of MS-DOS utilities, tools, and shareware
  from the Simtel mirror network (1990s)

### Walnut Creek CD-ROMs
- **Source:** archive.org
- **License:** Shareware / Public Domain collection
- **Contents:** Curated DOS software collections from Walnut Creek
  CDROM (founded 1991)

## ZIPFoundation

Used for extracting downloaded ZIP archives.

- **License:** MIT License
- **Source:** https://github.com/weichsel/ZIPFoundation

## Trademarks

"FreeDOS" is a trademark of Jim Hall and the FreeDOS Project.
"MS-DOS", "Microsoft", and "Windows" are trademarks of Microsoft
Corporation. "IBM" and "PC-DOS" are trademarks of International
Business Machines Corporation. "DOSBox" and "DOSBox Staging" are
the names of independent open-source projects. qxDOS is not
affiliated with, endorsed by, or sponsored by any of these
projects or companies. Use of these names in qxDOS is purely
descriptive and refers to the software each name identifies.
