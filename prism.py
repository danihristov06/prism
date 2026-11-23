import json
import os
import re
import struct
import sys
import subprocess
import time
from typing import Optional

def find_prism_exe() -> str:
    """Find the native prism.exe executable."""
    env_exe = os.environ.get("PRISM_EXE")
    if env_exe and os.path.exists(env_exe):
        return env_exe
    
    local_exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prism.exe")
    if os.path.exists(local_exe):
        return local_exe
        
    return "prism.exe"

_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

def compress(src: str, dst: str, check: bool = True, fast: bool = False, overwrite: bool = True, no_overwrite: bool = False, embed_meta: bool = False, ultra: bool = False, level: Optional[int] = None) -> subprocess.CompletedProcess:
    """Compress src to dst using prism."""
    exe = find_prism_exe()
    cmd = [exe, "c", src, dst]
    if level is not None:
        cmd.extend(["--level", str(int(level))])
    elif fast:
        cmd.append("--fast")
    elif ultra:
        cmd.append("--ultra")
    if embed_meta:
        cmd.append("--meta")
    if no_overwrite or not overwrite:
        cmd.append("-n")
    else:
        cmd.append("-y")
    return subprocess.run(cmd, check=check, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)

def decompress(src: str, dst: str, check: bool = True, overwrite: bool = True, no_overwrite: bool = False) -> subprocess.CompletedProcess:
    """Decompress src to dst using prism."""
    exe = find_prism_exe()
    cmd = [exe, "d", src, dst]
    if no_overwrite or not overwrite:
        cmd.append("-n")
    else:
        cmd.append("-y")
    return subprocess.run(cmd, check=check, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)

def info(archive: str) -> str:
    """Get archive info using prism."""
    exe = find_prism_exe()
    res = subprocess.run([exe, "i", archive], capture_output=True, text=True, encoding="utf-8", errors="replace", check=True, creationflags=_NO_WINDOW)
    return res.stdout

def _describe_algorithm(flags: int) -> str:
    parts = []
    if flags & (1 << 6):
        parts.append("Directory")
    if flags & (1 << 0):
        parts.append("CDC")
    if flags & (1 << 1):
        parts.append("TarDemux")
    if flags & (1 << 2):
        parts.append("Stride")
    if flags & (1 << 3):
        parts.append("BCJ2+LZMA (x86)")
    if flags & (1 << 4):
        parts.append("SPARC+LZMA")
    if flags & (1 << 5):
        parts.append("ZipPrecomp+LZMA")
    if flags & (1 << 7):
        parts.append("Parallel LZMA")
    if flags & (1 << 8):
        parts.append("Parallel LZMA2")
    if (flags & ((1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 8))) == 0:
        parts.append("Parallel BSC (BWT+QLFC)")
    return " + ".join(parts) if parts else "Standard"

def _read_metadata_python_fallback(archive: str) -> dict:
    """Fast O(1) pure-Python PRISM header + PMET trailer reader."""
    file_sz = os.path.getsize(archive)
    if file_sz < 28:
        raise ValueError("Invalid PRISM archive: too short")
    with open(archive, "rb") as f:
        hdr = f.read(28)
        if len(hdr) < 28 or hdr[:4] != b"PRISM"[:4]:
            raise ValueError("Invalid PRISM archive magic")
        version, flags, orig_size, comp_size = struct.unpack("<IIQQ", hdr[4:28])
        payload_end = 28 + comp_size
        pmet_data = None
        cursor = file_sz
        while cursor >= payload_end + 16:
            f.seek(cursor - 16)
            tr = f.read(16)
            if len(tr) < 16:
                break
            start_off, blk_len = struct.unpack("<QI", tr[:12])
            magic = tr[12:16]
            if magic not in (b"PIDX", b"PMET"):
                break
            if start_off < payload_end or start_off > cursor - 16 or blk_len != (cursor - 16 - start_off):
                break
            if magic == b"PMET":
                if 0 < blk_len <= 1024 * 1024:
                    f.seek(start_off)
                    raw_json = f.read(blk_len).decode("utf-8", errors="replace")
                    pmet_data = json.loads(raw_json)
                break
            cursor = start_off
    ratio_pct = (file_sz / orig_size * 100.0) if orig_size > 0 else 0.0
    savings_pct = (100.0 - ratio_pct) if orig_size > 0 else 0.0
    return {
        "file": archive,
        "version": version,
        "flags": flags,
        "is_dir": bool(flags & (1 << 6)),
        "orig_bytes": orig_size,
        "payload_bytes": comp_size,
        "archive_bytes": file_sz,
        "ratio_pct": ratio_pct,
        "savings_pct": savings_pct,
        "algorithm": _describe_algorithm(flags),
        "has_pmet": pmet_data is not None,
        "pmet": pmet_data,
    }

def read_metadata(archive: str) -> dict:
    """Read PRISM archive header and optional PMET embedded metadata in O(1)."""
    exe = find_prism_exe()
    res = subprocess.run(
        [exe, "meta", str(archive)],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        creationflags=_NO_WINDOW,
    )
    if res.returncode == 0 and res.stdout.strip():
        try:
            return json.loads(res.stdout)
        except json.JSONDecodeError:
            pass
    return _read_metadata_python_fallback(str(archive))

def verify(archive: str, check: bool = True) -> subprocess.CompletedProcess:
    """Verify archive integrity using prism."""
    exe = find_prism_exe()
    return subprocess.run([exe, "t", archive], check=check, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)

def verify_bench(archive: str, compute_sha256: bool = True) -> dict:
    """Benchmark in-memory decode + integrity verification using `prism t [--sha256]`."""
    exe = find_prism_exe()
    cmd = [exe, "t", str(archive)]
    if compute_sha256:
        cmd.append("--sha256")
    t0 = time.perf_counter()
    res = subprocess.run(cmd, check=False, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)
    wall_ms = (time.perf_counter() - t0) * 1000.0
    parsed = {}
    for line in res.stdout.splitlines():
        if line.startswith("VERIFY_JSON\t"):
            try:
                parsed = json.loads(line.split("\t", 1)[1])
                break
            except Exception:
                pass
    dec_ms = parsed.get("dec_ms")
    if dec_ms is None:
        m = re.search(r"Verify OK!\s*\(\d+\s+bytes,\s*([0-9.]+)\s*ms\)", res.stdout)
        if m:
            try:
                dec_ms = float(m.group(1))
            except ValueError:
                dec_ms = None
    if dec_ms is None and res.returncode == 0:
        dec_ms = wall_ms
    return {
        "ok": res.returncode == 0,
        "dec_ms": float(dec_ms) if dec_ms is not None else None,
        "orig_bytes": int(parsed.get("orig_bytes", 0)) if parsed.get("orig_bytes") is not None else 0,
        "content_bytes": int(parsed.get("content_bytes", 0)) if parsed.get("content_bytes") is not None else 0,
        "sha256": parsed.get("sha256"),
        "is_dir": bool(parsed.get("is_dir", False)),
        "stdout": res.stdout,
        "stderr": res.stderr,
    }

def parse_list_output(stdout: str) -> tuple[dict, list[dict]]:
    """Parse META and ENTRY lines from `prism l` stdout."""
    meta = {}
    entries = []
    for line in stdout.splitlines():
        parts = line.split("\t")
        if not parts:
            continue
        if parts[0] == "META":
            for kv in parts[1:]:
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    meta[k] = v
        elif parts[0] == "ENTRY" and len(parts) >= 4:
            entries.append({
                "path": parts[1],
                "size": int(parts[2]) if parts[2].isdigit() else 0,
                "kind": parts[3]
            })
    return meta, entries

def list_entries(archive: str) -> tuple[dict, list[dict]]:
    """List entries inside a prism archive, parsing META and ENTRY lines."""
    exe = find_prism_exe()
    res = subprocess.run([exe, "l", archive], capture_output=True, text=True, encoding="utf-8", errors="replace", check=True, creationflags=_NO_WINDOW)
    return parse_list_output(res.stdout)

def extract_one(archive: str, entry_path: str, out_file: str, check: bool = True) -> subprocess.CompletedProcess:
    """Extract a single entry from a prism archive."""
    exe = find_prism_exe()
    return subprocess.run([exe, "x1", archive, entry_path, out_file], check=check, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)

def extract_many(archive_path: str, output_dir: str, entry_paths: list[str], check: bool = True) -> subprocess.CompletedProcess:
    """Extract multiple entries from a prism archive in one pass, with x1 fallback for older prism.exe."""
    unique_entries = list(dict.fromkeys(entry_paths))
    if not unique_entries:
        return subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr="")

    exe = find_prism_exe()
    cmd = [exe, "xm", str(archive_path), str(output_dir), *unique_entries]
    res = subprocess.run(cmd, check=False, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)
    if res.returncode == 0:
        return res

    combined = (res.stdout or "") + "\n" + (res.stderr or "")
    if "Usage:" in combined or "unknown command" in combined.lower():
        for ep in unique_entries:
            out_path = os.path.join(str(output_dir), ep.replace("/", os.sep))
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            r1 = extract_one(str(archive_path), ep, out_path, check=False)
            if r1.returncode != 0:
                if check:
                    raise subprocess.CalledProcessError(r1.returncode, r1.args, output=r1.stdout, stderr=r1.stderr)
                return r1
        return subprocess.CompletedProcess(args=[exe, "x1_fallback"], returncode=0, stdout="", stderr="")

    if check:
        raise subprocess.CalledProcessError(res.returncode, res.args, output=res.stdout, stderr=res.stderr)
    return res

if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "gui":
        try:
            import prism_gui
            prism_gui.main()
        except ImportError:
            print("prism_gui module not found.")
            sys.exit(1)
        sys.exit(0)
        
    if len(sys.argv) < 2:
        print("Usage: python prism.py [c|d|i|t|l|x1|xm|gui] ...")
        exe = find_prism_exe()
        try:
            subprocess.run([exe])
        except Exception:
            pass
        sys.exit(1)
    
    exe = find_prism_exe()
    cmd = [exe] + sys.argv[1:]
    res = subprocess.run(cmd)
    sys.exit(res.returncode)
