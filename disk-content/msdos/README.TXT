                  qxDOS - MS-DOS 4.0 Distribution
                  ===============================

Welcome.  This disk image was built by qxDOS, a free, open-source
DOS emulator app for iOS, iPadOS, and macOS.

This disk boots Multitasking MS-DOS 4.0 BETA, originally written by
Microsoft Corporation and IBM Corporation in 1985-1986.  In April
2024 Microsoft released the complete source code and binaries for
MS-DOS 1.25, MS-DOS 2.0, and the production and Multitasking versions
of MS-DOS 4.0 under the MIT License at:

  https://github.com/microsoft/MS-DOS

The Multitasking MS-DOS 4.0 BETA was a joint Microsoft / IBM project,
recovered by Ray Ozzie from his collection and released by Microsoft
in 2024.  It is the only fully bootable layout in the Microsoft
open-source release; that is why qxDOS uses it as the on-disk OS.

  qxDOS did not write MS-DOS.  All MS-DOS code, kernel, COMMAND.COM,
  and utilities are the work of Microsoft, IBM, and the original
  authors named in their source files.  qxDOS only repackages those
  binaries onto a bootable disk image suitable for use with the
  qxDOS emulator.


What is on this disk
--------------------

  C:\IBMBIO.COM          MS-DOS BIOS interface (hidden, system)
  C:\IBMDOS.COM          MS-DOS kernel       (hidden, system)
  C:\COMMAND.COM         Command shell
  C:\CONFIG.SYS          MS-DOS startup configuration
  C:\AUTOEXEC.BAT        Startup batch file
  C:\BIN\                Multitasking 4.0 utilities (CHKDSK, KILL, ...)
  C:\DOS33\              DOS 3.3 compatibility utilities (FDISK,
                         FORMAT, SYS) shipped on the original BETA disk
  C:\SOURCE\             Source code -- see SOURCE.TXT
  C:\LICENSE.TXT         MIT License (Microsoft + IBM)
  C:\CREDITS.TXT         Full credits and copyright notices

Bonus utilities from the MS-DOS 2.0 binary release (DEBUG, EDLIN, FC,
FIND, MORE, PRINT, RECOVER, SORT) are also installed in C:\BIN\
because they are documented to run under any newer DOS API.


Source code on this disk
------------------------

The MIT License Microsoft chose for the MS-DOS 4.0 release does not
require source distribution along with binaries -- but qxDOS ships
it anyway, because that is the polite thing to do and because keeping
source and binaries together makes the disk self-contained.

  C:\SOURCE\MSDOS40.ZIP   The 1988 production MS-DOS 4.00/4.01 source
                          code, exactly as published at
                          github.com/microsoft/MS-DOS in v4.0/src

  C:\SOURCE\MDOS4OZ.ZIP   The Multitasking MS-DOS 4.0 BETA boot disks
                          (DISK1, DISK2) including IBMBIO.ASM and
                          IBMDOS.ASM -- the source code that produced
                          the IBMBIO.COM and IBMDOS.COM that boot
                          this disk

If for any reason these archives are missing or corrupt, you can
fetch the originals at:

  https://github.com/microsoft/MS-DOS


qxDOS app source
----------------

The qxDOS app itself does not run on this disk and is not covered
by Microsoft's MIT release.  qxDOS is licensed under GNU GPL v3
and its source is at:

  https://github.com/avwohl/qxDOS

For more about qxDOS, see C:\CREDITS.TXT.
