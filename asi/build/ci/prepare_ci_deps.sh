#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Bootstraps external build dependencies (Mesa 26.3.0 tarball/source, libdrm_amdgpu >= 2.4.133,
# and NotoSansCJKsc-Regular.ttf) for clean GitHub Actions / Linux builds without mutating git sources.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."

CI_CACHE_DIR="${OPENRR_CI_CACHE_DIR:-$HOME/.cache/openrr-ci}"
MESA_WORK="${OPENRR_MESA_263_WORK:-$HOME/work/openrr-mesa-263-comparison}"
MESA_SOURCE="${OPENRR_MESA_263_SOURCE:-$MESA_WORK/mesa-26.3.0}"
MESA_TARBALL="${OPENRR_MESA_263_TARBALL:-$CI_CACHE_DIR/mesa-26.3.0.tar.xz}"
MESA_SHA256="91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a"

MESA_DEPS_PREFIX="${OPENRR_MESA_DEPS_PREFIX:-$HOME/src/mesa-deps}"
LIBDRM_VER="2.4.134"
LIBDRM_COMMIT="e984d448b8b17aab853369e6c203e53719f46de1"

jobs="${OPENRR_BUILD_JOBS:-$(nproc 2>/dev/null || echo 8)}"
if ! [[ "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "invalid OPENRR_BUILD_JOBS: $jobs" >&2
    exit 1
fi

mkdir -p "$CI_CACHE_DIR" "$MESA_WORK"

verify_sha256() {
    local file="$1" expected="$2"
    [[ -f "$file" ]] || return 1
    local got
    got=$(sha256sum "$file" | awk '{print $1}')
    [[ "$got" == "$expected" ]]
}

# 1. Mesa 26.3.0 tarball & source
if ! verify_sha256 "$MESA_TARBALL" "$MESA_SHA256"; then
    echo "[ci-deps] Downloading Mesa 26.3.0 tarball..."
    rm -f "$MESA_TARBALL.tmp"
    if ! curl -fsSL --retry 3 "https://archive.mesa3d.org/mesa-26.3.0.tar.xz" -o "$MESA_TARBALL.tmp"; then
        curl -fsSL --retry 3 "https://gitlab.freedesktop.org/mesa/mesa/-/releases/mesa-26.3.0/downloads/mesa-26.3.0.tar.xz" -o "$MESA_TARBALL.tmp"
    fi
    verify_sha256 "$MESA_TARBALL.tmp" "$MESA_SHA256" || {
        echo "FATAL: Mesa 26.3.0 tarball SHA-256 mismatch" >&2
        exit 1
    }
    mv "$MESA_TARBALL.tmp" "$MESA_TARBALL"
fi

if [[ ! -f "$MESA_SOURCE/VERSION" ]]; then
    echo "[ci-deps] Extracting Mesa 26.3.0 source to $MESA_SOURCE..."
    rm -rf "$MESA_SOURCE"
    mkdir -p "$MESA_SOURCE"
    tar -xJf "$MESA_TARBALL" -C "$MESA_SOURCE" --strip-components=1
fi

# 2. libdrm_amdgpu >= 2.4.133 under $HOME/src/mesa-deps
if [[ ! -f "$MESA_DEPS_PREFIX/lib/x86_64-linux-gnu/pkgconfig/libdrm_amdgpu.pc" ]]; then
    echo "[ci-deps] Building libdrm $LIBDRM_VER into $MESA_DEPS_PREFIX..."
    DRM_WORK="$CI_CACHE_DIR/drm-src"
    DRM_BUILD="$CI_CACHE_DIR/drm-build"
    rm -rf "$DRM_WORK" "$DRM_BUILD"
    mkdir -p "$DRM_WORK"
    DRM_TARBALL="$CI_CACHE_DIR/libdrm-$LIBDRM_VER.tar.xz"
    if [[ -f "$DRM_TARBALL" ]] || curl -fsSL --retry 3 "https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM_VER.tar.xz" -o "$DRM_TARBALL.tmp" 2>/dev/null; then
        if [[ -f "$DRM_TARBALL.tmp" ]]; then
            mv "$DRM_TARBALL.tmp" "$DRM_TARBALL"
        fi
        tar -xJf "$DRM_TARBALL" -C "$DRM_WORK" --strip-components=1
    else
        rm -f "$DRM_TARBALL.tmp"
        git init -q "$DRM_WORK"
        git -C "$DRM_WORK" fetch --depth 1 https://gitlab.freedesktop.org/mesa/drm.git "$LIBDRM_COMMIT"
        git -C "$DRM_WORK" checkout -q FETCH_HEAD
    fi
    meson setup "$DRM_BUILD" "$DRM_WORK" \
        --prefix="$MESA_DEPS_PREFIX" \
        --libdir=lib/x86_64-linux-gnu \
        --buildtype=release \
        -Damdgpu=enabled -Dintel=disabled -Dradeon=disabled -Dnouveau=disabled \
        -Dudev=false -Dtests=false -Dcairo-tests=disabled -Dvalgrind=disabled
    ninja -C "$DRM_BUILD" -j "$jobs" install
    rm -rf "$DRM_WORK" "$DRM_BUILD"
fi

# 3. CJK font (NotoSansCJKsc-Regular.ttf, cached in $CI_CACHE_DIR across CI runs)
FONT_OUT="asi/lang/fonts/NotoSansCJKsc-Regular.ttf"
FONT_CACHE="$CI_CACHE_DIR/NotoSansCJKsc-Regular.ttf"
if [[ ! -f "$FONT_OUT" ]]; then
    if [[ ! -f "$FONT_CACHE" ]] || [[ "$(wc -c < "$FONT_CACHE")" -le 5242880 ]]; then
        if [[ -n "${OPENRR_CJK_FONT_URL:-}" ]]; then
            echo "[ci-deps] Downloading CJK font from OPENRR_CJK_FONT_URL..."
            curl -fsSL --retry 3 "$OPENRR_CJK_FONT_URL" -o "$FONT_CACHE.tmp.$$"
            if [[ "$(wc -c < "$FONT_CACHE.tmp.$$")" -le 5242880 ]]; then
                rm -f "$FONT_CACHE.tmp.$$"
                echo "FATAL: Downloaded font unexpectedly small" >&2
                exit 1
            fi
            mv -f "$FONT_CACHE.tmp.$$" "$FONT_CACHE"
        else
            echo "[ci-deps] Extracting Noto Sans CJK SC from system NotoSansCJK TTC..."
            python3 - "$FONT_CACHE" <<'PY'
import pathlib, sys
from fontTools.ttLib import TTCollection

out_path = pathlib.Path(sys.argv[1])
candidates = [
    pathlib.Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
    pathlib.Path("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"),
]
ttc_path = next((p for p in candidates if p.is_file()), None)
if ttc_path is None:
    ttc_path = next(pathlib.Path("/usr/share/fonts").rglob("NotoSansCJK*Regular*.ttc"), None)
if ttc_path is None:
    raise SystemExit("FATAL: NotoSansCJK-Regular.ttc not found (install fonts-noto-cjk or set OPENRR_CJK_FONT_URL)")

ttc = TTCollection(str(ttc_path))
chosen = None
for font in ttc.fonts:
    names = {r.toUnicode() for r in font["name"].names}
    if any("Noto Sans CJK SC" in n or "NotoSansCJKsc" in n for n in names):
        chosen = font
        break
if chosen is None:
    raise SystemExit(f"FATAL: Noto Sans CJK SC face not found inside {ttc_path}")

out_path.parent.mkdir(parents=True, exist_ok=True)
tmp = out_path.with_suffix(".ttf.tmp")
chosen.save(str(tmp))
if tmp.stat().st_size <= 5 * 1024 * 1024:
    raise SystemExit(f"FATAL: Extracted font unexpectedly small: {tmp.stat().st_size} bytes")
tmp.replace(out_path)
print(f"Extracted {out_path} ({out_path.stat().st_size:,} bytes) from {ttc_path}")
PY
        fi
    else
        echo "[ci-deps] Using cached CJK font $FONT_CACHE..."
    fi
    mkdir -p "$(dirname -- "$FONT_OUT")"
    cp -- "$FONT_CACHE" "$FONT_OUT"
fi

echo "[ci-deps] Ready: MESA_TARBALL=$MESA_TARBALL MESA_DEPS=$MESA_DEPS_PREFIX FONT=$FONT_OUT"
