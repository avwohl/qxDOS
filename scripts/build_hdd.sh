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
OUTIMG="$IMGDIR/fd/freedos_hd.img"
SRCIMG="$IMGDIR/fd/freedos.img"

# Download FreeDOS boot floppy if not present
if [ ! -f "$SRCIMG" ]; then
  echo "Downloading FreeDOS 1.4 boot floppy..."
  # Original: https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/official/FD14BOOT.img
  curl -L --retry 3 --connect-timeout 30 -o "$SRCIMG" \
    https://www.awohl.com/freedos/freedos.img
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

echo "LiveCD ISO:  ${LIVEISO:-not found}"
echo "Bonus ISO:   ${BONUSISO:-not found}"

# Geometry: 16 heads, 63 sectors/track
HEADS=16
SPT=63
CYLS=407  # ~200MB
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
mkfs.fat -F 16 -n "FREEDOS" -h $PART_START -S 512 -s 8 -g $HEADS/$SPT /tmp/partition.img
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
install_pkg() {
  local zipfile="$1"
  local tmpdir="/tmp/fdpkg_$$"
  [ -f "$zipfile" ] || return 0
  mkdir -p "$tmpdir"
  unzip -o -q "$zipfile" -d "$tmpdir" -x 'SOURCE/*' 'source/*' 2>/dev/null || true
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
             MTCP.CFG NET.BAT COPYING.TXT; do
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
    echo "  Installed NE2000.COM, mTCP (FTP, TELNET, PING, HTGET, DHCP)"
fi

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
# 8. Catalog consistency check
# =========================================================================
echo ""
echo "--- Catalog consistency check ---"
WARNINGS=0

RELEASE_XML="$IMGDIR/release_assets/disks.xml"
BUNDLED_XML="$IMGDIR/iosFreeDOS/Resources/disks.xml"
DISK_NAME=$(basename "$OUTIMG")
ACTUAL_SIZE=$(stat -f%z "$OUTIMG")
ACTUAL_SHA=$(shasum -a 256 "$OUTIMG" | awk '{print $1}')

check_xml() {
  local label="$1" xmlfile="$2"
  if [ ! -f "$xmlfile" ]; then
    echo "  WARNING: $label not found: $xmlfile"
    WARNINGS=$((WARNINGS + 1))
    return
  fi
  eval "$(python3 -c "
import xml.etree.ElementTree as ET, sys
tree = ET.parse('$xmlfile')
for d in tree.findall('disk'):
    if d.findtext('filename') == '$DISK_NAME':
        print('xml_size=' + (d.findtext('size') or ''))
        print('xml_sha=' + (d.findtext('sha256') or ''))
        sys.exit(0)
print('xml_size='); print('xml_sha=')
")"
  if [ -z "$xml_size" ]; then
    echo "  WARNING: $DISK_NAME not found in $label"
    WARNINGS=$((WARNINGS + 1))
  elif [ "$xml_size" != "$ACTUAL_SIZE" ]; then
    echo "  WARNING: $label size mismatch: xml=$xml_size actual=$ACTUAL_SIZE"
    WARNINGS=$((WARNINGS + 1))
  fi
  if [ -n "$xml_sha" ] && [ "$xml_sha" != "$ACTUAL_SHA" ]; then
    echo "  WARNING: $label sha256 mismatch"
    echo "    xml:    $xml_sha"
    echo "    actual: $ACTUAL_SHA"
    WARNINGS=$((WARNINGS + 1))
  elif [ -z "$xml_sha" ]; then
    echo "  WARNING: $label has empty sha256 for $DISK_NAME"
    WARNINGS=$((WARNINGS + 1))
  fi
}

check_xml "release_assets/disks.xml" "$RELEASE_XML"
check_xml "Resources/disks.xml" "$BUNDLED_XML"

if [ -f "$RELEASE_XML" ] && [ -f "$BUNDLED_XML" ]; then
  if ! diff -q "$RELEASE_XML" "$BUNDLED_XML" > /dev/null 2>&1; then
    echo "  WARNING: release_assets/disks.xml and Resources/disks.xml differ"
    WARNINGS=$((WARNINGS + 1))
  fi
fi

if [ "$WARNINGS" -eq 0 ]; then
  echo "  All checks passed."
else
  echo ""
  echo "  $WARNINGS warning(s). Update disks.xml files to match the new disk image."
  echo "  Actual size: $ACTUAL_SIZE  sha256: $ACTUAL_SHA"
fi
