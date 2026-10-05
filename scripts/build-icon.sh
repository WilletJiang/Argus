#!/bin/zsh
set -euo pipefail

TASK_ICON_ROOT="${0:A:h:h}"
TASK_ICON_RESOURCES="$TASK_ICON_ROOT/Resources"
TASK_ICON_RENDERER=$(command -v resvg || true)
if [[ -z "$TASK_ICON_RENDERER" ]]; then
  print -u2 'resvg is required to regenerate the PNG from Resources/AppIcon.svg.'
  exit 1
fi

TASK_ICON_TEMP=$(mktemp -d /tmp/argus-icon.XXXXXX)
trap 'rm -rf "$TASK_ICON_TEMP"' EXIT
TASK_ICON_SET="$TASK_ICON_TEMP/AppIcon.iconset"
mkdir -p "$TASK_ICON_SET"

"$TASK_ICON_RENDERER" --skip-system-fonts --width 1024 --height 1024 \
  "$TASK_ICON_RESOURCES/AppIcon.svg" "$TASK_ICON_RESOURCES/AppIcon.png"
for TASK_ICON_SIZE in 16 32 128 256 512; do
  TASK_ICON_DOUBLE=$((TASK_ICON_SIZE * 2))
  sips -z "$TASK_ICON_SIZE" "$TASK_ICON_SIZE" "$TASK_ICON_RESOURCES/AppIcon.png" \
    --out "$TASK_ICON_SET/icon_${TASK_ICON_SIZE}x${TASK_ICON_SIZE}.png" >/dev/null
  sips -z "$TASK_ICON_DOUBLE" "$TASK_ICON_DOUBLE" "$TASK_ICON_RESOURCES/AppIcon.png" \
    --out "$TASK_ICON_SET/icon_${TASK_ICON_SIZE}x${TASK_ICON_SIZE}@2x.png" >/dev/null
done
iconutil -c icns "$TASK_ICON_SET" -o "$TASK_ICON_RESOURCES/AppIcon.icns"
print "Built Resources/AppIcon.png and Resources/AppIcon.icns from hand-written SVG."
