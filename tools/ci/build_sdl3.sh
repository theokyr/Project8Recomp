#!/usr/bin/env bash
# Build and install SDL3 + SDL3_image at pinned versions, for CI.
#
# Users should install SDL3 from their package manager (docs/BUILDING.md). This
# exists because runners either lack an SDL3 package or disagree about the
# version, and SDL3 still moves fast enough for that to matter.

set -euo pipefail

SDL_VERSION="${SDL_VERSION:-3.2.16}"
SDL_IMAGE_VERSION="${SDL_IMAGE_VERSION:-3.2.4}"
WORK="${WORK:-$(mktemp -d)}"
PREFIX="${SDL_PREFIX:-${GITHUB_WORKSPACE:-$PWD}/.sdl3-prefix}"
JOBS="${JOBS:-4}"
mkdir -p "$PREFIX"

fetch() {
  curl -fsSL --retry 3 --retry-delay 2 -o "$2" "$1"
}

build() {
  local src="$1"; shift
  cmake -S "$src" -B "$src/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DBUILD_SHARED_LIBS=ON "$@"
  cmake --build "$src/build" --config Release --parallel "$JOBS"
  cmake --install "$src/build" --config Release
}

cd "$WORK"

fetch "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz" SDL3.tar.gz
tar xzf SDL3.tar.gz
build "$WORK/SDL3-${SDL_VERSION}"

# The launcher loads exactly one PNG (its window icon), so every other codec is
# off: less to install, and nothing fetched at configure time.
fetch "https://github.com/libsdl-org/SDL_image/releases/download/release-${SDL_IMAGE_VERSION}/SDL3_image-${SDL_IMAGE_VERSION}.tar.gz" SDL3_image.tar.gz
tar xzf SDL3_image.tar.gz
build "$WORK/SDL3_image-${SDL_IMAGE_VERSION}" \
  -DSDLIMAGE_VENDORED=OFF -DSDLIMAGE_DEPS_SHARED=OFF \
  -DSDLIMAGE_PNG=ON \
  -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_BMP=OFF -DSDLIMAGE_GIF=OFF \
  -DSDLIMAGE_JPG=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_LBM=OFF \
  -DSDLIMAGE_PCX=OFF -DSDLIMAGE_PNM=OFF -DSDLIMAGE_QOI=OFF \
  -DSDLIMAGE_SVG=OFF -DSDLIMAGE_TGA=OFF -DSDLIMAGE_TIF=OFF \
  -DSDLIMAGE_WEBP=OFF -DSDLIMAGE_XCF=OFF -DSDLIMAGE_XPM=OFF -DSDLIMAGE_XV=OFF

echo "SDL3 ${SDL_VERSION} + SDL3_image ${SDL_IMAGE_VERSION} -> $PREFIX"

if [ -n "${GITHUB_ENV:-}" ]; then
  {
    echo "CMAKE_PREFIX_PATH=$PREFIX"
    echo "LD_LIBRARY_PATH=$PREFIX/lib:${LD_LIBRARY_PATH:-}"
    echo "PATH=$PREFIX/bin:$PATH"
  } >> "$GITHUB_ENV"
fi
