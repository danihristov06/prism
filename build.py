#!/usr/bin/env python3
"""Self-contained build script for PRISM."""

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.join(ROOT_DIR, "src")
DEPS_DIR = os.path.join(ROOT_DIR, "deps")
BUILD_DIR = os.path.join(ROOT_DIR, "build")

LIBBSC_URL = "https://github.com/IlyaGrebnov/libbsc/releases/download/v3.4.0/bsc-3.4.0-src.zip"
LZMA_URL = "https://github.com/ip7z/7zip/releases/download/26.06/lzma2606.7z"
ZLIB_API = "https://api.github.com/repos/madler/zlib/releases/latest"

HEADERS = {
    "User-Agent": "prism-build/1.0",
    "Accept": "application/vnd.github+json",
}


def download_file(url: str, dest: str) -> None:
    if os.path.exists(dest) and os.path.getsize(dest) > 10_000:
        print(f"  [cached] {dest}")
        return
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    print(f"  Downloading {url} -> {dest}")
    req = urllib.request.Request(url, headers={"User-Agent": HEADERS["User-Agent"]})
    with urllib.request.urlopen(req, timeout=300) as resp, open(dest, "wb") as f:
        shutil.copyfileobj(resp, f)


def fetch_zlib_archive() -> str:
    req = urllib.request.Request(ZLIB_API, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=30) as resp:
        rel = json.loads(resp.read().decode("utf-8"))
    for ext in (".zip", ".tar.gz", ".tar.xz"):
        for a in rel.get("assets", []):
            name = a["name"].lower()
            if name.endswith(ext) and not name.endswith(".asc"):
                dest = os.path.join(DEPS_DIR, a["name"])
                download_file(a["browser_download_url"], dest)
                return dest
    dest = os.path.join(DEPS_DIR, "zlib-src.tar.gz")
    download_file(rel["tarball_url"], dest)
    return dest


def extract_archive(archive_path: str, dest_dir: str) -> None:
    if os.path.exists(dest_dir) and os.listdir(dest_dir):
        return
    os.makedirs(dest_dir, exist_ok=True)
    low = archive_path.lower()
    if low.endswith(".zip"):
        with zipfile.ZipFile(archive_path, "r") as zf:
            zf.extractall(dest_dir)
    elif low.endswith((".tar.gz", ".tgz", ".tar.xz")):
        with tarfile.open(archive_path, "r:*") as tf:
            tf.extractall(dest_dir)
    elif low.endswith(".7z"):
        seven_z = shutil.which("7z") or shutil.which("7za")
        if seven_z:
            subprocess.run([seven_z, "x", "-y", f"-o{dest_dir}", archive_path], check=True)
        else:
            try:
                import py7zr  # type: ignore
                with py7zr.SevenZipFile(archive_path, mode="r") as z:
                    z.extractall(path=dest_dir)
            except ImportError as exc:
                raise RuntimeError(
                    f"Cannot extract {archive_path}: install 7z/7za on PATH or `pip install py7zr`"
                ) from exc


def find_dir_containing(root: str, target_file: str) -> str:
    for dirpath, _, filenames in os.walk(root):
        if target_file in filenames:
            return dirpath
    raise FileNotFoundError(f"Could not find {target_file} under {root}")


def fetch_and_extract_deps(skip_fetch: bool) -> dict:
    os.makedirs(DEPS_DIR, exist_ok=True)
    bsc_zip = os.path.join(DEPS_DIR, "bsc-3.4.0-src.zip")
    lzma_7z = os.path.join(DEPS_DIR, "lzma2606.7z")

    if not skip_fetch:
        print("=== Fetching third-party dependencies ===")
        download_file(LIBBSC_URL, bsc_zip)
        download_file(LZMA_URL, lzma_7z)
        zlib_arc = fetch_zlib_archive()
    else:
        zlib_arc = next(
            (os.path.join(DEPS_DIR, f) for f in os.listdir(DEPS_DIR) if f.startswith("zlib") and not os.path.isdir(os.path.join(DEPS_DIR, f))),
            "",
        )

    bsc_dir = os.path.join(DEPS_DIR, "libbsc_src")
    lzma_dir = os.path.join(DEPS_DIR, "lzma_src")
    zlib_dir = os.path.join(DEPS_DIR, "zlib_src")

    extract_archive(bsc_zip, bsc_dir)
    extract_archive(lzma_7z, lzma_dir)
    if zlib_arc:
        extract_archive(zlib_arc, zlib_dir)

    return {
        "bsc_root": os.path.dirname(find_dir_containing(bsc_dir, "libbsc.h")),
        "lzma_c": find_dir_containing(lzma_dir, "Lzma2Enc.c"),
        "zlib_root": find_dir_containing(zlib_dir, "deflate.c"),
    }


def resolve_tool(name: str, toolchain_dir: str | None) -> str:
    if toolchain_dir:
        for cand in (name, f"{name}.exe"):
            p = os.path.join(toolchain_dir, cand)
            if os.path.exists(p):
                return p
    found = shutil.which(name)
    if not found:
        raise FileNotFoundError(f"Required tool '{name}' not found on PATH (or in --toolchain).")
    return found


def compile_one(compiler: str, src_path: str, flags: list[str], obj_path: str) -> None:
    res = subprocess.run([compiler, *flags, "-c", src_path, "-o", obj_path], capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout, res.stderr, file=sys.stderr)
        raise RuntimeError(f"Compilation failed: {src_path}")


def build_static_lib(ar: str, name: str, jobs: list[tuple[str, str, list[str]]], out_a: str) -> None:
    obj_dir = os.path.join(BUILD_DIR, "objs", name)
    os.makedirs(obj_dir, exist_ok=True)
    objs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        futs = []
        for compiler, src, flags in jobs:
            obj = os.path.join(obj_dir, os.path.basename(src) + ".o")
            objs.append(obj)
            futs.append(ex.submit(compile_one, compiler, src, flags, obj))
        for f in futs:
            f.result()
    if os.path.exists(out_a):
        os.remove(out_a)
    subprocess.run([ar, "rcs", out_a, *objs], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build PRISM from source")
    parser.add_argument("--toolchain", help="Directory containing gcc, g++, ar")
    parser.add_argument("--skip-fetch", action="store_true", help="Use already-downloaded deps/ archives")
    parser.add_argument("--portable", action="store_true", help="Build with -msse4.2 instead of -march=native -mavx2 -mbmi2")
    args = parser.parse_args()

    gcc = resolve_tool("gcc", args.toolchain)
    gpp = resolve_tool("g++", args.toolchain)
    ar = resolve_tool("ar", args.toolchain)

    paths = fetch_and_extract_deps(args.skip_fetch)
    os.makedirs(BUILD_DIR, exist_ok=True)

    opt_flags = ["-O3", "-funroll-loops"]
    if args.portable:
        opt_flags.append("-msse4.2")
    else:
        opt_flags.extend(["-march=native", "-mavx2", "-mbmi2"])

    libz = os.path.join(BUILD_DIR, "libz.a")
    libbsc = os.path.join(BUILD_DIR, "libbsc.a")
    liblzma = os.path.join(BUILD_DIR, "liblzma.a")

    print("=== Building static libraries ===")
    zlib_srcs = [
        "adler32.c", "compress.c", "crc32.c", "deflate.c", "infback.c",
        "inffast.c", "inflate.c", "inftrees.c", "trees.c", "uncompr.c", "zutil.c",
    ]
    build_static_lib(
        ar, "zlib",
        [(gcc, os.path.join(paths["zlib_root"], s), [*opt_flags, "-DUNALIGNED_OK", f"-I{paths['zlib_root']}"]) for s in zlib_srcs],
        libz,
    )

    bsc_c_flags = [*opt_flags, "-fopenmp", "-DLIBSAIS_OPENMP", "-DLIBBSC_OPENMP_SUPPORT", f"-I{paths['bsc_root']}"]
    bsc_cpp_flags = ["-std=c++17", *bsc_c_flags]
    bsc_jobs = [(gcc, os.path.join(paths["bsc_root"], "libbsc/bwt/libsais/libsais.c"), bsc_c_flags)]
    for s in [
        "libbsc/adler32/adler32.cpp", "libbsc/bwt/bwt.cpp", "libbsc/coder/coder.cpp",
        "libbsc/coder/qlfc/qlfc.cpp", "libbsc/coder/qlfc/qlfc_model.cpp",
        "libbsc/filters/detectors.cpp", "libbsc/filters/preprocessing.cpp",
        "libbsc/libbsc/libbsc.cpp", "libbsc/lzp/lzp.cpp",
        "libbsc/platform/platform.cpp", "libbsc/st/st.cpp",
    ]:
        bsc_jobs.append((gpp, os.path.join(paths["bsc_root"], s), bsc_cpp_flags))
    build_static_lib(ar, "bsc", bsc_jobs, libbsc)

    lzma_srcs = [
        "7zCrc.c", "7zCrcOpt.c", "Alloc.c", "Bcj2.c", "Bcj2Enc.c",
        "Bra.c", "Bra86.c", "BraIA64.c", "CpuArch.c", "Delta.c",
        "LzFind.c", "LzFindMt.c", "LzFindOpt.c",
        "LzmaDec.c", "LzmaEnc.c", "Lzma2Dec.c", "Lzma2Enc.c",
        "Ppmd7.c", "Ppmd7Enc.c", "Ppmd7Dec.c",
        "MtCoder.c", "MtDec.c", "Threads.c", "7zStream.c",
    ]
    lzma_flags = [*opt_flags, "-DZ7_LZMA_PROB32", f"-I{paths['lzma_c']}"]
    build_static_lib(ar, "lzma", [(gcc, os.path.join(paths["lzma_c"], s), lzma_flags) for s in lzma_srcs], liblzma)

    out_name = "prism.exe" if os.name == "nt" else "prism"
    out_bin = os.path.join(ROOT_DIR, out_name)
    print(f"=== Linking {out_bin} ===")
    t0 = time.perf_counter()
    cmd = [
        gpp, "-std=c++17", *opt_flags, "-Wall", "-Wextra", "-fopenmp", "-DZ7_LZMA_PROB32",
        f"-I{SRC_DIR}", f"-I{paths['bsc_root']}", f"-I{paths['lzma_c']}", f"-I{paths['zlib_root']}",
        os.path.join(SRC_DIR, "prism_main.cpp"),
        libbsc, liblzma, libz,
        "-static", "-s", "-o", out_bin,
    ]
    subprocess.run(cmd, check=True)
    dt = time.perf_counter() - t0
    print(f"Built {out_bin} ({os.path.getsize(out_bin):,d} B) in {dt:.2f}s")


if __name__ == "__main__":
    main()
