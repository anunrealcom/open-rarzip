#!/usr/bin/env bash
# 离线构建 libarchive 全格式依赖（全静态），无需联网。
# 依赖源码已位于 deps/src/{bzip2,xz,zstd,libarchive}，zlib 用 Qt 自带。
set -e

MINGW=/c/Qt/Tools/mingw1310_64
QT_ZLIB_INC=$MINGW/x86_64-w64-mingw32/include
QT_ZLIB_LIB=$MINGW/x86_64-w64-mingw32/lib/libz.a
SRC=$PWD/deps/src
PREFIX=$PWD/deps/dist

export PATH="$MINGW/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
CC=gcc.exe
CXX=g++.exe

log(){ echo "[$(date +%H:%M:%S)] $*"; }

mkdir -p "$PREFIX"

build_one(){
  local name="$1"; local src="$2"; shift 2
  log "=== building $name ==="
  rm -rf "$PREFIX/build_$name"
  cmake -S "$src" -B "$PREFIX/build_$name" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    "$@"
  cmake --build "$PREFIX/build_$name" -j4
  cmake --install "$PREFIX/build_$name"
  log "=== $name done ==="
}

# 1) bzip2 (static)
build_one bzip2 "$SRC/bzip2" \
  -DENABLE_APP=OFF -DENABLE_TESTS=OFF -DBUILD_SHARED_LIBS=OFF

# 2) xz / liblzma (static)
build_one xz "$SRC/xz" \
  -DBUILD_TESTING=OFF -DENABLE_TESTS=OFF -DENABLE_TEST_CC=OFF \
  -DENABLE_DOXYGEN=OFF -DBUILD_SHARED_LIBS=OFF

# 3) zstd (static, lib only)
build_one zstd "$SRC/zstd/build/cmake" \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_STATIC=ON \
  -DZSTD_BUILD_SHARED=OFF

# 4) libarchive (static, full filters)
log "=== building libarchive ==="
rm -rf "$PREFIX/build_libarchive"
cmake -S "$SRC/libarchive" -B "$PREFIX/build_libarchive" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$PREFIX;$QT_ZLIB_INC/.." \
  -DZLIB_INCLUDE_DIR="$QT_ZLIB_INC" -DZLIB_LIBRARY="$QT_ZLIB_LIB" \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
  -DENABLE_ZLIB=ON -DENABLE_BZip2=ON -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DENABLE_LZ4=OFF \
  -DENABLE_NETTLE=OFF -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_LIBB64=OFF \
  -DENABLE_ICONV=OFF -DENABLE_XATTR=OFF -DENABLE_ACL=OFF
cmake --build "$PREFIX/build_libarchive" -j4
cmake --install "$PREFIX/build_libarchive"

log "=== ALL DEPS BUILT ==="
find "$PREFIX" -name "*.a" | head -40
