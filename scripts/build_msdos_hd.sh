#!/bin/bash
# Build a bootable MS-DOS hard disk image from the Microsoft MS-DOS
# open-source release at https://github.com/microsoft/MS-DOS .
#
# The Microsoft repo contains FOUR distinct DOS versions:
#
#   v1.25/        source + a few binaries (BASIC games, COMMAND.COM)
#                 -- not a complete bootable layout
#   v2.0/         source + some binaries (no IBMBIO.COM)
#                 -- not a complete bootable layout
#   v4.0/         source-only (the 1988 production MS-DOS 4.00/4.01,
#                 IBM/Microsoft) -- not buildable without an old MASM
#   v4.0-ozzie/   binaries + source for the 1985-86 MULTITASKING
#                 MS-DOS 4.0 BETA (Microsoft + IBM, recovered by
#                 Ray Ozzie and released by Microsoft in 2024).
#                 Has a fully bootable layout in DISK1/.
#
# This script uses v4.0-ozzie/bin/DISK1/ as the bootable layout
# (it is the only complete bootable layout in the repo) and adds
# the v4.0/src tree as a separate MSDOS40-SRC.ZIP under C:\SOURCE\
# so users can also see the production MS-DOS 4.0 source code.
#
# Output:  fd/msdos_hd.img        (64 MB FAT16 with MBR)
#
# Both versions of MS-DOS 4.0 are released by Microsoft under the
# MIT License (see v4.0/LICENSE).  Copyright (c) IBM and Microsoft
# Corporation.

set -e

IMGDIR="$(cd "$(dirname "$0")/.." && pwd)"
FINALIMG="$IMGDIR/fd/msdos_hd.img"
OUTIMG="/tmp/msdos_hd_build.img"
DC="$IMGDIR/disk-content"
MSDOS_REPO="$IMGDIR/fd/source-cache/MS-DOS"

mkdir -p "$IMGDIR/fd/source-cache"

# Clone microsoft/MS-DOS once into our cache
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
# 1. Build a 64 MB FAT16 HDD image with an MBR + partition table
# =========================================================================
HEADS=16
SPT=63
CYLS=130   # ~64 MB
TOTAL_SECTORS=$((CYLS * HEADS * SPT))
PART_START=63
PART_SECTORS=$((TOTAL_SECTORS - PART_START))
IMG_SIZE=$((TOTAL_SECTORS * 512))

echo "Creating ${IMG_SIZE} byte ($((IMG_SIZE/1024/1024))MB) MS-DOS hard disk..."
echo "Geometry: C=$CYLS H=$HEADS S=$SPT = $TOTAL_SECTORS sectors"

dd if=/dev/zero of="$OUTIMG" bs=512 count=$TOTAL_SECTORS 2>/dev/null

# 2. MBR boot code + partition table -- same template as build_hdd.sh
python3 -c "
import struct, sys

code = bytearray(446)
boot2 = bytes([
    0xFA, 0x33, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C,
    0x8E, 0xD8, 0x8E, 0xC0, 0xFB,
    0xBE, 0xBE, 0x7D, 0xB9, 0x04, 0x00,
    0x80, 0x3C, 0x80, 0x74, 0x09,
    0x83, 0xC6, 0x10, 0xE2, 0xF5,
    0xCD, 0x18, 0xEB, 0xFE,
    0x8A, 0x74, 0x01, 0x8B, 0x4C, 0x02,
    0xB2, 0x80, 0xBB, 0x00, 0x7C,
    0xB8, 0x01, 0x02, 0xCD, 0x13,
    0x72, 0xFE,
    0xEA, 0x00, 0x7C, 0x00, 0x00,
])
code[:len(boot2)] = boot2

part = bytearray(16)
part[0] = 0x80
part[1] = 1; part[2] = 1; part[3] = 0
part[4] = 0x06
end_cyl = $CYLS - 1
part[5] = ($HEADS - 1) & 0xFF
part[6] = ($SPT & 0x3F) | ((end_cyl >> 2) & 0xC0)
part[7] = end_cyl & 0xFF
struct.pack_into('<I', part, 8, $PART_START)
struct.pack_into('<I', part, 12, $PART_SECTORS)

mbr = bytearray(512)
mbr[:len(code)] = code
mbr[446:462] = part
mbr[510] = 0x55; mbr[511] = 0xAA
sys.stdout.buffer.write(mbr)
" > /tmp/mbr_msdos.bin

dd if=/tmp/mbr_msdos.bin of="$OUTIMG" bs=512 count=1 conv=notrunc 2>/dev/null

# 3. Format partition as FAT16 with MSDOS volume label
dd if=/dev/zero of=/tmp/msdos_part.img bs=512 count=$PART_SECTORS 2>/dev/null
mkfs.fat -F 16 -n "MSDOS40" -h $PART_START -S 512 -s 4 -g $HEADS/$SPT /tmp/msdos_part.img
dd if=/tmp/msdos_part.img of="$OUTIMG" bs=512 seek=$PART_START conv=notrunc 2>/dev/null

# 4. mtools setup -- use C: pointing into the partition
export MTOOLS_SKIP_CHECK=1
PART_OFFSET=$((PART_START * 512))
cat > /tmp/mtoolsrc_msdos << EOF
mtools_skip_check=1
drive c: file="$OUTIMG" offset=$PART_OFFSET
EOF
export MTOOLSRC=/tmp/mtoolsrc_msdos

# =========================================================================
# 5. Install MS-DOS 4.0-ozzie layout into the disk root
# =========================================================================
echo "Installing MS-DOS 4.0 (Multitasking BETA) boot files..."

# IBMBIO.COM and IBMDOS.COM must be the first two files in the root
# directory, in that order, on a freshly-mformatted disk so the boot
# sector can find them.  We copy them first, before anything else.
mcopy -D o "$DISK1/IBMBIO.COM" c:/IBMBIO.COM
mcopy -D o "$DISK1/IBMDOS.COM" c:/IBMDOS.COM
mattrib +s +h c:/IBMBIO.COM c:/IBMDOS.COM 2>/dev/null || true

mcopy -D o "$DISK1/COMMAND.COM" c:/COMMAND.COM

# v4.0-ozzie utilities
mmd c:/BIN 2>/dev/null || true
for f in "$DISK1/BIN"/*; do
    [ -f "$f" ] || continue
    bn=$(basename "$f" | tr '[:lower:]' '[:upper:]')
    mcopy -D o "$f" "c:/BIN/$bn" 2>/dev/null || true
done

# v4.0-ozzie DOS33 compat utilities (FDISK, FORMAT, SYS)
mmd c:/DOS33 2>/dev/null || true
for f in "$DISK1/DOS33"/*; do
    [ -f "$f" ] || continue
    bn=$(basename "$f" | tr '[:lower:]' '[:upper:]')
    mcopy -D o "$f" "c:/DOS33/$bn" 2>/dev/null || true
done

# Documentation as shipped on the original boot disk
for f in README COMMANDS.DOC SM.DOC SM.INI; do
    if [ -f "$DISK1/$f" ]; then
        # README has no extension -- preserve original name
        out=$(echo "$f" | tr '[:lower:]' '[:upper:]')
        mcopy -D o "$DISK1/$f" "c:/$out" 2>/dev/null || true
    fi
done

# Bonus: include the v2.0 standalone utilities that don't conflict
# with the v4.0-ozzie set.  These are real .COM/.EXE files that should
# run under the multitasking kernel (the v2.0 DOS API is a strict
# subset of v4.0).
V2BIN="$MSDOS_REPO/v2.0/bin"
if [ -d "$V2BIN" ]; then
    echo "  adding v2.0 utilities (DEBUG, EDLIN, FC, FIND, MORE, PRINT, SORT)..."
    for f in DEBUG.COM EDLIN.COM FC.EXE FIND.EXE MORE.COM PRINT.COM \
             RECOVER.COM SORT.EXE; do
        if [ -f "$V2BIN/$f" ]; then
            mcopy -D o "$V2BIN/$f" "c:/BIN/$f" 2>/dev/null || true
        fi
    done
fi

# =========================================================================
# 6. CONFIG.SYS / AUTOEXEC.BAT
# =========================================================================
printf 'BUFFERS=20\r\nFILES=40\r\nBREAK=ON\r\n' > /tmp/CONFIG.SYS
mcopy -D o /tmp/CONFIG.SYS c:/CONFIG.SYS

printf '@ECHO OFF\r\nPATH C:\\BIN;C:\\DOS33;C:\\\r\nPROMPT $P$G\r\nECHO.\r\nECHO Multitasking MS-DOS 4.0 BETA  --  Copyright (c) IBM and Microsoft\r\nECHO Released under the MIT License at github.com/microsoft/MS-DOS\r\nECHO.\r\nECHO See C:\\README.TXT for details and C:\\CREDITS.TXT for credits.\r\nECHO Source code is in C:\\SOURCE\\MSDOS40-SRC.ZIP\r\nECHO.\r\n' > /tmp/AUTOEXEC.BAT
mcopy -D o /tmp/AUTOEXEC.BAT c:/AUTOEXEC.BAT

# =========================================================================
# 7. Attribution / license / source files
# =========================================================================
echo "Installing license texts and attribution files..."

to_dos() {
    sed 's/\r$//' "$1" | sed 's/$/'$'\r''/' > "$2"
}

if [ -d "$DC/msdos" ]; then
    for f in README.TXT CREDITS.TXT SOURCE.TXT; do
        if [ -f "$DC/msdos/$f" ]; then
            to_dos "$DC/msdos/$f" "/tmp/$f"
            mcopy -D o "/tmp/$f" "c:/$f"
        fi
    done
fi

# Microsoft / IBM MIT license text -- use the one from the repo
# itself if present, else fall back to disk-content/licenses/MIT.TXT
if [ -f "$MSDOS_REPO/v4.0/LICENSE" ]; then
    to_dos "$MSDOS_REPO/v4.0/LICENSE" /tmp/LICENSE.TXT
    mcopy -D o /tmp/LICENSE.TXT c:/LICENSE.TXT
elif [ -f "$DC/licenses/MIT.TXT" ]; then
    to_dos "$DC/licenses/MIT.TXT" /tmp/LICENSE.TXT
    mcopy -D o /tmp/LICENSE.TXT c:/LICENSE.TXT
fi

# =========================================================================
# 8. MS-DOS 4.0 production source archive (the 1988 IBM/Microsoft
#    release).  Zip up v4.0/src into a single archive on C:\SOURCE\
#    so users can read the production MS-DOS 4.00/4.01 source.
# =========================================================================
echo "Building MSDOS40-SRC.ZIP from microsoft/MS-DOS v4.0/src..."
SRCZIP="$IMGDIR/fd/source-cache/MSDOS40-SRC.ZIP"
if [ ! -f "$SRCZIP" ] || [ "$MSDOS_REPO/v4.0/src" -nt "$SRCZIP" ]; then
    rm -f "$SRCZIP"
    (cd "$MSDOS_REPO/v4.0" && zip -qr "$SRCZIP" src LICENSE)
fi

mmd c:/SOURCE 2>/dev/null || true
if [ -f "$SRCZIP" ]; then
    mcopy -D o "$SRCZIP" "c:/SOURCE/MSDOS40.ZIP"
    echo "  installed C:\\SOURCE\\MSDOS40.ZIP ($(ls -lh "$SRCZIP" | awk '{print $5}'))"
fi

# Also include the v4.0-ozzie SOURCE for the boot files (DISK2/BIOS
# has IBMBIO.ASM and IBMDOS.ASM, the source for the IBMBIO.COM /
# IBMDOS.COM we ship in the disk root).  Skip the binary disk image
# blobs and PDF manuals to keep the archive small enough to fit.
OZZIE_SRC_ZIP="$IMGDIR/fd/source-cache/MDOS4OZ-SRC.ZIP"
if [ ! -f "$OZZIE_SRC_ZIP" ]; then
    (cd "$MSDOS_REPO/v4.0-ozzie" && \
        zip -qr "$OZZIE_SRC_ZIP" bin/DISK1 bin/DISK2 \
            -x 'bin/DISK1/BIN/*' 'bin/DRDOS*')
fi
if [ -f "$OZZIE_SRC_ZIP" ]; then
    mcopy -D o "$OZZIE_SRC_ZIP" "c:/SOURCE/MDOS4OZ.ZIP"
    echo "  installed C:\\SOURCE\\MDOS4OZ.ZIP ($(ls -lh "$OZZIE_SRC_ZIP" | awk '{print $5}'))"
fi

# =========================================================================
# 9. Report contents
# =========================================================================
echo ""
echo "MS-DOS hard disk created: $OUTIMG"
echo "Size: $(ls -lh "$OUTIMG" | awk '{print $5}')"
echo ""
echo "Contents:"
mdir c: 2>/dev/null || echo "(mdir failed)"
echo ""
echo "Free space:"
minfo c: 2>/dev/null | grep -i "free\|total" || true

# =========================================================================
# 10. Update the catalog
# =========================================================================
bash "$IMGDIR/scripts/update_catalog.sh" "$OUTIMG" "$FINALIMG"
