# Quick Start Guide

Welcome to a DOS emulator for iPhone, iPad, and Mac, powered by DOSBox Staging.

## Choose a DOS

In the **Machine** section, pick which DOS to run:

- **FreeDOS** — boots the FreeDOS kernel from disk. Full open-source DOS with 230+ utilities. This is the default.
- **MS-DOS** — boots MS-DOS from disk. Bring your own install media, or use MS-DOS 4.0 (MIT license) from the catalog.
- **DOSBox DOS** — DOSBox's built-in kernel and shell. No OS on disk needed. ~30 utilities on Z: drive.

## First Boot (FreeDOS)

1. Open the app and scroll to **Disk Catalog**
2. Download **FreeDOS Starter** (22MB) or **FreeDOS Hard Disk** (200MB)
3. Tap **Use as C:**
4. Set **Boot** to **Hard Disk C:**
5. Tap **Start Emulator**

You should see FreeDOS boot to a `C:\>` prompt.

## Installing from Scratch

To install any DOS onto a blank disk (like a real PC):

1. Set the DOS type in **Machine** settings
2. Create a blank hard disk via **Create Blank Disk**
3. Load install media (boot floppy and/or CD) from the catalog or Files
4. Set **Boot** to **Floppy A:**
5. Start — the OS installer runs and installs to C:
6. After install, remove the floppy and set boot to **Hard Disk C:**

## Keyboard

The on-screen keyboard has special keys in the left sidebar:

- **Ctrl** — toggles Ctrl mode (next key sent as Ctrl+key)
- **Alt** — toggles Alt mode
- **Fn** — toggles function key mode
- **Esc** — sends the Escape key
- **Enter** — sends the Enter key
- **Tab** — sends the Tab key

On a hardware keyboard (iPad, Mac), all keys work normally including F1-F12.

## Mouse

Touch the screen to control the mouse:

- **Tap** — left click
- **Long press** — right click
- **Drag** — move mouse with button held

Enable or disable the mouse in the Peripherals settings.

## Touch Controls

For games, enable virtual touch controls from the left sidebar:

- Tap the **gamecontroller** icon to choose a layout
- Tap the **eye** icon to show/hide the overlay
- Built-in presets available for DOOM, Duke Nukem 3D, and general FPS games
- Create custom layouts with the layout editor

## Disks

FreeDOS supports floppy disks (A: and B:), hard disks (C: and D:), and CD-ROM ISOs.

- Use the **Disk Catalog** to download ready-made images
- Use **Download from URL** to load images from the internet
- Use **Load from Files** to import images from your device
- Use **Create Blank Disk** to make empty floppy or hard disk images

The catalog includes FreeDOS boot images, Simtel MS-DOS archives, and Walnut Creek
CD-ROM collections from archive.org. ZIP downloads are extracted automatically.

## Configuration Profiles

Save different machine setups as named profiles. Each profile remembers:

- Machine type, CPU type, CPU speed, RAM, peripherals
- Which disk images are loaded
- Touch control layout
- Boot drive selection

Use the profile picker at the top of the settings screen to switch between them.

## File Transfer

Move files between DOS and your device with two built-in commands:

```
C:\> R myfile.txt C:\MYFILE.TXT    (copy from device into DOS)
C:\> W C:\DOCUMENT.TXT document.txt (copy from DOS to device)
```

Files are stored in the FreeDOS folder in the Files app. See the File Transfer help topic for details.

## Networking

Type `FDNET` at the DOS prompt to start networking:

```
C:\> FDNET
```

This loads the NE2000 network driver and gets an IP address. Once connected, use `FTP` to transfer files, `TELNET` for remote terminals, or `HTGET` to download files from the web. See the Networking help topic for details.

## Boot Drive

Select which drive to boot from in the **Boot** section:

- **Floppy A:** — boot from the floppy disk in drive A (used during OS installation)
- **Hard Disk C:** — boot from the hard disk (normal operation)
- **CD-ROM** — boot from a CD-ROM ISO image (requires a boot floppy with CD drivers)

In DOSBox DOS mode, the boot drive selects which drive to switch to at startup (no actual boot occurs).
