#!/bin/bash
# Publish disk images and catalog as a GitHub release.
# Creates or updates the "disks" release so the app can download them.
set -e

IMGDIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$IMGDIR"

TAG="disks"
TITLE="Disk Images"

# Collect assets
ASSETS=()
for f in release_assets/disks.xml release_assets/help_index.json release_assets/help_*.md; do
  [ -f "$f" ] && ASSETS+=("$f")
done
for f in fd/freedos_hd.img fd/freedos_starter.img; do
  [ -f "$f" ] && ASSETS+=("$f")
done

echo "Assets to publish:"
for a in "${ASSETS[@]}"; do
  echo "  $a ($(du -h "$a" | cut -f1))"
done

# Pre-publish checks: files and hashes only (URLs won't work until release exists)
echo ""
echo "Checking file consistency..."
FAIL=0
python3 << 'PYEOF'
import xml.etree.ElementTree as ET, os, hashlib, sys
tree = ET.parse("release_assets/disks.xml")
errs = 0
for disk in tree.getroot().findall("disk"):
    fn = disk.findtext("filename","")
    path = f"fd/{fn}"
    if not os.path.exists(path): continue
    sz = os.path.getsize(path)
    xml_sz = disk.findtext("size","")
    xml_sha = disk.findtext("sha256","")
    if xml_sz and int(xml_sz) != sz:
        print(f"  ERROR: {fn} size mismatch xml={xml_sz} actual={sz}"); errs+=1
    sha = hashlib.sha256(open(path,"rb").read()).hexdigest()
    if xml_sha and xml_sha != sha:
        print(f"  ERROR: {fn} sha256 mismatch"); errs+=1
    elif not xml_sha:
        print(f"  ERROR: {fn} missing sha256 in catalog"); errs+=1
    else:
        print(f"  OK: {fn}")
if not os.path.exists("qxDOS/Resources/disks.xml"):
    print("  ERROR: bundled disks.xml missing"); errs+=1
elif open("release_assets/disks.xml").read() != open("qxDOS/Resources/disks.xml").read():
    print("  ERROR: bundled disks.xml differs from release_assets"); errs+=1
else:
    print("  OK: bundled disks.xml matches")
sys.exit(errs)
PYEOF
[ $? -ne 0 ] && { echo "Fix errors before publishing."; exit 1; }

# Delete existing release if present, then create fresh
if gh release view "$TAG" > /dev/null 2>&1; then
  echo ""
  echo "Deleting existing '$TAG' release..."
  gh release delete "$TAG" --yes --cleanup-tag
fi

echo ""
echo "Creating release '$TAG'..."
gh release create "$TAG" \
  --title "$TITLE" \
  --notes "Disk images and catalog for qxDOS. Auto-downloaded by the app." \
  --latest \
  "${ASSETS[@]}"

echo ""
echo "Published. Verifying URLs..."
sleep 3
FAIL=0
for a in "${ASSETS[@]}"; do
  fn=$(basename "$a")
  url="https://github.com/avwohl/qxDOS2/releases/latest/download/$fn"
  status=$(curl -sI -o /dev/null -w "%{http_code}" -L "$url" 2>/dev/null)
  if [ "$status" = "200" ]; then
    echo "  OK: $fn ($status)"
  else
    echo "  FAIL: $fn ($status) $url"
    FAIL=1
  fi
done

if [ "$FAIL" -eq 0 ]; then
  echo ""
  echo "All assets published and reachable."
else
  echo ""
  echo "Some assets not reachable yet — may need a moment to propagate."
fi
