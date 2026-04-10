#!/bin/bash
# Build a 1.44 MB FreeDOS boot floppy with qxDOS attribution baked in.
#
# Strategy: take the upstream FreeDOS 1.4 boot floppy (which has the
# kernel + COMMAND.COM + the FreeDOS install shell), copy in our
# README.TXT / CREDITS.TXT / SOURCE.TXT / LICENSE\GPL2.TXT and the
# qxDOS-flavor FDCONFIG.SYS + AUTOEXEC.BAT.  Write the result to
# fd/freedos_qx.img and update the disk catalog.
#
# This is a derivative work of the FreeDOS boot floppy.  The
# original kernel and FreeCom remain GPL v2+ -- see C:\CREDITS.TXT
# on the resulting floppy and  https://github.com/FDOS .

set -e

IMGDIR="$(cd "$(dirname "$0")/.." && pwd)"
FINALIMG="$IMGDIR/fd/freedos_qx.img"
OUTIMG="/tmp/freedos_qx_build.img"
SRCIMG="$IMGDIR/fd/freedos.img"
DC="$IMGDIR/disk-content"

# Download upstream FreeDOS 1.4 FloppyEdition (zip of all install floppies)
# and extract the 1.44 MB boot floppy.  We cache the result as
# fd/freedos.img so subsequent builds don't redownload.
if [ ! -f "$SRCIMG" ] || [ "$(stat -f%z "$SRCIMG" 2>/dev/null || stat -c%s "$SRCIMG")" != "1474560" ]; then
  mkdir -p "$IMGDIR/fd"
  echo "Downloading FreeDOS 1.4 FloppyEdition zip..."
  curl -L --retry 3 --connect-timeout 30 -o /tmp/fd14floppy.zip \
    https://download.freedos.org/1.4/FD14-FloppyEdition.zip
  echo "Extracting 1.44 MB boot floppy..."
  rm -rf /tmp/fd14floppy
  mkdir -p /tmp/fd14floppy
  unzip -j -o /tmp/fd14floppy.zip '144m/x86BOOT.img' -d /tmp/fd14floppy
  cp /tmp/fd14floppy/x86BOOT.img "$SRCIMG"
fi

# Sanity check: a real 1.44 MB floppy is exactly 1474560 bytes
SRCSZ=$(stat -f%z "$SRCIMG" 2>/dev/null || stat -c%s "$SRCIMG")
if [ "$SRCSZ" != "1474560" ]; then
  echo "ERROR: $SRCIMG is $SRCSZ bytes, expected 1474560 (1.44 MB floppy)"
  echo "       The download did not return a valid FreeDOS boot floppy."
  exit 1
fi

# Start from a copy of the upstream image -- preserves bootloader,
# kernel, and FreeCom exactly.
echo "Cloning upstream FreeDOS boot floppy..."
cp "$SRCIMG" "$OUTIMG"

# mtools setup
export MTOOLS_SKIP_CHECK=1
cat > /tmp/mtoolsrc_floppy << EOF
mtools_skip_check=1
drive a: file="$OUTIMG"
EOF
export MTOOLSRC=/tmp/mtoolsrc_floppy

# Helper: convert host text file to DOS line endings then mcopy onto a:
to_dos() {
    sed 's/\r$//' "$1" | sed 's/$/'$'\r''/' > "$2"
}

# =========================================================================
# Add attribution files
# =========================================================================
echo "Adding attribution files to floppy..."

if [ -d "$DC/freedos" ]; then
    for f in README.TXT CREDITS.TXT SOURCE.TXT; do
        if [ -f "$DC/freedos/$f" ]; then
            to_dos "$DC/freedos/$f" "/tmp/$f"
            mcopy -D o "/tmp/$f" "a:/$f" 2>/dev/null || \
                echo "  WARNING: could not add $f (floppy may be full)"
        fi
    done
fi

# License: only GPL2.TXT fits on a 1.44 MB floppy alongside the kernel.
# Don't bother with GPL3.TXT or MIT.TXT here -- floppy is FreeDOS-only,
# and the on-floppy SOURCE.TXT already points to where the full text
# can be obtained.  Skip GPL2.TXT entirely if there isn't room.
if [ -f "$DC/licenses/GPL2.TXT" ]; then
    mmd a:/LICENSE 2>/dev/null || true
    to_dos "$DC/licenses/GPL2.TXT" "/tmp/GPL2.TXT"
    mcopy -D o "/tmp/GPL2.TXT" "a:/LICENSE/GPL2.TXT" 2>/dev/null || \
        echo "  NOTE: GPL2.TXT did not fit -- pointer in SOURCE.TXT instead"
fi

# Replace AUTOEXEC.BAT with a friendlier qxDOS one if there isn't
# already an autoexec the user expects (the upstream installer floppy
# runs the FreeDOS installer; we leave it alone if that's what the
# upstream image is).  Detect the installer by looking for setup.bat
# or FDIBOOT.BAT.
if mdir -b a: 2>/dev/null | grep -qiE "(SETUP|FDIBOOT)"; then
    echo "  upstream image is the installer floppy, leaving boot scripts alone"
else
    echo "  installing qxDOS AUTOEXEC.BAT and FDCONFIG.SYS"
    printf 'LASTDRIVE=Z\r\nFILES=40\r\nBUFFERS=20\r\nDOS=HIGH\r\nSHELL=A:\\COMMAND.COM A:\\ /E:1024 /P\r\n' > /tmp/FDCONFIG.SYS
    mcopy -D o /tmp/FDCONFIG.SYS a:/ 2>/dev/null || true

    printf '@ECHO OFF\r\nPROMPT $P$G\r\nECHO.\r\nECHO qxDOS FreeDOS boot floppy.  See A:\\README.TXT for what is here\r\nECHO and A:\\CREDITS.TXT for the people who wrote FreeDOS.\r\nECHO Source code:  see A:\\SOURCE.TXT  or  github.com/avwohl/qxDOS\r\nECHO.\r\n' > /tmp/AUTOEXEC.BAT
    mcopy -D o /tmp/AUTOEXEC.BAT a:/ 2>/dev/null || true
fi

# =========================================================================
# Report contents
# =========================================================================
echo ""
echo "FreeDOS boot floppy: $OUTIMG"
echo "Size: $(ls -lh "$OUTIMG" | awk '{print $5}')"
echo ""
echo "Contents:"
mdir a: 2>/dev/null || echo "(mdir failed)"
echo ""
echo "Free space:"
minfo a: 2>/dev/null | grep -i "free\|total" || true

# =========================================================================
# Update the catalog
# =========================================================================
bash "$IMGDIR/scripts/update_catalog.sh" "$OUTIMG" "$FINALIMG"
