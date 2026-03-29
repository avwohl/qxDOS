# File Transfer

The app includes two commands for transferring files between DOS and your device:

- **R** (Read) — copies a file from your device into DOS
- **W** (Write) — copies a file from DOS to your device

Files are stored in the qxDOS folder in the Files app on iPad and iPhone.

## Copying a File into DOS

```
C:\> R myfile.txt C:\MYFILE.TXT
File transferred.
```

The first argument is the filename on your device (in the qxDOS folder). The second argument is the DOS destination path.

## Copying a File out of DOS

```
C:\> W C:\DOCUMENT.TXT document.txt
File transferred.
```

The first argument is the DOS source path. The second argument is the filename to create on your device.

## Where Files Go

**iPad and iPhone:**

1. Open the **Files** app
2. Browse to **On My iPad** (or iPhone)
3. Open the **qxDOS** folder

Files you write with **W** appear here. Files you want to read with **R** should be placed here first using the Files app, AirDrop, or any other method.

**Mac (Catalyst):**

Files go to the app's container in `~/Library/Containers/com.awohl.qxDOS/Data/Documents/`. Open Finder, press Cmd+Shift+G, and paste that path.

## Tips

- Use bare filenames on the host side (no paths) — the app is sandboxed and can only access its own Documents folder
- DOS filenames follow the 8.3 convention: `MYFILE.TXT`, not `My Long File.txt`
- R creates (or overwrites) the DOS file
- W creates (or overwrites) the host file
- Files transfer one byte at a time — large files take a moment
- Both R.COM and W.COM are in `C:\FREEDOS\BIN` (on the PATH)

## Examples

Download a file from the web and copy it into DOS:

```
C:\> NET
C:\> HTGET http://example.com/GAME.ZIP GAME.ZIP
C:\> R game.zip C:\GAME.ZIP
```

Copy a DOS document to your device for sharing:

```
C:\> W C:\LETTER.TXT letter.txt
```

Then open the Files app and share `letter.txt` via AirDrop, email, etc.
