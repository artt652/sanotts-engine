#!/usr/bin/env bash
# sanotts_play под Android: PIE на системных libc/libdl (bionic), API 24 — как build_android.sh.
#   ./build_android_play.sh SYSROOT TRIPLE OUT   (из src/play)
set -euo pipefail
SYSROOT="$1"; TRIPLE="$2"; OUT="$3"; API=24
HERE="$(cd "$(dirname "$0")" && pwd)"; STUBS_PY="$HERE/../android_stubs.py"
case "$TRIPLE" in
  aarch64-linux-android)    LIBDIR=aarch64-linux-android;  INTERP=/system/bin/linker64 ;;
  armv7a-linux-androideabi) LIBDIR=arm-linux-androideabi;  INTERP=/system/bin/linker ;;
  x86_64-linux-android)     LIBDIR=x86_64-linux-android;   INTERP=/system/bin/linker64 ;;
esac
CRT="$SYSROOT/usr/lib/$LIBDIR/$API"
CC=(clang "--target=$TRIPLE$API" "--sysroot=$SYSROOT" -fPIE -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables)
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
CC+=(-ffile-prefix-map="$HERE"=/build)
"${CC[@]}" -c -Os -ffunction-sections -fdata-sections -std=gnu11 -Wall -I"$HERE" "$HERE/sanotts_play.c" -o "$tmp/play.o"
EXTRA=()
[ "$TRIPLE" = armv7a-linux-androideabi ] && EXTRA+=("$SYSROOT/usr/lib/$LIBDIR/libclang_rt.builtins-arm-android.a")
python3 "$STUBS_PY" "$SYSROOT/symbols" "$TRIPLE" "$tmp" "$tmp/play.o" "${EXTRA[@]}"
for l in c m dl; do "${CC[@]}" -w -fno-builtin -shared -nostdlib -fuse-ld=lld -Wl,-soname,lib$l.so "$tmp/lib$l.c" -o "$tmp/lib$l.so"; done
"${CC[@]}" -Os -fuse-ld=lld -nostdlib -pie -s -Wl,--dynamic-linker="$INTERP" \
  -Wl,-z,max-page-size=16384 -Wl,-z,now -Wl,-z,relro -Wl,--hash-style=both -Wl,--build-id=sha1 -Wl,--gc-sections \
  "$CRT/crtbegin_dynamic.o" "$tmp/play.o" "${EXTRA[@]}" -L"$tmp" -lc -ldl "$CRT/crtend_android.o" -o "$OUT"
python3 - "$OUT" <<'PY'
import struct, sys
p = sys.argv[1]; b = bytearray(open(p, "rb").read())
is64 = b[4] == 2; e = "<"
phoff, phentsize, phnum = (struct.unpack_from(e + "Q", b, 0x20)[0], *struct.unpack_from(e + "HH", b, 0x36)) if is64 \
    else (struct.unpack_from(e + "I", b, 0x1C)[0], *struct.unpack_from(e + "HH", b, 0x2A))
for i in range(phnum):
    o = phoff + i * phentsize
    if struct.unpack_from(e + "I", b, o)[0] != 2: continue
    off, sz = (struct.unpack_from(e + "Q", b, o + 8)[0], struct.unpack_from(e + "Q", b, o + 32)[0]) if is64 \
        else (struct.unpack_from(e + "I", b, o + 4)[0], struct.unpack_from(e + "I", b, o + 16)[0])
    ent = 16 if is64 else 8; fmt = e + ("qQ" if is64 else "iI")
    for j in range(off, off + sz, ent):
        tag, val = struct.unpack_from(fmt, b, j)
        if tag == 0x6ffffffb: struct.pack_into(fmt, b, j, tag, val & ~0x08000000)
open(p, "wb").write(b)
PY
echo "built $OUT ($(wc -c <"$OUT") bytes)"
