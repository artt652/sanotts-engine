#!/usr/bin/env python3
"""Заглушки libc.so / libm.so / libdl.so для линковки Android-программы без NDK.

  android_stubs.py MAPS_DIR TRIPLE OUT_DIR obj1.o obj2.o ... [libgcc.a]

MAPS_DIR — каталог с libc.map.txt, libm.map.txt, libdl.map.txt из bionic.
Берёт неопределённые символы из объектов, для каждого находит библиотеку по
картам символов bionic — с учётом архитектуры и уровня API (24, Android 7) —
и пишет OUT_DIR/lib{c,m,dl}.c. Символ, которого нет ни в одной библиотеке на
API 24, — ошибка: такая программа не запустится на Android 7.
"""
import os
import re
import subprocess
import sys

API = 24
CODENAMES = {"L": 21, "M": 23, "N": 24, "O": 26, "P": 28, "Q": 29, "R": 30, "S": 31,
             "T": 33, "UpsideDownCake": 34, "VanillaIceCream": 35, "Baklava": 36}
ARCH = {"aarch64-linux-android": "arm64", "armv7a-linux-androideabi": "arm",
        "x86_64-linux-android": "x86_64"}
ALL_ARCHES = {"arm", "arm64", "x86", "x86_64", "riscv64"}
EXCLUDE_TAGS = {"platform-only", "apex", "systemapi", "llndk", "future"}


def level(v):
    v = v.strip()
    return int(v) if v.isdigit() else CODENAMES.get(v, 10 ** 6)


def parse_map(path, arch):
    """-> {symbol: is_variable} для символов, доступных на API и arch."""
    out = {}
    block_ok = True
    for raw in open(path, encoding="utf-8"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^(\w+)\s*\{(.*)$", line)
        if m:
            tags = m.group(2).split("#", 1)[1].split() if "#" in m.group(2) else []
            block_ok = check(tags, arch) and not m.group(1).endswith("PRIVATE") \
                and not m.group(1).startswith("LIBC_PLATFORM")
            continue
        if line.startswith("}") or line.endswith(":"):
            continue
        sym, _, tagstr = line.partition(";")
        sym = sym.strip()
        tags = tagstr.split("#", 1)[1].split() if "#" in tagstr else []
        if block_ok and check(tags, arch) and sym and "*" not in sym:
            out[sym] = "var" in tags
    return out


def check(tags, arch):
    arches = [t for t in tags if t in ALL_ARCHES]
    if arches and arch not in arches:
        return False
    for t in tags:
        if t in EXCLUDE_TAGS:
            return False
        if t.startswith("introduced=") and level(t.split("=", 1)[1]) > API:
            return False
        if t.startswith("introduced-" + arch + "=") and level(t.split("=", 1)[1]) > API:
            return False
    return True


def nm(path):
    r = subprocess.run(["llvm-nm", "--no-sort", path], capture_output=True, text=True, check=True)
    defined, undefined = set(), set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in ("U", "w"):
            undefined.add(parts[1])
        elif len(parts) == 3 and parts[1] not in ("U", "w"):
            defined.add(parts[2])
    return defined, undefined


def main():
    maps, triple, out_dir, objs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    arch = ARCH[triple]
    libs = {name: parse_map(os.path.join(maps, f"lib{name}.map.txt"), arch) for name in ("c", "m", "dl")}
    defined, undefined = set(), set()
    for o in objs:
        d, u = nm(o)
        defined |= d
        undefined |= u
    # crtbegin/crtend: их собственные ссылки на libc
    need = sorted((undefined | {"__libc_init", "__cxa_atexit", "__register_atfork"}) - defined
                  - {"main", "__fini_array_start", "__fini_array_end", "__init_array_start",
                     "__init_array_end", "__preinit_array_start", "__preinit_array_end", "_DYNAMIC"})
    os.makedirs(out_dir, exist_ok=True)
    per = {"c": [], "m": [], "dl": []}
    missing = []
    for s in need:
        for name in ("c", "m", "dl"):
            if s in libs[name]:
                per[name].append((s, libs[name][s]))
                break
        else:
            missing.append(s)
    if missing:
        sys.exit(f"android_stubs: not available on {arch} at API {API}: {' '.join(missing)}")
    for name, syms in per.items():
        with open(os.path.join(out_dir, f"lib{name}.c"), "w") as f:
            for s, is_var in syms:
                f.write(f"char {s}[8];\n" if is_var else f"void {s}(void) {{}}\n")
    print(f"android_stubs: {arch}: libc {len(per['c'])}, libm {len(per['m'])}, libdl {len(per['dl'])} symbols")


if __name__ == "__main__":
    main()
