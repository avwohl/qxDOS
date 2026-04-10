#!/bin/bash
# Build a bootable FreeDOS hard disk image (system only, no games)
# Creates a FAT16 HDD image with MBR + partition table
#
# Content sources (searched in order):
#   1. FreeDOS 1.4 LiveCD ISO  - full system (230 utilities) + packages
#   2. FreeDOS 1.4 Bonus CD ISO - editors, disk tools
#   3. fd/freedos.img           - fallback: boot floppy (minimal)

set -e

IMGDIR="$(cd "$(dirname "$0")/.." && pwd)"
FINALIMG="$IMGDIR/fd/freedos_hd.img"
OUTIMG="/tmp/freedos_hd_build.img"
SRCIMG="$IMGDIR/fd/freedos.img"

# Download FreeDOS boot floppy if not present (or cached file is bogus)
if [ ! -f "$SRCIMG" ] || [ "$(stat -f%z "$SRCIMG" 2>/dev/null || stat -c%s "$SRCIMG")" != "1474560" ]; then
  echo "Downloading FreeDOS 1.4 FloppyEdition zip..."
  curl -L --retry 3 --connect-timeout 30 -o /tmp/fd14floppy.zip \
    https://download.freedos.org/1.4/FD14-FloppyEdition.zip
  echo "Extracting 1.44 MB boot floppy..."
  rm -rf /tmp/fd14floppy
  mkdir -p /tmp/fd14floppy
  unzip -j -o /tmp/fd14floppy.zip '144m/x86BOOT.img' -d /tmp/fd14floppy
  cp /tmp/fd14floppy/x86BOOT.img "$SRCIMG"
fi

# --- Locate ISOs ---
find_iso() {
  local name="$1"
  for dir in "$IMGDIR/fd" "$HOME/Downloads" "$HOME/Downloads/old" \
             "$HOME/Downloads/old/FD14-LiveCD" "$HOME/Downloads/old/FD14-BonusCD"; do
    [ -f "$dir/$name" ] && echo "$dir/$name" && return
  done
}

LIVEISO=$(find_iso "FD14LIVE.iso" || true)
BONUSISO=$(find_iso "FD14BNS.iso" || true)

# Download LiveCD if not found
if [ -z "$LIVEISO" ]; then
  LIVEISO="$IMGDIR/fd/FD14LIVE.iso"
  LIVEZIP="$IMGDIR/fd/FD14-LiveCD.zip"
  echo "Downloading FreeDOS 1.4 LiveCD..."
  curl -L --retry 3 --connect-timeout 30 -o "$LIVEZIP" \
    https://download.freedos.org/1.4/FD14-LiveCD.zip
  echo "Extracting LiveCD ISO..."
  unzip -o -q "$LIVEZIP" -d "$IMGDIR/fd/"
  rm -f "$LIVEZIP"
  if [ ! -f "$LIVEISO" ]; then
    echo "ERROR: Expected $LIVEISO after extraction"
    ls "$IMGDIR/fd/"*.iso 2>/dev/null
    exit 1
  fi
fi

echo "LiveCD ISO:  ${LIVEISO:-not found}"
echo "Bonus ISO:   ${BONUSISO:-not found}"

# Geometry: 16 heads, 63 sectors/track
HEADS=16
SPT=63
# Bumped from 407 (~200 MB) to 650 (~320 MB) to make room for the
# FreeDOS package source trees that now travel with every binary
# (GPL §3 compliance), plus the kernel/FreeCom/mTCP source archives
# under C:\SOURCE\.
CYLS=650  # ~320MB
TOTAL_SECTORS=$((CYLS * HEADS * SPT))
PART_START=63
PART_SECTORS=$((TOTAL_SECTORS - PART_START))
IMG_SIZE=$((TOTAL_SECTORS * 512))

echo "Creating ${IMG_SIZE} byte ($((IMG_SIZE/1024/1024))MB) hard disk image..."
echo "Geometry: C=$CYLS H=$HEADS S=$SPT = $TOTAL_SECTORS sectors"

# 1. Create blank image
dd if=/dev/zero of="$OUTIMG" bs=512 count=$TOTAL_SECTORS 2>/dev/null

# 2. Write MBR boot code + partition table
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
" > /tmp/mbr.bin

dd if=/tmp/mbr.bin of="$OUTIMG" bs=512 count=1 conv=notrunc 2>/dev/null

# 3. Format partition as FAT16
dd if=/dev/zero of=/tmp/partition.img bs=512 count=$PART_SECTORS 2>/dev/null
# -s 16 (8 KB clusters) is required for FAT16 on a ~320 MB partition;
# anything smaller produces >65525 clusters, which mkfs.fat rejects.
mkfs.fat -F 16 -n "FREEDOS" -h $PART_START -S 512 -s 16 -g $HEADS/$SPT /tmp/partition.img
dd if=/tmp/partition.img of="$OUTIMG" bs=512 seek=$PART_START conv=notrunc 2>/dev/null

# 4. Set up mtools
export MTOOLS_SKIP_CHECK=1
PART_OFFSET=$((PART_START * 512))
cat > /tmp/mtoolsrc_hd << EOF
mtools_skip_check=1
drive c: file="$OUTIMG" offset=$PART_OFFSET
EOF
export MTOOLSRC=/tmp/mtoolsrc_hd

# =========================================================================
# 5. Install FreeDOS system
# =========================================================================

# --- Mount ISOs if available ---
LIVE_MNT=""
BONUS_MNT=""
DID_MOUNT_LIVE=""
DID_MOUNT_BONUS=""
cleanup_mounts() {
  [ -n "$DID_MOUNT_LIVE" ] && hdiutil detach "$LIVE_MNT" -quiet 2>/dev/null || true
  [ -n "$DID_MOUNT_BONUS" ] && hdiutil detach "$BONUS_MNT" -quiet 2>/dev/null || true
}
trap cleanup_mounts EXIT

# Mount ISO or find existing mount
mount_iso() {
  local iso="$1" label="$2"
  # Check well-known mount points first
  for vol in /Volumes/${label}*; do
    [ -d "$vol" ] && echo "$vol" && return 0
  done
  # Try to mount
  local out=$(hdiutil attach "$iso" -readonly -nobrowse 2>&1)
  if [ $? -eq 0 ]; then
    echo "$out" | tail -1 | sed 's/.*	//'
    echo "NEW" >&2
    return 0
  fi
  return 1
}

if [ -n "$LIVEISO" ]; then
  LIVE_MNT=$(mount_iso "$LIVEISO" "FD14-Live" 2>/tmp/mount_live_status)
  if [ -z "$LIVE_MNT" ]; then
    echo "  WARNING: Failed to mount LiveCD"
  else
    echo "  LiveCD at $LIVE_MNT"
    grep -q NEW /tmp/mount_live_status 2>/dev/null && DID_MOUNT_LIVE=1
  fi
fi
if [ -n "$BONUSISO" ]; then
  BONUS_MNT=$(mount_iso "$BONUSISO" "FD14-BONUS" 2>/tmp/mount_bonus_status)
  if [ -z "$BONUS_MNT" ]; then
    echo "  WARNING: Failed to mount Bonus CD"
  else
    echo "  Bonus CD at $BONUS_MNT"
    grep -q NEW /tmp/mount_bonus_status 2>/dev/null && DID_MOUNT_BONUS=1
  fi
fi

# Helper: recursively copy a host directory to the FAT image
copy_tree() {
  local src="$1" dest="$2"
  [ -d "$src" ] || return 0
  mmd "$dest" 2>/dev/null || true
  for f in "$src"/*; do
    local bn=$(basename "$f")
    if [ -f "$f" ]; then
      mcopy -D o "$f" "$dest/$bn" 2>/dev/null || true
    elif [ -d "$f" ]; then
      copy_tree "$f" "$dest/$bn"
    fi
  done
}

# Helper: extract a FreeDOS package ZIP into C:\FREEDOS
#
# IMPORTANT (GPL §3 compliance): we used to pass `-x 'SOURCE/*' 'source/*'`
# here to skip the source trees and save disk space.  That was wrong --- many
# of the packages are GPL and the LiveCD ships its source archive *inside*
# each package zip exactly so that distributors can satisfy §3(a) ("the
# corresponding source code, on a medium customarily used for software
# interchange") just by passing the package along.  Stripping the SOURCE/
# tree turns binary redistribution into a license violation.
#
# We now extract the entire zip --- BIN, DOC, NLS, HELP, *and* SOURCE --- so
# the source code travels with the binaries on every disk we ship.  The
# SOURCE/ tree lands at  C:\FREEDOS\<PKG>\SOURCE\  next to the binaries.
install_pkg() {
  local zipfile="$1"
  local tmpdir="/tmp/fdpkg_$$"
  [ -f "$zipfile" ] || return 0
  mkdir -p "$tmpdir"
  unzip -o -q "$zipfile" -d "$tmpdir" 2>/dev/null || true
  for d in "$tmpdir"/*/; do
    [ -d "$d" ] || continue
    local dname=$(basename "$d" | tr '[:lower:]' '[:upper:]')
    copy_tree "$d" "c:/FREEDOS/$dname"
  done
  for f in "$tmpdir"/*; do
    [ -f "$f" ] && mcopy -D o "$f" "c:/FREEDOS/$(basename "$f")" 2>/dev/null || true
  done
  rm -rf "$tmpdir"
}

echo "Installing FreeDOS kernel..."
mcopy -i "$SRCIMG" ::KERNEL.SYS /tmp/KERNEL.SYS
mcopy -D o /tmp/KERNEL.SYS c:

if [ -n "$LIVE_MNT" ] && [ -d "$LIVE_MNT/freedos" ]; then
  echo "  Copying pre-installed FreeDOS system from LiveCD (230 utilities)..."
  copy_tree "$LIVE_MNT/freedos" "c:/FREEDOS"

  # COMMAND.COM must be in root for boot
  if [ -f "$LIVE_MNT/COMMAND.COM" ]; then
    mcopy -D o "$LIVE_MNT/COMMAND.COM" c:
  fi

  # Install additional LiveCD packages (system tools, no games)
  echo "  Installing LiveCD packages..."
  for cat in archiver apps tools unix; do
    pkgdir="$LIVE_MNT/packages/$cat"
    [ -d "$pkgdir" ] || continue
    count=$(ls "$pkgdir"/*.zip 2>/dev/null | wc -l)
    echo "    $cat ($count packages)"
    for z in "$pkgdir"/*.zip; do install_pkg "$z"; done
  done

  # Install Bonus CD packages (editors, utilities)
  if [ -n "$BONUS_MNT" ]; then
    echo "  Installing Bonus CD packages..."
    for cat in edit disk util; do
      pkgdir="$BONUS_MNT/packages/$cat"
      [ -d "$pkgdir" ] || continue
      count=$(ls "$pkgdir"/*.zip 2>/dev/null | wc -l)
      echo "    $cat ($count packages)"
      for z in "$pkgdir"/*.zip; do install_pkg "$z"; done
    done
  fi
else
  echo "  No LiveCD ISO found - using minimal boot floppy install"
  mmd c:/FREEDOS 2>/dev/null || true
  mmd c:/FREEDOS/BIN 2>/dev/null || true
  mmd c:/FREEDOS/NLS 2>/dev/null || true

  for f in $(mdir -b -i "$SRCIMG" ::/FREEDOS/BIN/ 2>/dev/null | grep -v "^$"); do
      bn=$(echo "$f" | sed 's|.*/||')
      mcopy -i "$SRCIMG" "::FREEDOS/BIN/$bn" /tmp/"$bn" 2>/dev/null || true
      mcopy -D o /tmp/"$bn" "c:/FREEDOS/BIN/$bn" 2>/dev/null || true
  done
  mcopy -i "$SRCIMG" ::/FREEDOS/BIN/COMMAND.COM /tmp/COMMAND.COM
  mcopy -D o /tmp/COMMAND.COM c:

  for f in $(mdir -b -i "$SRCIMG" ::/FREEDOS/NLS/ 2>/dev/null | grep -v "^$"); do
      bn=$(echo "$f" | sed 's|.*/||')
      mcopy -i "$SRCIMG" "::FREEDOS/NLS/$bn" /tmp/"$bn" 2>/dev/null || true
      mcopy -D o /tmp/"$bn" "c:/FREEDOS/NLS/$bn" 2>/dev/null || true
  done
fi

# =========================================================================
# 6. Configuration
# =========================================================================
printf 'LASTDRIVE=Z\r\nFILES=40\r\nBUFFERS=20\r\nDOS=HIGH\r\nSHELL=C:\\COMMAND.COM C:\\ /E:1024 /P\r\n' > /tmp/FDCONFIG.SYS
mcopy -D o /tmp/FDCONFIG.SYS c:

printf '@ECHO OFF\r\nSET DOSDIR=C:\\FREEDOS\r\nSET PATH=C:\\FREEDOS\\BIN;C:\\NET\r\nSET NLSPATH=C:\\FREEDOS\\NLS\r\nSET TEMP=C:\\TEMP\r\nSET DIRCMD=/OGN\r\nSET MTCPCFG=C:\\NET\\MTCP.CFG\r\nPROMPT $P$G\r\nIF NOT EXIST C:\\TEMP\\NUL MD C:\\TEMP\r\nC:\\FREEDOS\\BIN\\CWSDPMI -p\r\n' > /tmp/AUTOEXEC.BAT
mcopy -D o /tmp/AUTOEXEC.BAT c:

mmd c:/TEMP 2>/dev/null || true

# Install R.COM and W.COM
if [ -f "$IMGDIR/dos/r.com" ]; then
    mcopy -D o "$IMGDIR/dos/r.com" "c:/FREEDOS/BIN/R.COM"
    mcopy -D o "$IMGDIR/dos/w.com" "c:/FREEDOS/BIN/W.COM"
    echo "Installed R.COM and W.COM"
fi

# Install DPMITEST.COM (DPMI diagnostic tool)
if [ -f "$IMGDIR/dos/dpmitest.com" ]; then
    mcopy -D o "$IMGDIR/dos/dpmitest.com" "c:/DPMITEST.COM"
    echo "Installed DPMITEST.COM"
fi

# =========================================================================
# Install networking tools (NE2000 packet driver + mTCP)
# =========================================================================
if [ -d "$IMGDIR/dos/net" ]; then
    echo "Installing networking tools..."
    mmd c:/NET 2>/dev/null || true
    for f in NE2000.COM DHCP.EXE FTP.EXE TELNET.EXE PING.EXE HTGET.EXE \
             MTCP.CFG NET.BAT FDNET.BAT COPYING.TXT; do
        if [ -f "$IMGDIR/dos/net/$f" ]; then
            case "$f" in
                *.BAT|*.CFG|*.TXT)
                    sed 's/\r$//' "$IMGDIR/dos/net/$f" | sed 's/$/'$'\r''/' > "/tmp/$f"
                    mcopy -D o "/tmp/$f" "c:/NET/$f"
                    ;;
                *)
                    mcopy -D o "$IMGDIR/dos/net/$f" "c:/NET/$f"
                    ;;
            esac
        fi
    done
    # Install FDNET.BAT to FREEDOS\BIN so it's on PATH (standard FreeDOS location)
    if [ -f "$IMGDIR/dos/net/FDNET.BAT" ]; then
        sed 's/\r$//' "$IMGDIR/dos/net/FDNET.BAT" | sed 's/$/'$'\r''/' > "/tmp/FDNET.BAT"
        mcopy -D o "/tmp/FDNET.BAT" "c:/FREEDOS/BIN/FDNET.BAT"
    fi
    echo "  Installed NE2000.COM, mTCP (FTP, TELNET, PING, HTGET, DHCP), FDNET"
fi

# =========================================================================
# Install attribution / license / source files (GPL §3 compliance)
# =========================================================================
#
# Every disk we ship has to carry the license texts and the
# corresponding source for any GPL binary on the disk.  We do this
# by:
#
#   1. Copying disk-content/freedos/*.TXT to the root of the disk.
#   2. Copying disk-content/licenses/*.TXT to C:\LICENSE\.
#   3. Downloading kernel + freecom + mTCP source archives once and
#      caching them under  fd/source-cache/ , then copying them to
#      C:\SOURCE\ on the disk.  The FreeDOS package sources are
#      already on the disk because install_pkg() no longer strips
#      SOURCE/ from the package zips.
#
echo "Installing license texts and attribution files..."
DC="$IMGDIR/disk-content"

# DOS-line-ending the host text files into /tmp before mcopying
to_dos() {
    sed 's/\r$//' "$1" | sed 's/$/'$'\r''/' > "$2"
}

if [ -d "$DC/freedos" ]; then
    for f in README.TXT CREDITS.TXT SOURCE.TXT; do
        if [ -f "$DC/freedos/$f" ]; then
            to_dos "$DC/freedos/$f" "/tmp/$f"
            mcopy -D o "/tmp/$f" "c:/$f"
        fi
    done
fi

mmd c:/LICENSE 2>/dev/null || true
if [ -d "$DC/licenses" ]; then
    for f in GPL2.TXT GPL3.TXT MIT.TXT; do
        if [ -f "$DC/licenses/$f" ]; then
            to_dos "$DC/licenses/$f" "/tmp/$f"
            mcopy -D o "/tmp/$f" "c:/LICENSE/$f"
        fi
    done
fi

# Source archives -- cache under fd/source-cache/ so we don't
# re-download on every build.
SRCCACHE="$IMGDIR/fd/source-cache"
mkdir -p "$SRCCACHE"

fetch_source() {
    local out="$1"
    local url="$2"
    if [ ! -f "$out" ]; then
        echo "  fetching $(basename "$out") from $url"
        curl -L --retry 3 --connect-timeout 30 -fsS -o "$out.tmp" "$url" \
            && mv "$out.tmp" "$out" \
            || { echo "  WARNING: download failed for $(basename "$out")"; rm -f "$out.tmp"; }
    fi
}

echo "Installing source archives (GPL §3)..."
fetch_source "$SRCCACHE/KERNEL-SRC.ZIP" \
    "https://github.com/FDOS/kernel/archive/refs/heads/master.zip"
fetch_source "$SRCCACHE/FREECOM-SRC.ZIP" \
    "https://github.com/FDOS/freecom/archive/refs/heads/master.zip"
fetch_source "$SRCCACHE/MTCP-SRC.ZIP" \
    "https://www.brutman.com/mTCP/download/mTCP-src_2025-01-10.zip"

mmd c:/SOURCE 2>/dev/null || true
for f in KERNEL-SRC.ZIP FREECOM-SRC.ZIP MTCP-SRC.ZIP; do
    if [ -f "$SRCCACHE/$f" ]; then
        mcopy -D o "$SRCCACHE/$f" "c:/SOURCE/$f"
        echo "  installed C:\\SOURCE\\$f ($(ls -lh "$SRCCACHE/$f" | awk '{print $5}'))"
    else
        echo "  WARNING: $f missing -- disk will violate GPL §3 until rebuilt with network"
    fi
done

# =========================================================================
# 7. Boot sector
# =========================================================================
dd if="$SRCIMG" of=/tmp/floppy_boot.bin bs=512 count=1 2>/dev/null

python3 -c "
import struct, sys
floppy = open('/tmp/floppy_boot.bin', 'rb').read()
partition = open('$OUTIMG', 'rb')
partition.seek($PART_START * 512)
part_boot = bytearray(partition.read(512))
partition.close()
result = bytearray(part_boot)
result[0:3] = floppy[0:3]
result[3:11] = floppy[3:11]
struct.pack_into('<I', result, 0x1C, $PART_START)
result[0x3E:510] = floppy[0x3E:510]
result[510] = 0x55; result[511] = 0xAA
sys.stdout.buffer.write(bytes(result))
" > /tmp/new_boot.bin

dd if=/tmp/new_boot.bin of="$OUTIMG" bs=512 seek=$PART_START count=1 conv=notrunc 2>/dev/null

echo "Patching boot sector for FAT16..."
python3 -c "
import sys
data = bytearray(open('$OUTIMG', 'rb').read())
boot_off = $PART_START * 512
fat12 = bytes([0xAB,0x89,0xC6,0x01,0xF6,0x01,0xC6,0xD1,0xEE,0xAD,
               0x73,0x04,0xB1,0x04,0xD3,0xE8,0x80,0xE4,0x0F,0x3D,
               0xF8,0x0F,0x72,0xE8])
actual = bytes(data[boot_off+0xFC : boot_off+0xFC+24])
if actual != fat12:
    print('WARNING: Boot sector bytes differ, skipping FAT16 patch')
    sys.exit(0)
fat16 = bytes([0xAB,0x89,0xC6,0x01,0xF6,0xAD,0x3D,0xF8,0xFF,
               0x72,0xF5] + [0x90]*13)
data[boot_off+0xFC : boot_off+0xFC+24] = fat16
open('$OUTIMG', 'wb').write(data)
print('  FAT16 cluster chain patch applied')
"

echo ""
echo "Disk image created: $OUTIMG"
echo "Size: $(ls -lh "$OUTIMG" | awk '{print $5}')"
echo ""
echo "Contents:"
mdir c: 2>/dev/null || echo "(mdir failed)"
echo ""
echo "Free space:"
minfo c: 2>/dev/null | grep -i "free\|total" || true

# Dev copy
DEVIMG="$IMGDIR/fd/freedos_hd_dev.img"
cp "$OUTIMG" "$DEVIMG"
echo ""
echo "Dev copy: $DEVIMG"

# =========================================================================
# 8. Compare with existing image and update catalog if changed
# =========================================================================
bash "$IMGDIR/scripts/update_catalog.sh" "$OUTIMG" "$FINALIMG"
