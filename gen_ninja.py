#!/usr/bin/env python3
"""leopard-shell 构建 + xbps 打包（build / pack 两个 target）。"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent          # leopard-shell/
OUT  = ROOT / "out"

SOURCES = ["main.c"]
TARGET  = OUT / "lpsh.elf"

# XACT CRT（../XJ380-SDK/out/crt）：启动对象 + 静态运行时 + 分配器
CRT_ROOT = ROOT / ".." / "XJ380-SDK"
INC      = CRT_ROOT / "out" / "crt" / "include"
LIB      = CRT_ROOT / "out" / "crt" / "lib" / "x86_64"
CRT_OBJS = [LIB / "crt0.o", LIB / "start.o"]
CRT_LIBS = [LIB / "libxact.a", LIB / "liballoc.a"]

# ---- xbps 包配置 ----
# install/data 均为相对 / 的安装路径（无前导斜杠）
PKG = {
    "pkgname": "leopard-shell",
    "pkgver":  "1.0_1",
    "arch":    "x86_64",
    "desc":    "Leopard shell (lpsh)",
    "deps":    "",
    "install": "apps/system/lpsh.elf",
    "data":    [],
}


def rel(p):
    p = Path(p)
    if p.is_absolute():
        try:
            return str(p.relative_to(ROOT))
        except ValueError:
            return str(p)
    return str(p)


def esc(p):
    return rel(p).replace("$", "$$").replace(":", "$:").replace(" ", "$ ")


def emit_package(L, pkg, target):
    inst = pkg["install"]
    data = pkg.get("data", [])
    out  = OUT / f"{pkg['pkgname']}-{pkg['pkgver']}.{pkg['arch']}.xbps"

    args = f"-A {pkg['arch']} -n {pkg['pkgname']}-{pkg['pkgver']} -s \"{pkg['desc']}\""
    if pkg.get("deps"):
        args += f" -D \"{pkg['deps']}\""

    files = [(target, inst)] + [(ROOT / s, d) for s, d in data]

    # 一个 package 规则：先 stage 全部文件到 .pkg/，再 cd out 运行 xbps-create
    # （xbps-create 无 -o，输出到 CWD，自动命名 <pkgver>.<arch>.xbps）
    cmd = "rm -rf .pkg"
    for src, dst in files:
        cmd += f" && mkdir -p .pkg/$$(dirname {dst}) && cp {esc(src)} .pkg/{dst}"
    cmd += f" && cd out && xbps-create --compression gzip {args} ../.pkg"

    L += ["rule package", f"  command = {cmd}", ""]
    L += [f"build {esc(out)}: package {' '.join(esc(s) for s, _ in files)}"]
    L += [f"build build: phony {esc(target)}"]
    L += [f"build pack: phony {esc(out)}"]
    L += ["default build"]


def main():
    cc   = os.environ.get("CC",  "clang")
    cxx  = os.environ.get("CXX", "clang++")
    ld   = os.environ.get("LD",  "ld")
    opt  = "-O" + os.environ.get("OPT_LEVEL", "2")
    col  = os.environ.get("DIAGNOSTICS_COLOR", "-fdiagnostics-color=always")

    common = (f"{col} {opt} -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign "
              "-fno-stack-protector -fno-exceptions -fno-strict-aliasing -fshort-wchar -nostdinc")
    c_flags   = f"{common} -std=c11   -I {rel(INC)} -I . -MMD -MP"
    cpp_flags = f"{common} -std=c++11 -I {rel(INC)} -I . -Wno-write-strings -MMD -MP"

    L = ["# leopard-shell 构建 + xbps 打包", "ninja_required_version = 1.10",
         f"cc = {cc}", f"cxx = {cxx}", f"ld = {ld}",
         f"c_flags = {c_flags}", f"cpp_flags = {cpp_flags}", ""]
    L += [
        "rule cc",
        "  command = mkdir -p $$(dirname $out) && $cc $c_flags -MF $out.d -c $in -o $out",
        "  depfile = $out.d", "  deps = gcc", "",
        "rule cxx",
        "  command = mkdir -p $$(dirname $out) && $cxx $cpp_flags -MF $out.d -c $in -o $out",
        "  depfile = $out.d", "  deps = gcc", "",
        "rule link",
        "  command = $ld -Ttext=0x200000 $in -o $out", "",
    ]

    app_objs = []
    for src in SOURCES:
        rule = "cc" if src.endswith(".c") else "cxx"
        obj = OUT / (src + ".o")
        L += [f"build {esc(obj)}: {rule} {esc(ROOT / src)}"]
        app_objs.append(obj)
    L += [""]

    ins = CRT_OBJS + app_objs + CRT_LIBS
    L += [f"build {esc(TARGET)}: link {' '.join(esc(o) for o in ins)}", ""]

    emit_package(L, PKG, TARGET)

    L += ["rule clean", "  command = rm -rf out .pkg", "build clean: clean"]

    (ROOT / "build.ninja").write_text("\n".join(L) + "\n", encoding="utf-8")
    print("已生成 build.ninja（leopard-shell）。用法：ninja build / ninja pack")


if __name__ == "__main__":
    main()
