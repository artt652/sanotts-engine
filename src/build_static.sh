#!/usr/bin/env bash
# Статическая сборка sanotts_cli со встроенным espeak-ng — для поставки
# готовых бинарников (bin/<arch>/ этого репозитория) для Linux и Windows.
#
#   CC=aarch64-linux-gnu-gcc ./build_static.sh /path/to/sanoTTS out/sanotts_cli
#
# Готовые бинарники в bin/ собраны на musl через zig (pip install ziglang),
# с обёрткой CC вида:  exec python -m ziglang cc -target aarch64-linux-musl "$@"
# (x86_64-linux-musl, aarch64-linux-musl, arm-linux-musleabihf, x86-linux-musl). musl, а не
# glibc, потому что статический glibc при старте делает системные вызовы
# (rseq и др.), которые seccomp-фильтр Android может убить — а движок должен
# работать и в Android-окружениях. Результат с glibc-сборкой совпадает (фонемы — 1:1,
# звук — до 1 младшего бита).
#
# Windows (bin/windows-*/sanotts_cli.exe) — тем же скриптом, с обёрткой
#   exec python -m ziglang cc -target x86_64-windows-gnu "$@"   (или x86-windows-gnu для 32-битной);
# недостающие в mingw POSIX-заголовки берутся из wincompat/, функции — из compat.h.
#
# Берёт из репозитория sanoTTS: C-рантайм (mcu/src, mcu/include,
# mcu/ports/wasm/snt_voice_wasm.c, cp_id_tables_multi.h) и исходники
# espeak-ng 1.52 (mcu/ports/wasm/espeak) — тот же набор, что их браузерная
# и ESP32-сборки. Результат не зависит ни от libespeak-ng, ни от libc хоста.
set -euo pipefail

REPO="${1:?usage: build_static.sh SANOTTS_REPO OUT}"
OUT="${2:?usage: build_static.sh SANOTTS_REPO OUT}"
CC="${CC:-gcc}"
HERE="$(cd "$(dirname "$0")" && pwd)"
M="$REPO/mcu"
ESP="$M/ports/wasm/espeak"

LIB=(common.c mnemonics.c error.c ieee80.c compiledata.c compiledict.c
     dictionary.c encoding.c intonation.c langopts.c numbers.c phoneme.c
     phonemelist.c readclause.c setlengths.c soundicon.c spect.c ssml.c
     synthdata.c synthesize.c tr_languages.c translate.c translateword.c
     voices.c wavegen.c speech.c espeak_api.c)
UCD=(case.c categories.c ctype.c proplist.c scripts.c tostring.c)

# Windows (mingw): недостающие POSIX-заголовки (endian.h) — из wincompat/.
EXTRA=(); LIBS=(-lm)
# (без grep -q: под pipefail ранний выход grep обрывал конвейер и проверка «не срабатывала»)
if "$CC" -dM -E -x c /dev/null 2>/dev/null | grep '_WIN32' >/dev/null; then EXTRA=(-I"$HERE/wincompat"); LIBS=(-lshell32); fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# espeak-ng — из копии с исправлениями (espeak-fixes.patch): при отсутствии
# словаря языка (например, en_dict) espeak читал неинициализированную память.
cp -r "$ESP" "$tmp/espeak"
patch -s -p1 -d "$tmp/espeak" < "$HERE/espeak-fixes.patch"
ESP="$tmp/espeak"
# Временный путь не должен попадать в бинарник (__FILE__ в assert) — сборка воспроизводима.
PFX=(-ffile-prefix-map="$tmp"=/build)
objs=()
for f in "${LIB[@]}"; do
  "$CC" "${PFX[@]}" -c -Os -ffunction-sections -fdata-sections -std=gnu11 -w -DHAVE_CONFIG_H -DLIBESPEAK_NG_EXPORT -include "$HERE/compat.h" "${EXTRA[@]}" -I"$ESP" -I"$ESP/include" -I"$ESP/ucd-tools/include" \
        -I"$ESP/libespeak-ng" "$ESP/libespeak-ng/$f" -o "$tmp/esp_${f%.c}.o"
  objs+=("$tmp/esp_${f%.c}.o")
done
for f in "${UCD[@]}"; do
  "$CC" "${PFX[@]}" -c -Os -ffunction-sections -fdata-sections -std=gnu11 -w -include "$HERE/compat.h" "${EXTRA[@]}" -I"$ESP/ucd-tools/include" "$ESP/ucd-tools/$f" -o "$tmp/ucd_${f%.c}.o"
  objs+=("$tmp/ucd_${f%.c}.o")
done

# Без -march=native: бинарник должен работать на любом CPU этой архитектуры.
"$CC" -O3 -std=gnu99 -static -s -ffunction-sections -fdata-sections -Wl,--gc-sections -DLIBESPEAK_NG_EXPORT \
  -I"$ESP/include" -I"$M/include" -I"$M/src" -I"$M/ports/wasm" \
  -o "$OUT" \
  "$HERE/sanotts_cli.c" "$HERE/snt_g2p.c" \
  "$M/ports/wasm/snt_voice_wasm.c" "$M/src/snt_front_f32.c" "$M/src/snt_piperlite.c" \
  "${objs[@]}" "${LIBS[@]}"
echo "built $OUT ($(wc -c <"$OUT") bytes)"

# Утилита огласовок для арабского (tashkeel/) — отдельная программа рядом с движком.
EXT=""; case "$OUT" in *.exe) EXT=".exe" ;; esac
TK="$(dirname "$OUT")/sanotts_tashkeel$EXT"
"$CC" -O3 -std=gnu99 -static -s -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$HERE/tashkeel" -o "$TK" "$HERE/tashkeel/sanotts_tashkeel.c" "${LIBS[@]}"
echo "built $TK ($(wc -c <"$TK") bytes)"
