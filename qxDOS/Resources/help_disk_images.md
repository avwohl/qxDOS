# Creating Disk Images

## Quick Recipe

Create a 100MB bootable hard disk image compatible with DOSBox, QEMU, Bochs, and 86Box:

```
# Choose geometry
HEADS=16
SPT=63
CYLS=200
TOTAL=$((CYLS * HEADS * SPT))
PART_START=63
PART_SIZE=$((TOTAL - PART_START))

# 1. Create blank image
dd if=/dev/zero of=disk.img bs=512 count=$TOTAL

# 2. Write MBR partition table (use fdisk, sfdisk, or script)
#    Partition 1: start=63, type=06 (FAT16), active

# 3. Format with correct geometry
dd if=/dev/zero of=/tmp/part.img bs=512 count=$PART_SIZE
mkfs.fat -F 16 -n "MYDISK" -h $PART_START -S 512 -s 8 -g $HEADS/$SPT /tmp/part.img

# 4. Write partition into image
dd if=/tmp/part.img of=disk.img bs=512 seek=$PART_START conv=notrunc
```

The key step is **`-g $HEADS/$SPT`** on the mkfs.fat line. Without it, the BPB geometry won't match the partition table, and some emulators will misread the disk.

## How Hard Disk Images Work

A raw disk image is a flat file — sector 0 is the first 512 bytes, sector 1 is the next 512, and so on. Every emulator reads them the same way.

The complexity comes from **geometry**. Real hard disks were addressed by Cylinder, Head, and Sector (CHS). A disk with 16 heads and 63 sectors per track maps CHS address (C=1, H=0, S=1) to linear sector `1×16×63 + 0×63 + 0 = 1008`. If you get the geometry wrong, CHS-to-LBA translation lands on the wrong sector.

Three places store geometry, and they must agree:

| Location | What it stores | Set by |
|----------|---------------|--------|
| **MBR partition table** | CHS of partition start/end | fdisk, sfdisk, or your script |
| **FAT BPB** (boot sector) | Heads and sectors-per-track | mkfs.fat (`-g H/S`) |
| **Emulator config** | Drive geometry for BIOS INT 13h | DOSBox conf, QEMU `-drive`, CMOS |

If the BPB says 4 heads / 32 spt but the MBR uses 16 heads / 63 spt, the emulator detects the mismatch and may fall back to CHS translation with wrong parameters — corrupting reads.

## Choosing Geometry

Standard conventions for different image sizes:

| Image size | Cylinders | Heads | Sectors/Track | Notes |
|-----------|-----------|-------|---------------|-------|
| ≤ 504 MB | varies | 16 | 63 | Classic CHS limit |
| ≤ 8 GB | varies | 16 | 63 | LBA with CHS compat |
| Floppy 1.44MB | 80 | 2 | 18 | Standard 3.5" HD |
| Floppy 720KB | 80 | 2 | 9 | Standard 3.5" DD |

For hard disks, **16 heads / 63 sectors** is the safest choice. Adjust cylinders for the size you want:

```
size_in_MB = cylinders × 16 × 63 × 512 / 1048576
```

So 200 cylinders ≈ 100MB, 400 cylinders ≈ 200MB.

## FAT12 vs FAT16 vs FAT32

The FAT type depends on cluster count, not image size, but in practice:

| FAT type | Typical size range | mkfs.fat flag |
|----------|-------------------|---------------|
| FAT12 | Floppies, < 16MB | `-F 12` |
| FAT16 | 16MB – 2GB | `-F 16` |
| FAT32 | > 512MB | `-F 32` |

FAT16 is the most compatible choice for DOS hard disk images.

## The `-h` (Hidden Sectors) Flag

The BPB `hidden sectors` field tells DOS how many sectors precede the partition. For a partition starting at sector 63 (standard), use `-h 63`. If this is wrong, the FAT driver can't find the root directory.

## Cluster Size (`-s`)

Larger clusters waste space but allow larger volumes:

| Flag | Cluster size | Good for |
|------|-------------|----------|
| `-s 4` | 2 KB | Small images (< 64MB) |
| `-s 8` | 4 KB | Medium images (64–512MB) |
| `-s 16` | 8 KB | Large images (> 512MB) |

## Verifying an Image

Check that BPB geometry matches your intended geometry:

```python
import struct
with open('disk.img', 'rb') as f:
    f.seek(63 * 512)  # skip to partition boot sector
    boot = f.read(512)
    spt = struct.unpack_from('<H', boot, 0x18)[0]
    heads = struct.unpack_from('<H', boot, 0x1A)[0]
    print(f'BPB: heads={heads} spt={spt}')
```

Both values should match the geometry used in your MBR partition table.

## Compatibility Notes

- **DOSBox / DOSBox-staging**: Checks BPB vs disk geometry. Mismatches cause CHS fallback with wrong translation. Use correct BPB geometry.
- **QEMU**: Accepts raw images. Use `-drive file=disk.img,format=raw,geometry=C/H/S` to override geometry if needed.
- **Bochs**: Specify geometry in `bochsrc`: `ata0-master: type=disk, path=disk.img, cylinders=200, heads=16, spt=63`.
- **86Box / PCem**: Auto-detect from image size using standard translation tables. Correct BPB is still important for DOS.
- **SIMH**: Uses raw images with geometry specified in the emulator config. BPB must match.

The universal rule: **make the BPB match the geometry everyone else uses**, and the image will work everywhere.
