#!/bin/bash
# End-to-end disk catalog check.
# Tests exactly what the running app will experience.
set -e

IMGDIR="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_XML="$IMGDIR/release_assets/disks.xml"
BUNDLED_XML="$IMGDIR/qxDOS/Resources/disks.xml"
BUILT_XML=""
FD_DIR="$IMGDIR/fd"
APP_DISKS_DIR="$HOME/Documents/Disks"
ERRORS=0

err() { echo "  ERROR: $*"; ERRORS=$((ERRORS + 1)); }
ok()  { echo "  OK: $*"; }

echo "=== Disk Catalog End-to-End Check ==="

# ---------------------------------------------------------------
# 1. Source XML files
# ---------------------------------------------------------------
echo ""
echo "--- Source XML files ---"
[ -f "$RELEASE_XML" ] && ok "release_assets/disks.xml exists" || err "release_assets/disks.xml missing"
[ -f "$BUNDLED_XML" ] && ok "Resources/disks.xml exists" || err "Resources/disks.xml missing"
if [ -f "$RELEASE_XML" ] && [ -f "$BUNDLED_XML" ]; then
  if diff -q "$RELEASE_XML" "$BUNDLED_XML" > /dev/null 2>&1; then
    ok "both copies are identical"
  else
    err "release_assets/disks.xml and Resources/disks.xml DIFFER — run: cp release_assets/disks.xml qxDOS/Resources/disks.xml"
  fi
fi

# ---------------------------------------------------------------
# 2. Built app
# ---------------------------------------------------------------
echo ""
echo "--- Built app ---"
BUILT_APP=$(find "$HOME/Library/Developer/Xcode/DerivedData" -path "*/qxDOS*/Build/Products/*-maccatalyst/qxDOS.app/Contents/Resources/disks.xml" 2>/dev/null | head -1)
if [ -n "$BUILT_APP" ]; then
  if diff -q "$RELEASE_XML" "$BUILT_APP" > /dev/null 2>&1; then
    ok "built app has current disks.xml"
  else
    err "built app has STALE disks.xml — rebuild the app"
  fi
else
  echo "  SKIP: no built app found"
fi

# ---------------------------------------------------------------
# 3. Cached catalog (what the running app loaded last time)
# ---------------------------------------------------------------
echo ""
echo "--- App cached state (~/Documents/Disks/) ---"
CACHED="$APP_DISKS_DIR/disks_catalog.xml"
if [ -f "$CACHED" ]; then
  if diff -q "$RELEASE_XML" "$CACHED" > /dev/null 2>&1; then
    ok "cached catalog matches source"
  else
    err "cached catalog is STALE — the running app is using old data"
    echo "       Fix: rm '$CACHED'"
  fi
else
  ok "no cached catalog (app will use bundled)"
fi

# ---------------------------------------------------------------
# 4. Every disk entry: sizes, hashes, URLs, auto-download
# ---------------------------------------------------------------
echo ""
echo "--- Disk entries ---"
python3 << 'PYEOF'
import xml.etree.ElementTree as ET
import os, hashlib, sys, urllib.request, urllib.error

imgdir = os.environ.get("IMGDIR", ".")
release_xml = os.path.join(imgdir, "release_assets", "disks.xml")
fd_dir = os.path.join(imgdir, "fd")
app_disks = os.path.expanduser("~/Documents/Disks")
base_url = "https://github.com/avwohl/qxDOS2/releases/latest/download"
errors = 0

def err(msg):
    global errors
    print(f"  ERROR: {msg}")
    errors += 1

def ok(msg):
    print(f"  OK: {msg}")

tree = ET.parse(release_xml)
root = tree.getroot()

version = root.get("version", "")
if not version:
    err("catalog has no version attribute")
else:
    ok(f"catalog version={version}")

for disk in root.findall("disk"):
    fn = disk.findtext("filename", "")
    name = disk.findtext("name", "")
    xml_size = disk.findtext("size", "")
    xml_sha = disk.findtext("sha256", "")
    explicit_url = disk.findtext("url", "")
    default_drive = disk.findtext("defaultDrive", "")
    download_url = explicit_url if explicit_url else f"{base_url}/{fn}"

    print(f"")
    print(f"  [{fn}] {name}")

    # --- Local build copy in fd/ ---
    local_path = os.path.join(fd_dir, fn)
    if os.path.exists(local_path):
        actual_size = os.path.getsize(local_path)
        if xml_size and int(xml_size) != actual_size:
            err(f"size mismatch: xml={xml_size} actual={actual_size}")
        elif xml_size:
            ok(f"size={actual_size}")

        sha = hashlib.sha256(open(local_path, "rb").read()).hexdigest()
        if xml_sha and xml_sha.lower() != sha:
            err(f"sha256 mismatch: xml={xml_sha[:16]}... actual={sha[:16]}...")
        elif xml_sha:
            ok(f"sha256 matches")
        else:
            err(f"empty <sha256> in catalog, actual={sha}")

    # --- Already downloaded by app? ---
    app_copy = os.path.join(app_disks, fn)
    if os.path.exists(app_copy):
        ok(f"app has downloaded copy at ~/Documents/Disks/{fn}")
    else:
        # Not downloaded yet — check if the URL works
        reachable = False
        try:
            req = urllib.request.Request(download_url, method="HEAD")
            req.add_header("User-Agent", "check_disks/1.0")
            resp = urllib.request.urlopen(req, timeout=10)
            ok(f"download URL reachable ({resp.status})")
            reachable = True
        except urllib.error.HTTPError as e:
            reachable = False
            if default_drive:
                err(f"AUTO-DOWNLOAD WILL FAIL: {fn} has <defaultDrive>{default_drive}</defaultDrive> "
                    f"but URL returns {e.code}")
                print(f"         URL: {download_url}")
                print(f"         The app will try to download this on first launch and show an error.")
                print(f"         Fix: either publish a release, or remove <defaultDrive> from this disk.")
            elif not explicit_url:
                ok(f"URL 404 (pre-release, no <defaultDrive> — user must click Download)")
            else:
                err(f"URL returns {e.code}: {download_url[:80]}")
        except Exception as e:
            err(f"URL check failed: {e}")

        # THE KEY CHECK: if this disk auto-downloads and URL is broken,
        # the user gets a 404 error on first launch
        if default_drive and not reachable and not os.path.exists(app_copy):
            err(f"FIRST LAUNCH BROKEN: {fn} will auto-download (defaultDrive={default_drive}) "
                f"but is not available")

print(f"")
sys.exit(1 if errors > 0 else 0)
PYEOF
ERRORS=$((ERRORS + $?))

echo ""
if [ "$ERRORS" -gt 0 ]; then
  echo "FAILED — the app will show errors to users. Fix the issues above."
  exit 1
else
  echo "ALL CHECKS PASSED — the app will work correctly."
fi
