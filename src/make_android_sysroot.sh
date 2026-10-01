#!/usr/bin/env bash
# Android (bionic) sysroot для build_android.sh — без Android NDK, из исходников:
#   * заголовки libc и kernel uapi — из bionic (github.com/aosp-mirror/platform_bionic);
#   * crtbegin_dynamic.o / crtend_android.o — собираются из bionic/libc/arch-common;
#   * карты символов libc/libm/libdl (для заглушек и проверки API 24);
#   * для arm32 — помощники компилятора из compiler-rt (llvm-project, та же
#     версия, что clang): libclang_rt.builtins-arm-android.a.
#
#   ./make_android_sysroot.sh BIONIC_DIR COMPILER_RT_BUILTINS_DIR OUT_SYSROOT
#
# Готовые бинарники в bin/android-*/ собраны clang/lld 18 на таком sysroot:
# bionic 731631f3 (2025-03-26), compiler-rt llvmorg-18.1.3.
set -euo pipefail
B="${1:?bionic dir}/libc"; RT="${2:?compiler-rt/lib/builtins}"; R="${3:?out}"
API=24
rm -rf "$R"; mkdir -p "$R/usr/include" "$R/symbols"
cp -r "$B/include/." "$R/usr/include/"
for d in linux asm-generic drm misc mtd rdma scsi sound video xen; do
  [ -d "$B/kernel/uapi/$d" ] && cp -r "$B/kernel/uapi/$d" "$R/usr/include/"
done
cp -r "$B/kernel/android/uapi/." "$R/usr/include/"
cp "$B/libc.map.txt" "$B/../libm/libm.map.txt" "$B/../libdl/libdl.map.txt" "$R/symbols/"

C="$B/arch-common/bionic"
for a in "aarch64-linux-android:arm64:arm64:aarch64-linux-android" \
         "arm-linux-androideabi:arm:arm:armv7a-linux-androideabi" \
         "x86_64-linux-android:x86_64:x86:x86_64-linux-android"; do
  IFS=: read -r libdir arch kasm triple <<<"$a"
  mkdir -p "$R/usr/include/$libdir"
  cp -r "$B/kernel/uapi/asm-$kasm/asm" "$R/usr/include/$libdir/asm"
  L="$R/usr/lib/$libdir/$API"; mkdir -p "$L"
  F=(--target="$triple$API" --sysroot="$R" -O2 -fPIC -I"$B/bionic" -I"$B/private" -I"$B/include"
     -I"$B" -I"$B/arch-$arch/include" -DPLATFORM_SDK_VERSION=$API)
  clang "${F[@]}" -D__ASSEMBLY__ -c "$C/crtbrand.S" -o "$L/crtbrand.o"
  clang "${F[@]}" -c "$C/crtbegin.c" -o "$L/crtbegin.o"
  clang "${F[@]}" -D__ASSEMBLY__ -c "$C/crtend.S" -o "$L/crtend_android.o"
  ld.lld -r "$L/crtbrand.o" "$L/crtbegin.o" -o "$L/crtbegin_dynamic.o"
  rm -f "$L/crtbrand.o" "$L/crtbegin.o"
done

O="$(mktemp -d)"; trap 'rm -rf "$O"' EXIT
F=(--target=armv7a-linux-androideabi$API --sysroot="$R" -O2 -fPIC -I"$RT" -fno-exceptions -fno-unwind-tables)
for s in arm/divsi3.S arm/udivsi3.S arm/modsi3.S arm/umodsi3.S arm/divmodsi4.S arm/udivmodsi4.S \
         arm/aeabi_idivmod.S arm/aeabi_uidivmod.S arm/aeabi_ldivmod.S arm/aeabi_uldivmod.S arm/aeabi_div0.c \
         udivmoddi4.c divmoddi4.c udivdi3.c divdi3.c umoddi3.c moddi3.c floatundidf.c floatdidf.c \
         floatundisf.c floatdisf.c fixdfdi.c fixunsdfdi.c fixsfdi.c fixunssfdi.c; do
  clang "${F[@]}" -c "$RT/$s" -o "$O/$(echo "$s" | tr / _).o"
done
llvm-ar rcs "$R/usr/lib/arm-linux-androideabi/libclang_rt.builtins-arm-android.a" "$O"/*.o
echo "sysroot ready: $R"
