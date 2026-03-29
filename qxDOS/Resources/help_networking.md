# Networking

FreeDOS includes NE2000 Ethernet networking with SLIRP, a virtual NAT router that connects DOS to the internet through your device's network connection. The disk comes with mTCP — a TCP/IP suite with FTP, Telnet, Ping, and an HTTP downloader.

## Quick Start

Type `FDNET` at the DOS prompt:

```
C:\> FDNET
Loading NE2000 packet driver...
Getting IP address via DHCP...
Network is ready.
```

That's it. The `FDNET` command loads the network driver and gets an IP address automatically. You only need to run it once per session.

## Transferring Files

Use FTP to move files in and out of DOS:

```
C:\> FTP ftp.example.com
```

Or download files from the web:

```
C:\> HTGET http://example.com/program.zip PROGRAM.ZIP
```

## Available Commands

After running `NET`, these commands are available:

| Command | Description |
|---------|-------------|
| `FTP host` | Connect to an FTP server |
| `TELNET host [port]` | Open a remote terminal session |
| `PING host` | Test network connectivity |
| `HTGET url file` | Download a file via HTTP |

All commands are in `C:\NET` which is on the PATH.

## Manual Setup

If you prefer to set things up manually instead of using `NET`:

### 1. Load the Packet Driver

```
C:\> C:\NET\NE2000 0x60 3 0x300
```

Arguments: software interrupt (0x60), IRQ (3), I/O base (0x300).

### 2. Set mTCP Config

```
C:\> SET MTCPCFG=C:\NET\MTCP.CFG
```

This is already set in AUTOEXEC.BAT.

### 3. Get an IP Address

```
C:\> DHCP
```

SLIRP's DHCP server assigns 10.0.2.15 with gateway 10.0.2.2.

## Network Details

| Setting | Value |
|---------|-------|
| NIC | NE2000 at I/O 0x300, IRQ 3 |
| Guest IP | 10.0.2.15 (via DHCP) |
| Gateway | 10.0.2.2 |
| DNS | 10.0.2.3 |
| Network | 10.0.2.0/24 |

## Troubleshooting

- **FDNET says "Could not load NE2000"** — the driver may already be loaded. Only run FDNET once per session.
- **DHCP timeout** — make sure your iPad/Mac has an internet connection.
- **Can't resolve hostnames** — use IP addresses, or check that DHCP completed successfully.
- **Slow transfers** — normal for emulated NE2000 over SLIRP. Large files take time.

## Software Licenses

- **NE2000.COM** — Crynwr packet driver (open source)
- **mTCP** (FTP, TELNET, DHCP, PING, HTGET) — GPLv3, by Michael Brutman (brutman.com/mTCP)
