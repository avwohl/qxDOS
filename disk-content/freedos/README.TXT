                  qxDOS - FreeDOS Distribution
                  ============================

Welcome.  This disk image was built by qxDOS, a free, open-source
DOS emulator app for iOS, iPadOS, and macOS.

This disk boots FreeDOS, a free, open-source DOS-compatible operating
system developed by The FreeDOS Project (https://www.freedos.org/).
FreeDOS is independent software, distributed here under the terms of
its own license (GNU General Public License v2 or later).

  qxDOS did not write FreeDOS.  All FreeDOS code, kernel, COMMAND.COM,
  and utilities are the work of the FreeDOS Project and the many
  contributors named in their respective copyright notices.

What is on this disk
--------------------

  C:\KERNEL.SYS         FreeDOS kernel (loaded at boot)
  C:\COMMAND.COM        FreeCom shell (the DOS prompt)
  C:\FDCONFIG.SYS       FreeDOS startup configuration
  C:\AUTOEXEC.BAT       Startup batch file
  C:\FREEDOS\BIN\       FreeDOS utilities (~230 commands)
  C:\FREEDOS\DOC\       FreeDOS package documentation
  C:\FREEDOS\NLS\       Language / locale files
  C:\NET\               mTCP networking tools and packet driver
  C:\SOURCE\            Source code archives -- see SOURCE.TXT
  C:\LICENSE\           License texts (GPL v2, GPL v3, MIT, Crynwr)
  C:\CREDITS.TXT        Full credits and copyright notices

Source code on this disk
------------------------

In accordance with section 3 of the GNU General Public License,
the corresponding source code for every GPL-licensed program on
this disk is shipped on the same medium.

  - FreeDOS package source: each FreeDOS package directory under
    C:\FREEDOS\<PKG>\ contains a SOURCE\ subdirectory with the
    source code for that package, exactly as published by the
    FreeDOS Project.

  - FreeDOS kernel and FreeCom source: see C:\SOURCE\

  - mTCP source:                          see C:\SOURCE\

  - qxDOS, DOSBox Staging, libslirp, and other host-side code does
    not run on this disk; their source is published on GitHub at
    https://github.com/avwohl/qxDOS  (qxDOS) and at the URLs in
    C:\CREDITS.TXT.

If anything is missing from this disk that should be here under the
GPL, please open an issue at https://github.com/avwohl/qxDOS/issues
and a corrected disk image will be published.

For more information about qxDOS, see:

  https://github.com/avwohl/qxDOS

For more information about FreeDOS, see:

  https://www.freedos.org/
  https://github.com/FDOS
