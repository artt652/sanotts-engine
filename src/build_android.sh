#!/usr/bin/env bash
# Сборка sanotts_cli под Android (Termux) — настоящая Android-программа:
# PIE, динамически связанная с системными bionic libc.so/libm.so/libdl.so,
# интерпретатор /system/bin/linker64 (linker для arm32), API 24 (Android 7),
# сегменты по 16 КБ. Так же собраны все пакеты Termux.
#
# Почему не статическая musl-сборка, как для обычного Linux: на Android 10+
# Termux запускает программы из своего каталога через системный загрузчик
# (termux-exec → /system/bin/linker64 ./программа), а тот принимает только
# ET_DYN; статический ET_EXEC он отвергает: "has unexpected e_type: 2".
#
#   ./build_android.sh SANOTTS_REPO SYSROOT TRIPLE OUT
#     TRIPLE: aarch64-linux-android | armv7a-linux-androideabi | x86_64-linux-android
#
# SYSROOT — bionic sysroot в раскладке NDK (usr/include, usr/include/<triple>/asm,
# usr/lib/<triple>/24/{crtbegin_dynamic.o,crtend_android.o}). Собирается из
# исходников bionic скриптом make_android_sysroot.sh (NDK не нужен).
# Нужны clang, ld.lld и llvm-nm; для arm32 в sysroot ещё libclang_rt.builtins-arm-android.a.
set -euo pipefail

REPO="${1:?usage: build_android.sh SANOTTS_REPO SYSROOT TRIPLE OUT}"
SYSROOT="${2:?}"
TRIPLE="${3:?}"
OUT="${4:?}"
API=24
HERE="$(cd "$(dirname "$0")" && pwd)"
M="$REPO/mcu"
ESP="$M/ports/wasm/espeak"

case "$TRIPLE" in
  aarch64-linux-android)    LIBDIR=aarch64-linux-android;  INTERP=/system/bin/linker64 ;;
  armv7a-linux-androideabi) LIBDIR=arm-linux-androideabi;  INTERP=/system/bin/linker ;;
  x86_64-linux-android)     LIBDIR=x86_64-linux-android;   INTERP=/system/bin/linker64 ;;
  *) echo "unknown triple $TRIPLE" >&2; exit 1 ;;
esac
CRT="$SYSROOT/usr/lib/$LIBDIR/$API"
CC=(clang "--target=$TRIPLE$API" "--sysroot=$SYSROOT" -fPIE -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables)

LIB=(common.c mnemonics.c error.c ieee80.c compiledata.c compiledict.c
     dictionary.c encoding.c intonation.c langopts.c numbers.c phoneme.c
     phonemelist.c readclause.c setlengths.c soundicon.c spect.c ssml.c
     synthdata.c synthesize.c tr_languages.c translate.c translateword.c
     voices.c wavegen.c speech.c espeak_api.c)
UCD=(case.c categories.c ctype.c proplist.c scripts.c tostring.c)

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# espeak-ng — из копии с исправлениями (espeak-fixes.patch): при отсутствии
# словаря языка (например, en_dict) espeak читал неинициализированную память.
cp -r "$ESP" "$tmp/espeak"
patch -s -p1 -d "$tmp/espeak" < "$HERE/espeak-fixes.patch"
ESP="$tmp/espeak"
# Декодер — из копии с piperlite-threads.patch (см. build_static.sh).
patch -s -o "$tmp/snt_piperlite.c" "$M/src/snt_piperlite.c" "$HERE/piperlite-threads.patch"
# Временный путь не должен попадать в бинарник (__FILE__ в assert) — сборка воспроизводима.
CC+=(-ffile-prefix-map="$tmp"=/build)
objs=()
for f in "${LIB[@]}"; do
  "${CC[@]}" -c -Os -ffunction-sections -fdata-sections -std=gnu11 -w -DHAVE_CONFIG_H -I"$ESP" -I"$ESP/include" -I"$ESP/ucd-tools/include" \
    -I"$ESP/libespeak-ng" "$ESP/libespeak-ng/$f" -o "$tmp/esp_${f%.c}.o"
  objs+=("$tmp/esp_${f%.c}.o")
done
for f in "${UCD[@]}"; do
  "${CC[@]}" -c -Os -ffunction-sections -fdata-sections -std=gnu11 -w -I"$ESP/ucd-tools/include" "$ESP/ucd-tools/$f" -o "$tmp/ucd_${f%.c}.o"
  objs+=("$tmp/ucd_${f%.c}.o")
done
for f in "$HERE/sanotts_cli.c" "$HERE/snt_g2p.c" "$HERE/snt_par.c" "$M/ports/wasm/snt_voice_wasm.c" \
         "$M/src/snt_front_f32.c" "$tmp/snt_piperlite.c"; do
  b="$(basename "${f%.c}")"
  "${CC[@]}" -c -O3 -ffunction-sections -fdata-sections -std=gnu99 -w -I"$HERE" -I"$ESP/include" -I"$M/include" -I"$M/src" -I"$M/ports/wasm" "$f" -o "$tmp/$b.o"
  objs+=("$tmp/$b.o")
done

# Утилита огласовок для арабского (tashkeel/) — отдельная программа рядом с движком.
"${CC[@]}" -c -O3 -ffunction-sections -fdata-sections -std=gnu99 -w -I"$HERE/tashkeel" "$HERE/tashkeel/sanotts_tashkeel.c" -o "$tmp/tashkeel.o"

# Заглушки libc.so/libm.so/libdl.so для линковки: те же SONAME, что у системных
# библиотек Android, и ровно те символы, которые программе нужны. На устройстве
# загрузчик свяжет их с настоящими /system/lib*/libc.so и т. д.
EXTRA=()
if [ "$TRIPLE" = armv7a-linux-androideabi ]; then
  # Помощники компилятора (__aeabi_idiv, __aeabi_uldivmod, __aeabi_ul2d …) из
  # compiler-rt, статически — как libclang_rt.builtins-arm-android.a в NDK.
  EXTRA+=("$SYSROOT/usr/lib/$LIBDIR/libclang_rt.builtins-arm-android.a")
fi
make_stubs() {  # DIR OBJ... — заглушки ровно под символы этих объектов
  local dir="$1"; shift
  python3 "$HERE/android_stubs.py" "$SYSROOT/symbols" "$TRIPLE" "$dir" "$@" "${EXTRA[@]}"
  for l in c m dl; do
    "${CC[@]}" -w -fno-builtin -shared -nostdlib -fuse-ld=lld -Wl,-soname,lib$l.so "$dir/lib$l.c" -o "$dir/lib$l.so"
  done
}
make_stubs "$tmp/stubs" "${objs[@]}"
make_stubs "$tmp/stubs_tk" "$tmp/tashkeel.o"

link_prog() {  # STUBS OUT OBJ...
  local stubs="$1" dst="$2"; shift 2
"${CC[@]}" -O3 -fuse-ld=lld -nostdlib -pie -s \
  -Wl,--dynamic-linker="$INTERP" \
  -Wl,-z,max-page-size=16384 -Wl,-z,now -Wl,-z,relro -Wl,--hash-style=both \
  -Wl,--build-id=sha1 -Wl,--gc-sections \
  "$CRT/crtbegin_dynamic.o" "$@" "${EXTRA[@]}" \
  -L"$stubs" -lm -lc -ldl \
  "$CRT/crtend_android.o" \
  -o "$dst"
# Как termux-elf-cleaner: загрузчик Android 7–8 не знает флага DF_1_PIE и на
# каждый запуск печатает "unsupported flags DT_FLAGS_1". Для него ELF и так PIE
# (ET_DYN), флаг ничего не добавляет — снимаем.
python3 - "$dst" <<'PY'
import struct, sys
p = sys.argv[1]; b = bytearray(open(p, "rb").read())
is64 = b[4] == 2; e = "<"
phoff, phentsize, phnum = (struct.unpack_from(e + "Q", b, 0x20)[0], *struct.unpack_from(e + "HH", b, 0x36)) if is64 \
    else (struct.unpack_from(e + "I", b, 0x1C)[0], *struct.unpack_from(e + "HH", b, 0x2A))
for i in range(phnum):
    o = phoff + i * phentsize
    if struct.unpack_from(e + "I", b, o)[0] != 2:        # PT_DYNAMIC
        continue
    off, sz = (struct.unpack_from(e + "Q", b, o + 8)[0], struct.unpack_from(e + "Q", b, o + 32)[0]) if is64 \
        else (struct.unpack_from(e + "I", b, o + 4)[0], struct.unpack_from(e + "I", b, o + 16)[0])
    ent = 16 if is64 else 8; fmt = e + ("qQ" if is64 else "iI")
    for j in range(off, off + sz, ent):
        tag, val = struct.unpack_from(fmt, b, j)
        if tag == 0x6ffffffb:                              # DT_FLAGS_1
            struct.pack_into(fmt, b, j, tag, val & ~0x08000000)
open(p, "wb").write(b)
PY
}
link_prog "$tmp/stubs" "$OUT" "${objs[@]}"
TK="$(dirname "$OUT")/sanotts_tashkeel"
link_prog "$tmp/stubs_tk" "$TK" "$tmp/tashkeel.o"
echo "built $TK ($(wc -c <"$TK") bytes)"
echo "built $OUT ($(wc -c <"$OUT") bytes)"
