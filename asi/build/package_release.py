#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate an OpenRR-ASI staged release directory and build a deterministic release archive."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zipfile

FORBIDDEN_ENV_STRINGS = (
    b"ONR_RR_COMPILER",
    b"ONR_ACO_OPTIONAL",
    b"ONR_ACO_TARGET",
    b"ONR_ACO_DIR",
    b"ONR_ACO_DUMP",
    b"ONR_TEST_SPOOF_LOADER",
    b"ONR_TEST_SIMULATE_RADV",
)
FORBIDDEN_SUFFIXES = {".log", ".pdb", ".debug", ".tmp", ".bak", ".pyc"}
ACO_TARGETS = ("gfx1201", "gfx1200")
LANG_FILES = ("English.txt", "zh-CN.txt", "ja.txt", "ko.txt")
FONT_FILES = ("Noto-CJK-LICENSE.txt", "README.txt", "NotoSansCJKsc-Regular.ttf")
ROOT_FILES = ("openrr-amd.ini", "openrr-ngx-host.dll")
DATA_DOC_FILES = ("OpenRR_LICENSE.txt", "OpenRR_THIRD_PARTY.md")


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _verify_pe64(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError(f"Invalid DOS header in {path}")
    pe_off = struct.unpack_from("<I", data, 60)[0]
    if pe_off + 6 > len(data) or data[pe_off:pe_off + 4] != b"PE\0\0":
        raise RuntimeError(f"Invalid PE header in {path}")
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    if machine != 0x8664:
        raise RuntimeError(f"Expected x86_64 PE (0x8664) in {path}, got 0x{machine:04x}")
    return data


def _require_exact_child(parent: Path, name: str, expect_dir: bool) -> Path:
    entries = {p.name: p for p in parent.iterdir()}
    if name not in entries:
        lower_map = {k.lower(): k for k in entries}
        actual = lower_map.get(name.lower())
        if actual:
            raise RuntimeError(f"Exact case mismatch in {parent}: expected '{name}', found '{actual}'")
        raise RuntimeError(f"Missing required {'directory' if expect_dir else 'file'}: {parent / name}")
    child = entries[name]
    if expect_dir and not child.is_dir():
        raise RuntimeError(f"Expected directory at {child}")
    if not expect_dir and not child.is_file():
        raise RuntimeError(f"Expected regular file at {child}")
    return child


def _read_version_header(repo_root: Path) -> str:
    hdr = repo_root / "runtime" / "include" / "opennr_rr_api.h"
    m = re.search(r'#define\s+ONR_RR_RUNTIME_VERSION\s+"([^"]+)"', hdr.read_text(encoding="utf-8"))
    if not m:
        raise RuntimeError(f"Unable to parse ONR_RR_RUNTIME_VERSION from {hdr}")
    return m.group(1)


def validate_stage(stage: Path) -> list[tuple[str, Path]]:
    """Audit staged files for casing, cleanliness, PE invariants, shaders, ACO records, and langs."""
    if not stage.is_dir():
        raise RuntimeError(f"Stage directory does not exist: {stage}")

    # Reject any forbidden artifacts anywhere inside stage before packaging.
    actual_rel: dict[str, Path] = {}
    for p in stage.rglob("*"):
        if p.is_file():
            if p.suffix.lower() in FORBIDDEN_SUFFIXES:
                raise RuntimeError(f"Forbidden staged artifact {p.relative_to(stage).as_posix()}")
            actual_rel[p.relative_to(stage).as_posix()] = p

    if any(p.name.lower() == "plugins" for p in stage.iterdir()):
        raise RuntimeError(f"Legacy root plugins/ directory must not exist in stage: {stage / 'plugins'}")
    for legacy_root in ("license", "third_party.md"):
        hit = next((p for p in stage.iterdir() if p.name.lower() == legacy_root), None)
        if hit is not None:
            raise RuntimeError(f"Legacy root license/notice file must not exist at stage root: {hit}")

    opti_dir = _require_exact_child(stage, "OptiScaler", expect_dir=True)
    plugins_dir = _require_exact_child(opti_dir, "plugins", expect_dir=True)
    asi_path = _require_exact_child(plugins_dir, "openrr-amd.asi", expect_dir=False)

    asi_bytes = _verify_pe64(asi_path)
    if b"InitializeASI" not in asi_bytes:
        raise RuntimeError(f"Missing InitializeASI export symbol in {asi_path}")
    leaked = [s.decode("ascii") for s in FORBIDDEN_ENV_STRINGS if s in asi_bytes]
    if leaked:
        raise RuntimeError(f"Forbidden test/override strings present in {asi_path}: {leaked}")

    expected: set[str] = {"OptiScaler/plugins/openrr-amd.asi"}
    for fname in ROOT_FILES:
        f = _require_exact_child(stage, fname, expect_dir=False)
        if f.stat().st_size == 0:
            raise RuntimeError(f"Empty root file: {f}")
        if fname == "openrr-ngx-host.dll":
            _verify_pe64(f)
        expected.add(fname)

    data_dir = _require_exact_child(stage, "openrr-amd", expect_dir=True)
    for dname in DATA_DOC_FILES:
        df = _require_exact_child(data_dir, dname, expect_dir=False)
        if df.stat().st_size == 0:
            raise RuntimeError(f"Empty notice/license file: {df}")
        expected.add(df.relative_to(stage).as_posix())
    shaders_dir = _require_exact_child(data_dir, "shaders", expect_dir=True)
    gfx11_dir = _require_exact_child(data_dir, "shaders-gfx11", expect_dir=True)

    spvs = sorted(shaders_dir.glob("*.spv"))
    gfx11_spvs = sorted(gfx11_dir.glob("*.spv"))
    if len(spvs) < 94:
        raise RuntimeError(f"Expected >= 94 shaders in {shaders_dir}, found {len(spvs)}")
    if {p.name for p in spvs} != {p.name for p in gfx11_spvs}:
        raise RuntimeError("Shader filename set mismatch between shaders/ and shaders-gfx11/")
    for p in spvs:
        if p.stat().st_size == 0:
            raise RuntimeError(f"Empty shader binary: {p}")
        expected.add(p.relative_to(stage).as_posix())
    for p in gfx11_spvs:
        raw = p.read_bytes()
        if not raw:
            raise RuntimeError(f"Empty gfx11 shader binary: {p}")
        if b"SPV_EXT_shader_float8" in raw:
            raise RuntimeError(f"gfx11 shader unexpectedly uses SPV_EXT_shader_float8: {p}")
        expected.add(p.relative_to(stage).as_posix())

    aco_dir = _require_exact_child(data_dir, "aco", expect_dir=True)
    for target in ACO_TARGETS:
        tgt_dir = _require_exact_child(aco_dir, target, expect_dir=True)
        pkg_dir = _require_exact_child(tgt_dir, "openrr-d3d12-v1", expect_dir=True)
        mani_json = _require_exact_child(pkg_dir, "manifest.json", expect_dir=False)
        mani_mpk = _require_exact_child(pkg_dir, "manifest.msgpack", expect_dir=False)
        if mani_mpk.stat().st_size == 0:
            raise RuntimeError(f"Empty manifest.msgpack: {mani_mpk}")
        manifest = json.loads(mani_json.read_text(encoding="utf-8"))
        if manifest.get("failures"):
            raise RuntimeError(f"ACO manifest contains failures for {target}: {manifest['failures']}")
        records = manifest.get("records", {})
        if len(records) != len(spvs):
            raise RuntimeError(
                f"Expected {len(spvs)} ACO records for {target}, found {len(records)} in {mani_json}"
            )
        expected.add(mani_json.relative_to(stage).as_posix())
        expected.add(mani_mpk.relative_to(stage).as_posix())
        for row in records.values():
            rec_file = _require_exact_child(pkg_dir, row["record"], expect_dir=False)
            rec_bytes = rec_file.read_bytes()
            if rec_bytes[:8] != b"NRPAL002":
                raise RuntimeError(f"Invalid NRPAL002 magic in {rec_file}")
            if _sha256(rec_bytes) != row["record_sha256"]:
                raise RuntimeError(f"record_sha256 mismatch in {rec_file}")
            expected.add(rec_file.relative_to(stage).as_posix())

    lang_dir = _require_exact_child(data_dir, "lang", expect_dir=True)
    for lname in LANG_FILES:
        lf = _require_exact_child(lang_dir, lname, expect_dir=False)
        if lf.stat().st_size == 0:
            raise RuntimeError(f"Empty language file: {lf}")
        expected.add(lf.relative_to(stage).as_posix())
    fonts_dir = _require_exact_child(lang_dir, "fonts", expect_dir=True)
    for fname in FONT_FILES:
        ff = _require_exact_child(fonts_dir, fname, expect_dir=False)
        if ff.stat().st_size == 0:
            raise RuntimeError(f"Empty font asset: {ff}")
        expected.add(ff.relative_to(stage).as_posix())

    actual_set = set(actual_rel.keys())
    if actual_set != expected:
        diff = sorted(actual_set ^ expected)
        raise RuntimeError(f"Unexpected or missing files in {stage}: {diff[:10]}")

    return sorted((rel, actual_rel[rel]) for rel in expected)


def write_deterministic_zip(entries: list[tuple[str, Path]], zip_path: Path) -> str:
    """Create a reproducible zip archive with fixed timestamps and POSIX permissions."""
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = zip_path.with_suffix(".zip.tmp")
    with zipfile.ZipFile(tmp_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rel_posix, src in entries:
            zi = zipfile.ZipInfo(filename=rel_posix, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 3
            mode = 0o100755 if src.suffix.lower() in (".asi", ".dll") else 0o100644
            zi.external_attr = mode << 16
            zf.writestr(zi, src.read_bytes())
    tmp_path.replace(zip_path)
    zip_sha = _sha256(zip_path.read_bytes())
    sha_path = zip_path.with_name(zip_path.name + ".sha256")
    sha_path.write_text(f"{zip_sha}  {zip_path.name}\n", encoding="utf-8")
    return zip_sha


def package_release(stage: Path, out_dir: Path, name: str) -> tuple[Path, Path, str, int, int]:
    entries = validate_stage(stage)
    uncompressed = sum(p.stat().st_size for _, p in entries)
    zip_path = out_dir / f"{name}.zip"
    zip_sha = write_deterministic_zip(entries, zip_path)
    sha_path = zip_path.with_name(zip_path.name + ".sha256")
    return zip_path, sha_path, zip_sha, len(entries), uncompressed


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", type=Path, default=Path("build/asi/stage"))
    ap.add_argument("--out-dir", type=Path, default=Path("dist"))
    ap.add_argument("--version", default=None)
    ap.add_argument("--name", default=None)
    ns = ap.parse_args()

    version = ns.version or _read_version_header(repo_root)
    name = ns.name or f"OpenRR-v{version}-OptiScaler"
    try:
        zip_path, sha_path, zip_sha, count, raw_bytes = package_release(ns.stage, ns.out_dir, name)
    except Exception as exc:
        print(f"FAIL: package_release: {exc}", file=sys.stderr)
        return 1

    print(
        f"Packaged {count} files ({raw_bytes:,} B -> {zip_path.stat().st_size:,} B) "
        f"into {zip_path} (sha256={zip_sha}, sidecar={sha_path})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
