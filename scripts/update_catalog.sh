#!/bin/bash
# Update catalog XMLs if a rebuilt disk image differs from the existing one.
#
# Usage: update_catalog.sh <new_image> <dest_image>
#   new_image  - freshly built image (e.g. /tmp/freedos_starter.img)
#   dest_image - target in fd/ (e.g. fd/freedos_starter.img)
#
# If the SHA matches the existing image, nothing is touched.
# If different: replaces the image, updates size+sha in both disks.xml,
# bumps <catalog version>, and syncs the two copies.

set -e

NEW="$1"
DEST="$2"

if [ -z "$NEW" ] || [ -z "$DEST" ]; then
  echo "Usage: $0 <new_image> <dest_image>"
  exit 1
fi

IMGDIR="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_XML="$IMGDIR/release_assets/disks.xml"
BUNDLED_XML="$IMGDIR/qxDOS/Resources/disks.xml"
DISK_NAME=$(basename "$DEST")

NEW_SHA=$(shasum -a 256 "$NEW" | awk '{print $1}')
NEW_SIZE=$(stat -f%z "$NEW")

echo ""
echo "--- Catalog update: $DISK_NAME ---"

# Compare with existing image
if [ -f "$DEST" ]; then
  OLD_SHA=$(shasum -a 256 "$DEST" | awk '{print $1}')
  if [ "$NEW_SHA" = "$OLD_SHA" ]; then
    echo "  No changes (SHA matches). Skipping."
    rm -f "$NEW"
    return 0 2>/dev/null || exit 0
  fi
  echo "  Disk changed: updating catalog."
  echo "    old: $OLD_SHA"
  echo "    new: $NEW_SHA"
else
  echo "  New disk image (no previous version)."
fi

# Replace image
cp "$NEW" "$DEST"
rm -f "$NEW"

# Update size + sha in both XMLs
update_xml() {
  local xmlfile="$1"
  [ -f "$xmlfile" ] || return
  python3 -c "
import xml.etree.ElementTree as ET, sys
tree = ET.parse('$xmlfile')
changed = False
for d in tree.getroot().findall('disk'):
    if d.findtext('filename') == '$DISK_NAME':
        sz = d.find('size')
        if sz is not None and sz.text != '$NEW_SIZE':
            sz.text = '$NEW_SIZE'; changed = True
        sha = d.find('sha256')
        if sha is not None and sha.text != '$NEW_SHA':
            sha.text = '$NEW_SHA'; changed = True
if changed:
    # Bump catalog version
    root = tree.getroot()
    ver = int(root.get('version', '0'))
    root.set('version', str(ver + 1))
    # Write with same formatting as original (re-read and patch to preserve XML style)
    print(f'  Updated {\"$xmlfile\"}: v{ver} -> v{ver+1}')
else:
    print(f'  {\"$xmlfile\"}: already up to date')
    sys.exit(0)

# Preserve original XML formatting by doing text replacement
import re
text = open('$xmlfile').read()
# Update sha256 for this disk
def replace_in_disk_block(text, filename, tag, new_value):
    pattern = r'(<filename>' + re.escape(filename) + r'</filename>.*?<' + tag + r'>)(.*?)(</' + tag + r'>)'
    return re.sub(pattern, r'\g<1>' + new_value + r'\3', text, flags=re.DOTALL)
text = replace_in_disk_block(text, '$DISK_NAME', 'size', '$NEW_SIZE')
text = replace_in_disk_block(text, '$DISK_NAME', 'sha256', '$NEW_SHA')
# Bump version
text = re.sub(r'catalog version=\"\d+\"', f'catalog version=\"{ver+1}\"', text)
open('$xmlfile', 'w').write(text)
"
}

update_xml "$RELEASE_XML"

# Sync bundled copy from release copy (single source of truth)
if [ -f "$RELEASE_XML" ]; then
  cp "$RELEASE_XML" "$BUNDLED_XML"
  echo "  Synced bundled disks.xml from release copy."
fi

echo "  Done. Run scripts/publish_disks.sh to push to GitHub."
