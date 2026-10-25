#!/usr/bin/env bash
# Fetch Dear ImGui into thirdparty/imgui (mirrors setup.ps1).
set -euo pipefail

IMGUI_VERSION="v1.91.8"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMGUI_DIR="$SCRIPT_DIR/thirdparty/imgui"

if [ -f "$IMGUI_DIR/imgui.h" ]; then
    echo "ImGui already present in $IMGUI_DIR"
    exit 0
fi

echo "Downloading Dear ImGui $IMGUI_VERSION..."
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
ZIP="$TMPDIR/imgui.zip"

curl -fsSL -o "$ZIP" "https://github.com/ocornut/imgui/archive/refs/tags/${IMGUI_VERSION}.zip"
unzip -q "$ZIP" -d "$TMPDIR"
EXTRACTED="$(find "$TMPDIR" -maxdepth 1 -name 'imgui-*' -type d | head -n 1)"
rm -rf "$IMGUI_DIR"
mkdir -p "$SCRIPT_DIR/thirdparty"
cp -r "$EXTRACTED" "$IMGUI_DIR"
echo "ImGui ready in $IMGUI_DIR"
