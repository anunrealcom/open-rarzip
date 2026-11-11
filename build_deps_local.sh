#!/usr/bin/env bash
# 离线构建全格式压缩依赖（静态库），产物安装到 deps/dist。
# 自动定位 Qt/MinGW 工具链；可用 QT_ROOT 环境变量覆盖。
# 依赖源码：deps/src/{bzip2,xz,zstd,libarchive}，zlib 复用 Qt 自带。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

log(){ echo "[$(date +%H:%M:%S)] $*"; }

QT_ROOT="${QT_ROOT:-}"
if [ -z "$QT_ROOT" ]; then
  for c in "D:/Qt" "C:/Qt" "E:/Qt" "/d/Qt" "/c/Qt"; do
    if [ -f "$c/Tools/mingw1310_64/bin/gcc.exe" ]; then QT_ROOT="$c"; break; fi
  done
fi
if [ -z "$QT_ROOT" ]; then
  echo "找不到 Qt MinGW 工具链，请设置 QT_ROOT，例如 QT_ROOT=D:/Qt" >&2
  exit 1
fi

# Git Bash 的 PATH 需要 POSIX 形式，CMake 参数仍使用 Windows 形式
case "$QT_ROOT" in
  [A-Za-z]:/*)
    _drive="${QT_ROOT%%:*}"; _rest="${QT_ROOT#*:}"
    QT_POSIX="/$(printf '%s' "$_drive" | tr 'A-Z' 'a-z')${_rest}" ;;
  *) QT_POSIX="$QT_ROOT" ;;
esac

MINGW="$QT_ROOT/Tools/mingw1310_64"
CMAKE_BIN="$QT_POSIX/Tools/CMake_64/bin"
NINJA_BIN="$QT_POSIX/Tools/Ninja"
SRC="$ROOT/deps/src"
PREFIX="$ROOT/deps/dist"
ZLIB_INC="$MINGW/x86_64-w64-mingw32/include"
ZLIB_LIB="$MINGW/x86_64-w64-mingw32/lib/libz.a"

export PATH="$QT_POSIX/Tools/mingw1310_64/bin:$CMAKE_BIN:$NINJA_BIN:$PATH"
CC="$MINGW/bin/gcc.exe"
CXX="$MINGW/bin/g++.exe"

# deps/src 不入库（约 120MB，见 .gitignore）。缺失时给出固定版本的复现命令，
# 避免 CMake 抛出难以定位的 "source directory does not exist"。
_missing_src=0
require_src(){
  local name="$1" url="$2" sha="$3"
  if [ -f "$SRC/$name/CMakeLists.txt" ]; then return 0; fi
  echo "[缺失] deps/src/$name" >&2
  printf '  git clone %s deps/src/%s && git -C deps/src/%s checkout %s\n' \
         "$url" "$name" "$name" "$sha" >&2
  _missing_src=1
}
require_src bzip2      "https://github.com/libarchive/bzip2.git"      "1ea1ac188ad4b9cb662e3f8314673c63df95a589"
require_src libarchive "https://github.com/libarchive/libarchive.git"  "28bd729bb4a8cfe85a60c904e555a226e6c88324"
require_src xz         "https://github.com/tukaani-project/xz.git"    "28a66a3d1b6af89ed6ee841384c339629fda8264"
require_src zstd       "https://github.com/facebook/zstd.git"         "7f929ae3f9139c0ce5127e75a9c519356fe1f1a2"
if [ "$_missing_src" -ne 0 ]; then
  echo "依赖源码缺失：按上面的命令取回固定版本后重新运行本脚本（也可直接拷贝 deps/src）。" >&2
  exit 1
fi

mkdir -p "$PREFIX"

build_one(){
  local name="$1"; local src="$2"; shift 2
  log "=== building $name ==="
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

build_one bzip2 "$SRC/bzip2" \
  -DENABLE_LIB_ONLY=ON -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=OFF \
  -DENABLE_APP=OFF -DENABLE_EXAMPLES=OFF -DENABLE_DOCS=OFF

# libarchive 的 FindBZip2 只按 "bz2" 名字查找，补一份常规命名的静态库
if [ -f "$PREFIX/lib/libbz2_static.a" ] && [ ! -f "$PREFIX/lib/libbz2.a" ]; then
  cp "$PREFIX/lib/libbz2_static.a" "$PREFIX/lib/libbz2.a"
fi

build_one xz "$SRC/xz" \
  -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF \
  -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_DOC=OFF -DXZ_NLS=OFF

# zstd 的 CMake 工程只启用 C 语言；传入 CXX 编译器会让
# CHECK_CXX_COMPILER_FLAG 报 "CXX: needs to be enabled before use"。
log "=== building zstd ==="
cmake --fresh -S "$SRC/zstd/build/cmake" -B "$PREFIX/build_zstd" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF \
  -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_SHARED=OFF
cmake --build "$PREFIX/build_zstd" -j4
cmake --install "$PREFIX/build_zstd"
log "=== zstd done ==="

log "=== building libarchive (CNG crypto enabled) ==="
cmake -S "$SRC/libarchive" -B "$PREFIX/build_libarchive" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DZLIB_INCLUDE_DIR="$ZLIB_INC" -DZLIB_LIBRARY="$ZLIB_LIB" \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
  -DENABLE_ZLIB=ON -DENABLE_BZip2=ON -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DENABLE_LZ4=OFF \
  -DENABLE_OPENSSL=OFF -DENABLE_NETTLE=OFF -DENABLE_MBEDTLS=OFF -DENABLE_CNG=ON \
  -DENABLE_LIBB64=OFF -DENABLE_ICONV=OFF -DENABLE_XATTR=OFF -DENABLE_ACL=OFF
cmake --build "$PREFIX/build_libarchive" -j4
cmake --install "$PREFIX/build_libarchive"

log "=== ALL DEPS BUILT ==="
find "$PREFIX" -name "*.a" | sort
