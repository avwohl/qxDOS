#!/bin/bash
# Build a 1.44 MB MS-DOS boot floppy from microsoft/MS-DOS .
#
# Uses the v4.0-ozzie/bin/DISK1/ layout (the only complete bootable
# MS-DOS layout in the open-source release).  See build_msdos_hd.sh
# for the rationale.  Output is fd/msdos.img.
#
# Copyright on the included files: (c) IBM and Microsoft Corporation,
# released under the MIT License at github.com/microsoft/MS-DOS .

set -e

IMGDIR="$(cd "$(dirname "$0")/.." && pwd)"
FINALIMG="$IMGDIR/fd/msdos.img"
OUTIMG="/tmp/msdos_floppy_build.img"
DC="$IMGDIR/disk-content"
MSDOS_REPO="$IMGDIR/fd/source-cache/MS-DOS"

mkdir -p "$IMGDIR/fd/source-cache"

if [ ! -d "$MSDOS_REPO" ]; then
    echo "Cloning microsoft/MS-DOS..."
    git clone --depth 1 https://github.com/microsoft/MS-DOS "$MSDOS_REPO"
fi

DISK1="$MSDOS_REPO/v4.0-ozzie/bin/DISK1"
if [ ! -d "$DISK1" ]; then
    echo "ERROR: $DISK1 missing -- microsoft/MS-DOS layout has changed"
    exit 1
fi

# =========================================================================
# 1. Create a 1.44 MB FAT12 floppy image
# =========================================================================
echo "Creating 1.44 MB MS-DOS boot floppy..."
dd if=/dev/zero of="$OUTIMG" bs=512 count=2880 2>/dev/null
mkfs.fat -F 12 -n "MSDOS40" /dev/null > /dev/null 2>&1 || true
mkfs.fat -F 12 -n "MSDOS40" "$OUTIMG"

# 2. mtools setup
export MTOOLS_SKIP_CHECK=1
cat > /tmp/mtoolsrc_msdos_fp << EOF
mtools_skip_check=1
drive a: file="$OUTIMG"
EOF
export MTOOLSRC=/tmp/mtoolsrc_msdos_fp

# =========================================================================
# 3. Install boot files
# =========================================================================

# IBMBIO.COM and IBMDOS.COM are the boot files; they must be the
# first two entries in the root directory.  Copy them first, then
# mark them hidden+system.
mcopy -D o "$DISK1/IBMBIO.COM" a:/IBMBIO.COM
mcopy -D o "$DISK1/IBMDOS.COM" a:/IBMDOS.COM
mattrib +s +h a:/IBMBIO.COM a:/IBMDOS.COM 2>/dev/null || true

mcopy -D o "$DISK1/COMMAND.COM" a:/COMMAND.COM

# Original CONFIG.SYS / AUTOEXEC.BAT from DISK1 -- this is the way
# the disk shipped, so leave it alone.  The user can run COMMANDS.DOC
# / SM.DOC to learn the multitasking shell.
for f in CONFIG.SYS AUTOEXEC.BAT; do
    if [ -f "$DISK1/$f" ]; then
        mcopy -D o "$DISK1/$f" "a:/$f"
    fi
done

# Utilities -- only the small ones that fit in 1.44 MB after the
# kernel.  Skip the multitasking-shell extras (SM.EXE, ARENA.EXE,
# etc.) which are bigger; they live on the HDD image.
mmd a:/BIN 2>/dev/null || true
for f in CHKDSK.EXE SLEEP.EXE KILL.EXE; do
    if [ -f "$DISK1/BIN/$f" ]; then
        mcopy -D o "$DISK1/BIN/$f" "a:/BIN/$f" 2>/dev/null || true
    fi
done

# DOS33 utilities -- FDISK, FORMAT, SYS
mmd a:/DOS33 2>/dev/null || true
for f in FDISK.COM FORMAT.COM SYS.COM; do
    if [ -f "$DISK1/DOS33/$f" ]; then
        mcopy -D o "$DISK1/DOS33/$f" "a:/DOS33/$f" 2>/dev/null || true
    fi
done

# =========================================================================
# 4. Attribution / license / source files
# =========================================================================
echo "Adding attribution and license files..."

to_dos() {
    sed 's/\r$//' "$1" | sed 's/$/'$'\r''/' > "$2"
}

if [ -d "$DC/msdos" ]; then
    for f in README.TXT CREDITS.TXT SOURCE.TXT; do
        if [ -f "$DC/msdos/$f" ]; then
            to_dos "$DC/msdos/$f" "/tmp/$f"
            mcopy -D o "/tmp/$f" "a:/$f" 2>/dev/null || \
                echo "  WARNING: could not add $f (floppy may be full)"
        fi
    done
fi

# MIT license text (small enough to always fit)
if [ -f "$MSDOS_REPO/v4.0/LICENSE" ]; then
    to_dos "$MSDOS_REPO/v4.0/LICENSE" /tmp/LICENSE.TXT
    mcopy -D o /tmp/LICENSE.TXT a:/LICENSE.TXT 2>/dev/null || true
elif [ -f "$DC/licenses/MIT.TXT" ]; then
    to_dos "$DC/licenses/MIT.TXT" /tmp/LICENSE.TXT
    mcopy -D o /tmp/LICENSE.TXT a:/LICENSE.TXT 2>/dev/null || true
fi

# =========================================================================
# 5. Report contents
# =========================================================================
echo ""
echo "MS-DOS boot floppy: $OUTIMG"
echo "Size: $(ls -lh "$OUTIMG" | awk '{print $5}')"
echo ""
echo "Contents:"
mdir a: 2>/dev/null || echo "(mdir failed)"
echo ""
echo "Free space:"
minfo a: 2>/dev/null | grep -i "free\|total" || true

# =========================================================================
# 6. Update the catalog
# =========================================================================
bash "$IMGDIR/scripts/update_catalog.sh" "$OUTIMG" "$FINALIMG"
