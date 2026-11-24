# PRISM — High-Ratio Parallel Archiver

[![Release](https://img.shields.io/badge/release-v1.0.2-3fb950?style=flat-square)](https://github.com/danihristov06/prism/releases/tag/v1.0.2)
[![License](https://img.shields.io/badge/license-Apache--2.0-58a6ff?style=flat-square)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?style=flat-square)](#building-from-source)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-lightgrey?style=flat-square)](#prebuilt-release)

**PRISM** is a multi-threaded C++17 solid directory and file archiver engineered for **maximum compression ratio without sacrificing speed**. Across both standard benchmarks (Silesia Corpus, `dedup_suite`) and large polyglot real-world projects, **PRISM beats `lzma2` (7-Zip `-mx=9`) at both compression ratio and speed** (encode and decode), while guaranteeing **100% lossless recovery** with built-in SHA-256 verification.

---

## Key Features & Architecture

- **Content-Aware Multi-Bucket Solid Archiving**: Automatically classifies files into specialized solid streams (`TEXT`, `EXEC_X86`, `BINARY`, `TAR_ARCHIVE`, `META`) and encodes/decodes them concurrently so similar data shares a unified dictionary.
- **Content-Defined Chunking (CDC) Deduplication**: Fast rolling-hash chunking eliminates cross-file and cross-version redundancy before entropy coding.
- **4-Way Associative Long-Range Matcher (LRM)**: Captures long-distance matches across a `512 MB` window ahead of the backend codecs.
- **Hybrid Parallel Backends**:
  - **Parallel BSC (`BWT + QLFC`)**: High-throughput Burrows-Wheeler + Quantized Local Frequency Coding for text, JSON, JS/TS, source code, and structured logs.
  - **Parallel LZMA2 + BCJ2 + `PMCH`**: Multi-threaded LZMA2 with adaptive `(lc, lp, pb)` context probing, x86 `BCJ2`, Mach-O (`ARM64` & `x86_64`) and ELF64 (`ARM64` & `x86_64`) branch filters, plus a PE `SizeOfCode` guard so pure data-table DLLs (e.g. `icudt64.dll`) are not mangled.
- **Columnar Stride v2 Preconditioner**: Auto-detects 8/32/64-bit record strides, applies column-wise delta coding + blocked byte transposition, and un-transposes in parallel on decode.
- **TarDemux & Lossless Deflate Precompression**: Unpacks `.tar` / `.tar.gz` / `.zip` streams losslessly into solid buckets and recompresses with zero-copy deflate reconstruction.
- **Low-Level Win32 & SIMD Optimizations**: Direct Win32 `CreateFileW`/`ReadFile` I/O, parallel `BCJ2` stream decoding, 3-stage PGO (Profile-Guided Optimization) builds, Python wrapper (`prism.py`), and Tkinter Desktop GUI (`prism_gui.py`).

---

## Performance Benchmarks (PRISM vs `lzma2`)

> **Note:** File sizes below are reported in binary **MB** (`bytes / 1,048,576`, matching Windows Explorer and the PRISM GUI).

### 1. Real-World Polyglot Project (`467.73 MB`, 35,576 files)

Mixed real-world application tree containing `153 MB` Mach-O + DWARF binaries, `103 MB` `.js`, `42.58 MB` `.tar.gz`, `39.4 MB` `.map`, `32.8 MB` `.dll`, and `22.7 MB` `.ts`.

![Real-World Polyglot Project: PRISM v1.0.2 vs 7-Zip LZMA2](docs/charts/realworld_vs_lzma2.png)

| Archiver / Preset | Compressed Size | Size vs `lzma2` | Encode Time | Encode Speedup | Decode / SHA-256 Verify |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **7-Zip `lzma2` (`-mx=9`)** | `104.00 MB` | Baseline | `80.00s` | `1.00x` | — |
| **PRISM Normal (`--level 5`)** | **`81.25 MB`** | **`-21.9%` smaller** | **`34.09s`** | **`2.35x` faster** | **`4.23s`** / **`4.66s`** (`100.4 MiB/s`) |
| **PRISM Ultra (`--level 9`)** | **`79.58 MB`** | **`-23.5%` smaller** | **`57.38s`** | **`1.39x` faster** | **`5.77s`** / **`5.87s`** (`79.7 MiB/s`) |

---

### 2. Silesia Corpus & Deduplication Suite (`10/10` Wins vs `lzma2`)

PRISM beats 7-Zip `lzma2` (`-mx=9`) on **every single target** in compression ratio, encode speed, and decode speed:

![Silesia Corpus Compression Ratio: PRISM vs 7-Zip LZMA2](docs/charts/silesia_vs_lzma2_ratio.png)

![Silesia Corpus Encode and Decode Speed: PRISM vs 7-Zip LZMA2](docs/charts/silesia_vs_lzma2_speed.png)

<details>
<summary>Full 10-target Silesia &amp; Dedup Suite table (from <code>prism_benchmark_results.json</code>)</summary>

| Target | Original (MB) | `lzma2` Ratio | PRISM Ratio | `lzma2` Enc | PRISM Enc | `lzma2` Dec | PRISM Dec |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| `silesia.tar` | `202.14` | `22.97%` | **`21.28%`** | `2.71 MiB/s` | **`22.72 MiB/s`** | `107.6 MiB/s` | **`115.1 MiB/s`** |
| `silesia_dir` | `202.12` | `22.97%` | **`21.28%`** | `1.61 MiB/s` | **`19.04 MiB/s`** | `40.3 MiB/s` | **`116.2 MiB/s`** |
| `mozilla` | `48.85` | `26.05%` | **`24.55%`** | `1.80 MiB/s` | **`10.40 MiB/s`** | `69.1 MiB/s` | **`110.4 MiB/s`** |
| `ooffice` | `5.87` | `39.43%` | **`35.31%`** | `4.37 MiB/s` | **`7.78 MiB/s`** | `41.9 MiB/s` | **`46.5 MiB/s`** |
| `osdb` | `9.62` | `28.28%` | **`26.75%`** | `3.80 MiB/s` | **`39.42 MiB/s`** | `63.9 MiB/s` | **`65.3 MiB/s`** |
| `dickens` | `9.72` | `27.78%` | **`25.82%`** | `1.63 MiB/s` | **`45.03 MiB/s`** | `63.3 MiB/s` | **`72.5 MiB/s`** |
| `xml` | `5.10` | `8.51%` | **`8.33%`** | `3.90 MiB/s` | **`59.25 MiB/s`** | `41.0 MiB/s` | **`71.8 MiB/s`** |
| `mr` | `9.51` | `27.56%` | **`24.55%`** | `1.14 MiB/s` | **`50.22 MiB/s`** | `17.9 MiB/s` | **`48.2 MiB/s`** |
| `sao` | `6.92` | `60.87%` | **`55.90%`** | `1.44 MiB/s` | **`9.76 MiB/s`** | `12.6 MiB/s` | **`68.3 MiB/s`** |
| `dedup_suite` | `128.87` | `14.53%` | **`14.30%`** | `1.20 MiB/s` | **`16.32 MiB/s`** | `55.9 MiB/s` | **`169.7 MiB/s`** |

</details>

---

### 3. PRISM v1.0.2 vs v1.0.1 Improvements

![PRISM v1.0.2 vs v1.0.1 Improvements](docs/charts/prism_v102_vs_v101.png)

---

### 4. PRISM v1.0.2 (PGO) 9-Workload Verification Suite

![PRISM v1.0.2 PGO 9-Workload Suite](docs/charts/prism_v102_suite.png)

---

## Compression Presets

| Preset | CLI Flag | Description |
| :--- | :--- | :--- |
| **Fast** | `--fast` / `--level 1` | Maximum throughput for quick archiving |
| **Normal** | `--level 5` (default) | Balanced speed & high ratio (`64 MB` solid LZMA2, 2-way LRM) |
| **Ultra** | `--ultra` / `--level 9` | Maximum compression ratio (adaptive codec/context probe, 4-way LRM, `128 MB` solid LZMA2) |

---

## Usage

### Command-Line Interface (`prism.exe`)

```bash
# Compress a file or directory (default: Normal / --level 5)
prism c <input_path> <output.prism> [-y|-n] [--meta]

# Compress with Ultra preset (--level 9, smallest size)
prism c <input_path> <output.prism> --ultra -y

# Compress with a specific level (1..9)
prism c <input_path> <output.prism> --level 7 -y

# Decompress
prism d <input.prism> <output_path> [-y|-n]

# Verify archive integrity (lossless + SHA-256 check)
prism t <input.prism> --sha256

# Inspect archive metadata / list contents
prism i <input.prism>
prism l <input.prism>

# Extract single or multiple entries
prism x1 <input.prism> <entry_path> <output_file> [-y|-n]
prism xm <input.prism> <out_dir> <entry1> [entry2 ...] [-y|-n]
```

### Desktop GUI & Python API

```bash
# Launch the Tkinter Desktop GUI
python prism_gui.py
```

```python
import prism

prism.compress("my_folder", "archive.prism", ultra=True)
prism.decompress("archive.prism", "restored_folder")
```

---

## Building from Source

### Prerequisites

- **C++17 compiler** with OpenMP (`gcc` / `g++` / `ar` — e.g. [w64devkit](https://github.com/skeeto/w64devkit) or MinGW-w64 on Windows)
- **Python 3.9+** (used by `build.py` to fetch dependencies and orchestrate parallel builds)

### Build Steps

```bash
git clone https://github.com/danihristov06/prism.git
cd prism

# Fetch libbsc, LZMA SDK, and zlib, then compile static libs + prism.exe
python build.py
```

Optional flags:
- `--portable`: compile with `-msse4.2` instead of `-march=native -mavx2 -mbmi2` for older CPUs.
- `--toolchain <dir>`: point to a directory containing `gcc`, `g++`, and `ar`.
- `--skip-fetch`: reuse already-downloaded archives in `deps/`.

---

## Prebuilt Release

Prebuilt Windows x64 PGO binaries (`prism-v1.0.2-windows-x64.zip`, `prism.exe`, and `SHA256SUMS.txt`) are available on the [GitHub Releases](https://github.com/danihristov06/prism/releases) page.

---

## What's New

### v1.0.2

- **Hardware SHA-NI Acceleration**: Added x86 SHA-NI (`_mm_sha256rnds2_epu32`, `_mm_sha256msg1_epu32`, `_mm_sha256msg2_epu32`) for `Sha256::compress_block` with automatic scalar fallback for `--portable` builds, cutting 467 MB SHA-256 verification overhead from ~2.2–2.6s to ~0.1–0.4s.
- **Zero-Alloc `PDIR` SHA-256 Verification**: Eliminated 35,576+ `std::string` / `std::to_string` heap allocations in `cmd_verify` using zero-copy `const char*` views, stack formatting, and an `O(N)` already-sorted fast path.
- **OpenMP Thread-Limit Control & LPT Chunk Scheduling**: Added `decode_thread_limit()` to prevent nested OpenMP oversubscription across `bsc`, `lzma2`, `lzma`, and `ppmd` decoders, plus Longest-Processing-Time (`comp_lens[a] > comp_lens[b]`) chunk scheduling.
- **Parallel Decode Pipelines**: Parallelized `PMCH` section branch filtering, `lrm_lzma2_decompress` (`tokens` + `literals`), `exec_x86` (`c_main` + `c_call`/`c_jump`/`c_rc`), `tar_demux_decode`, `cdc_decode`, and `decode_tar_demux_parts` (running `small_items` concurrently while `large_items` decode).
- **Improved Ultra 96 KB LZMA Probe**: Upgraded the Ultra chunk probe (`level=3, btMode=1, fb=32, mc=16, dict=256KB`) and tightened BSC hijack thresholds (`<= 95%` Ultra / `<= 96%` Normal), fixing the `exec_x86` Ultra regression (`-4.60%` smaller, `+368.7%` faster decode).
- **Benchmark Gains vs v1.0.1**:
  - **Real-World Polyglot (`467.73 MB`)**: Normal encode **`-5.9%`** (`34.09s` vs `36.23s`), Normal decode **`-8.0%`** (`4.23s` vs `4.60s`), Normal SHA-256 verify **`-35.5%`** (`4.66s` vs `7.23s`); Ultra encode **`-5.1%`** (`57.38s` vs `60.47s`), Ultra SHA-256 verify **`-25.2%`** (`5.87s` vs `7.85s`).
  - **9-Workload Normal (`--level 5`)**: GeoMean **`+3.75%` faster encode**, **`+2.02%` faster decode** (`exec_x86` `+32.1%`), **`+0.000%` size**.
  - **9-Workload Ultra (`--level 9`)**: GeoMean **`-0.804%` smaller** (`exec_x86` `-4.602%`) and **`+13.17%` faster decode** (`exec_x86` `+368.7%`).

<details>
<summary>v1.0.1 Changelog</summary>

- **Adaptive Per-Chunk Codec & LZMA2 Context Selector**: Probes each >= 2 MB chunk using a 96 KB sample across 4 LZMA `(lc, lp, pb)` configurations and `libbsc` (`QLFC_FAST`), automatically selecting `libbsc` (`0xFE`) when it beats LZMA2 by >= 3% or the optimal LZMA2 context otherwise.
- **4-Way Associative Long-Range Matcher (LRM)**: Expanded LRM window to `512 MB` with 4-way slot associativity (`mode >= 3`) and backend-aware Ultra thresholds (`32 MB` distance / `192 B` min match).
- **Universal Binary Branch Filter (ELF64 + Mach-O + PE)**: Extended the `PMCH` filter to detect 64-bit little-endian ELF binaries (`x86_64` & `ARM64` executable `PT_LOAD` segments) alongside Mach-O (`__TEXT,__text`) and PE `BCJ2`.
- **Full Multi-Bucket Parallel Pipeline & Solid Normal Blocks**: Runs `TEXT`, `BINARY`, `EXEC_X86`, and `TAR_ARCHIVE` buckets concurrently, and uses `64 MB` solid LZMA2 dictionaries in Normal mode (`--level 5`).

</details>

---

## Roadmap & TODO

- **Desktop GUI (`prism_gui.py`)**:
  - [ ] Improve overall UI layout, visual polish, and responsiveness
  - [ ] Fix UI loading states (non-blocking async loading/progress, prevent freezes on large archives, clean cancellation)
  - [ ] Support opening files directly from inside an archive (extract-to-temp and open with default OS application)
  - [ ] Fix overwrite behavior (currently not working properly — reliable overwrite / skip / prompt handling in GUI and CLI sync)
  - [ ] Add drag-and-drop archiving/extraction, in-archive search/filter, and persistent settings
- **CLI, Packaging & Platform**:
  - [ ] Windows Explorer context-menu integration & `.prism` file association
  - [ ] Standalone GUI bundle / installer (no separate Python install needed)
  - [ ] Automated Linux & macOS builds and CI release pipeline
- **Compression Engine**:
  - [ ] Further Ultra decode throughput tuning without sacrificing compression ratio
  - [ ] Archive append/update support and streaming (`stdin`/`stdout`) mode

---

## Third-Party Libraries & License

PRISM is licensed under the **Apache License 2.0** (see [`LICENSE`](LICENSE)).

- [libbsc](https://github.com/IlyaGrebnov/libbsc) — Apache License 2.0
- [LZMA SDK](https://github.com/ip7z/7zip) — Public Domain
- [zlib](https://github.com/madler/zlib) — zlib License
