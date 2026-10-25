#!/usr/bin/env bash
# Linux counterpart of scripts/package-release.ps1: stages and tars the
# fast-enough GUI, the afm-cli harness and (optionally) bundled adb next to
# the binaries, mirroring the Windows release layout (where findAdb() looks
# for platform-tools/adb next to the executable).
#
# Usage:
#   scripts/package-release.sh --version v1.2.3 [--out DIR] [--build-dir DIR]
#       [--skip-build] [--platform-tools DIR | --no-platform-tools]
set -euo pipefail

usage() {
    sed -n '2,10p' "$0"
    exit "${1:-0}"
}

version=""
out_dir=""
build_dir="build-release"
skip_build=0
platform_tools=""
no_platform_tools=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) version="${2:?--version needs a value}"; shift 2 ;;
        --out) out_dir="${2:?--out needs a value}"; shift 2 ;;
        --build-dir) build_dir="${2:?--build-dir needs a value}"; shift 2 ;;
        --skip-build) skip_build=1; shift ;;
        --platform-tools) platform_tools="${2:?--platform-tools needs a value}"; shift 2 ;;
        --no-platform-tools) no_platform_tools=1; shift ;;
        -h|--help) usage 0 ;;
        *) echo "Unknown option: $1" >&2; usage 1 ;;
    esac
done

[[ -n "$version" ]] || { echo "--version is required" >&2; usage 1; }
[[ "$version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Version must look like v1.2.3" >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
[[ -n "$out_dir" ]] || out_dir="$root"
arch="$(uname -m)"
name="fast-enough-$version-linux-$arch"
stage_parent="$root/release-staging"
stage="$stage_parent/$name"

if [[ $skip_build -eq 0 ]]; then
    cmake -S "$root" -B "$root/$build_dir" -DCMAKE_BUILD_TYPE=Release -DAFM_BUILD_GUI=ON
    cmake --build "$root/$build_dir" -j"$(nproc)"
fi
for binary in fast-enough afm-cli; do
    [[ -x "$root/$build_dir/$binary" ]] || {
        echo "Missing $root/$build_dir/$binary (build it or drop --skip-build)" >&2
        exit 1
    }
done

if [[ $no_platform_tools -eq 1 ]]; then
    platform_tools=""
elif [[ -z "$platform_tools" ]]; then
    for candidate in \
        "$HOME/Android/Sdk/platform-tools" \
        "${ANDROID_HOME:-}/platform-tools" \
        "${ANDROID_SDK_ROOT:-}/platform-tools" \
        "$root/thirdparty/android-platform-tools/platform-tools"; do
        if [[ -n "$candidate" && -x "$candidate/adb" ]]; then
            platform_tools="$candidate"
            break
        fi
    done
    [[ -n "$platform_tools" ]] || {
        echo "No platform-tools found (pass --platform-tools DIR or --no-platform-tools)" >&2
        exit 1
    }
fi
if [[ -n "$platform_tools" && ! -x "$platform_tools/adb" ]]; then
    echo "--platform-tools must contain adb: $platform_tools" >&2
    exit 1
fi

rm -rf "$stage"
mkdir -p "$stage/share/applications" "$stage/share/icons/hicolor/256x256/apps"

install -m 755 "$root/$build_dir/fast-enough" "$stage/fast-enough"
install -m 755 "$root/$build_dir/afm-cli" "$stage/afm-cli"
install -m 755 "$root/server/afm-server" "$stage/afm-server"
install -m 644 "$root/server/afm-labels.dex" "$stage/afm-labels.dex"
[[ -n "$platform_tools" ]] && cp -a "$platform_tools" "$stage/platform-tools"
cp "$root/LICENSE" "$stage/LICENSE"
cp "$root/readme.txt" "$stage/readme.txt"
cp "$root/LINUX.md" "$stage/LINUX.md"
if [[ -f "$root/res/app.png" ]]; then
    cp "$root/res/app.png" "$stage/share/icons/hicolor/256x256/apps/fast-enough.png"
fi

cat > "$stage/share/applications/fast-enough.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Fast Enough? - Android File Explorer
Comment=Browse and transfer files with an Android device over ADB
Exec=fast-enough %F
Icon=fast-enough
StartupWMClass=fast-enough
Terminal=false
Categories=Utility;FileManager;
MimeType=application/vnd.android.package-archive;
DESKTOP

cat > "$stage/install.sh" <<'INSTALLER'
#!/usr/bin/env bash
# Install fast-enough into the XDG user prefix (~/.local by default).
# Binaries live in $prefix/lib/fast-enough so the bundled platform-tools/
# directory sits next to the executable, which is where findAdb() looks.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
prefix="${1:-$HOME/.local}"
lib="$prefix/lib/fast-enough"
mkdir -p "$lib" "$prefix/bin" "$prefix/share/applications" \
         "$prefix/share/icons/hicolor/256x256/apps"
cp -a "$here/fast-enough" "$here/afm-cli" "$here/afm-server" "$here/afm-labels.dex" "$lib/"
if [[ -d "$here/platform-tools" ]]; then
    cp -a "$here/platform-tools" "$lib/"
fi
ln -sf "$lib/fast-enough" "$prefix/bin/fast-enough"
ln -sf "$lib/afm-cli" "$prefix/bin/afm-cli"
sed "s|^Exec=fast-enough|Exec=$lib/fast-enough|" \
    "$here/share/applications/fast-enough.desktop" \
    > "$prefix/share/applications/fast-enough.desktop"
if [[ -f "$here/share/icons/hicolor/256x256/apps/fast-enough.png" ]]; then
    cp "$here/share/icons/hicolor/256x256/apps/fast-enough.png" \
       "$prefix/share/icons/hicolor/256x256/apps/fast-enough.png"
fi
if command -v update-desktop-database >/dev/null; then
    update-desktop-database "$prefix/share/applications" 2>/dev/null || true
fi
echo "Installed to $prefix (binaries in $lib)."
echo "Run: fast-enough   (or afm-cli help)"
INSTALLER
chmod +x "$stage/install.sh"

mkdir -p "$out_dir"
tarball="$out_dir/$name.tar.gz"
rm -f "$tarball"
tar -C "$stage_parent" -czf "$tarball" "$name"
echo "Release tarball created: $tarball"
