#!/bin/bash
# Build a small (~22MB) bootable FreeDOS starter disk.
# Minimal system with CWSDPMI and essential utilities.

set -e

IMGDIR="$(cd "$(dirname "$0")/.." && pwd)"
FINALIMG="$IMGDIR/fd/freedos_starter.img"
OUTIMG="/tmp/freedos_starter_build.img"
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

# --- Locate LiveCD ISO for system files ---
find_iso() {
  local name="$1"
  for dir in "$IMGDIR/fd" "$HOME/Downloads" "$HOME/Downloads/old" \
             "$HOME/Downloads/old/FD14-LiveCD"; do
    [ -f "$dir/$name" ] && echo "$dir/$name" && return
  done
}

LIVEISO=$(find_iso "FD14LIVE.iso" || true)

# Download LiveCD if not found -- the starter disk needs the FreeDOS
# utility set from the LiveCD; without it the starter is just the
# kernel + COMMAND.COM with no DIR/MEM/EDIT/etc.
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

# Geometry: 16 heads, 63 sectors/track
HEADS=16
SPT=63
# Bumped from 45 (~22 MB) to 65 (~32 MB) so the starter disk has
# room for the kernel + FreeCom + mTCP source archives, license
# texts, and attribution files.  GPL §3 compliance.
CYLS=65   # ~32MB
TOTAL_SECTORS=$((CYLS * HEADS * SPT))
PART_START=63
PART_SECTORS=$((TOTAL_SECTORS - PART_START))
IMG_SIZE=$((TOTAL_SECTORS * 512))

echo "Creating ${IMG_SIZE} byte ($((IMG_SIZE/1024/1024))MB) starter disk image..."
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
mkfs.fat -F 16 -n "FREEDOS" -h $PART_START -S 512 -s 4 -g $HEADS/$SPT /tmp/partition.img
dd if=/tmp/partition.img of="$OUTIMG" bs=512 seek=$PART_START conv=notrunc 2>/dev/null

# 4. Set up mtools
export MTOOLS_SKIP_CHECK=1
PART_OFFSET=$((PART_START * 512))
cat > /tmp/mtoolsrc_starter << EOF
mtools_skip_check=1
drive c: file="$OUTIMG" offset=$PART_OFFSET
EOF
export MTOOLSRC=/tmp/mtoolsrc_starter

# =========================================================================
# 5. Install minimal FreeDOS system
# =========================================================================

# Mount LiveCD if available
LIVE_MNT=""
DID_MOUNT_LIVE=""
cleanup_mounts() {
  [ -n "$DID_MOUNT_LIVE" ] && hdiutil detach "$LIVE_MNT" -quiet 2>/dev/null || true
}
trap cleanup_mounts EXIT

mount_iso() {
  local iso="$1" label="$2"
  for vol in /Volumes/${label}*; do
    [ -d "$vol" ] && echo "$vol" && return 0
  done
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

echo "Installing FreeDOS kernel..."
mcopy -i "$SRCIMG" ::KERNEL.SYS /tmp/KERNEL.SYS
mcopy -D o /tmp/KERNEL.SYS c:

if [ -n "$LIVE_MNT" ] && [ -d "$LIVE_MNT/freedos" ]; then
  echo "  Copying minimal FreeDOS system from LiveCD..."
  mmd c:/FREEDOS 2>/dev/null || true
  mmd c:/FREEDOS/BIN 2>/dev/null || true
  mmd c:/FREEDOS/NLS 2>/dev/null || true

  # Copy only essential binaries
  ESSENTIAL="COMMAND.COM CWSDPMI.EXE MEM.EXE MORE.COM TYPE.COM DIR.COM DELTREE.EXE EDIT.EXE XCOPY.EXE CHOICE.EXE FDISK.EXE FORMAT.EXE SYS.COM LABEL.EXE SHSUCDX.COM"
  for f in $ESSENTIAL; do
    if [ -f "$LIVE_MNT/freedos/bin/$f" ]; then
      mcopy -D o "$LIVE_MNT/freedos/bin/$f" "c:/FREEDOS/BIN/$f" 2>/dev/null || true
    elif [ -f "$LIVE_MNT/freedos/bin/$(echo $f | tr '[:upper:]' '[:lower:]')" ]; then
      mcopy -D o "$LIVE_MNT/freedos/bin/$(echo $f | tr '[:upper:]' '[:lower:]')" "c:/FREEDOS/BIN/$f" 2>/dev/null || true
    fi
  done

  # COMMAND.COM must be in root for boot
  if [ -f "$LIVE_MNT/COMMAND.COM" ]; then
    mcopy -D o "$LIVE_MNT/COMMAND.COM" c:
  fi
else
  echo "  No LiveCD ISO found - using boot floppy"
  mmd c:/FREEDOS 2>/dev/null || true
  mmd c:/FREEDOS/BIN 2>/dev/null || true
  mcopy -i "$SRCIMG" ::/FREEDOS/BIN/COMMAND.COM /tmp/COMMAND.COM
  mcopy -D o /tmp/COMMAND.COM c:
  mcopy -D o /tmp/COMMAND.COM c:/FREEDOS/BIN/
  for f in $(mdir -b -i "$SRCIMG" ::/FREEDOS/BIN/ 2>/dev/null | grep -v "^$"); do
    bn=$(echo "$f" | sed 's|.*/||')
    mcopy -i "$SRCIMG" "::FREEDOS/BIN/$bn" /tmp/"$bn" 2>/dev/null || true
    mcopy -D o /tmp/"$bn" "c:/FREEDOS/BIN/$bn" 2>/dev/null || true
  done
fi

# =========================================================================
# 6. Configuration
# =========================================================================
printf 'LASTDRIVE=Z\r\nFILES=40\r\nBUFFERS=20\r\nDOS=HIGH\r\nSHELL=C:\\COMMAND.COM C:\\ /E:1024 /P\r\n' > /tmp/FDCONFIG.SYS
mcopy -D o /tmp/FDCONFIG.SYS c:

printf '@ECHO OFF\r\nSET DOSDIR=C:\\FREEDOS\r\nSET PATH=C:\\FREEDOS\\BIN;C:\\NET\r\nSET NLSPATH=C:\\FREEDOS\\NLS\r\nSET TEMP=C:\\TEMP\r\nSET DIRCMD=/ON\r\nSET MTCPCFG=C:\\NET\\MTCP.CFG\r\nIF NOT EXIST C:\\TEMP\\NUL MD C:\\TEMP\r\nECHO.\r\nECHO FreeDOS ready. Type HELP for commands, EDIT to edit files.\r\nECHO Type FDNET to start networking (FTP, TELNET, PING).\r\nECHO.\r\n' > /tmp/AUTOEXEC.BAT
mcopy -D o /tmp/AUTOEXEC.BAT c:

mmd c:/TEMP 2>/dev/null || true

# Install DPMITEST.COM
if [ -f "$IMGDIR/dos/dpmitest.com" ]; then
    mcopy -D o "$IMGDIR/dos/dpmitest.com" "c:/DPMITEST.COM"
    echo "Installed DPMITEST.COM"
fi

# Install R.COM and W.COM (host file transfer)
for f in r.com w.com; do
    if [ -f "$IMGDIR/dos/$f" ]; then
        upper=$(echo "$f" | tr '[:lower:]' '[:upper:]')
        mcopy -D o "$IMGDIR/dos/$f" "c:/FREEDOS/BIN/$upper"
        echo "Installed $upper"
    fi
done

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
                    # Ensure DOS line endings (CR+LF) for text files
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
echo "Installing license texts and attribution files..."
DC="$IMGDIR/disk-content"

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

# Source archives -- shared cache with build_hdd.sh
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
echo "Starter disk created: $OUTIMG"
echo "Size: $(ls -lh "$OUTIMG" | awk '{print $5}')"
echo ""
echo "Contents:"
mdir c: 2>/dev/null || echo "(mdir failed)"
echo ""
echo "Free space:"
minfo c: 2>/dev/null | grep -i "free\|total" || true

# =========================================================================
# 8. Compare with existing image and update catalog if changed
# =========================================================================
bash "$IMGDIR/scripts/update_catalog.sh" "$OUTIMG" "$FINALIMG"
