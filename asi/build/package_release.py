#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate an OpenRR-ASI staged release directory and build a deterministic release archive."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import zipfile
import zlib

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

_FIXED_DATE_TIME = (2026, 1, 1, 0, 0, 0)
_ZIP_CD_STRUCT = struct.Struct("<4s4B4HL2L5H2L")
_ZIP_EOCD_STRUCT = struct.Struct("<4s4H2LH")
_ZIP32_LIMIT = 0xFFFFFFFF
_ZIP_MAX_COUNT = 0xFFFF


def _resolve_jobs(cli_jobs: int | None) -> int:
    if cli_jobs is not None:
        if cli_jobs < 1:
            raise ValueError(f"jobs must be >= 1, got {cli_jobs}")
        return cli_jobs
    env_val = os.environ.get("OPENRR_BUILD_JOBS", "").strip()
    if env_val:
        try:
            n = int(env_val)
            if n >= 1:
                return n
        except ValueError:
            pass
    return os.cpu_count() or 1


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


def _require_exact_child(
    parent: Path,
    name: str,
    expect_dir: bool,
    dir_cache: dict[Path, dict[str, Path]] | None = None,
) -> Path:
    if dir_cache is not None and parent in dir_cache:
        entries = dir_cache[parent]
    else:
        entries = {p.name: p for p in parent.iterdir()}
        if dir_cache is not None:
            dir_cache[parent] = entries
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


def _validate_stage_with_cache(stage: Path, jobs: int = 1) -> tuple[list[tuple[str, Path]], dict[Path, bytes]]:
    """Audit staged files while caching directory scans and validated file bytes."""
    if not stage.is_dir():
        raise RuntimeError(f"Stage directory does not exist: {stage}")

    # Reject any forbidden artifacts anywhere inside stage before packaging.
    actual_rel: dict[str, Path] = {}
    for p in stage.rglob("*"):
        if p.is_file():
            if p.suffix.lower() in FORBIDDEN_SUFFIXES:
                raise RuntimeError(f"Forbidden staged artifact {p.relative_to(stage).as_posix()}")
            actual_rel[p.relative_to(stage).as_posix()] = p

    dir_cache: dict[Path, dict[str, Path]] = {}
    stage_entries = {p.name: p for p in stage.iterdir()}
    dir_cache[stage] = stage_entries

    if any(name.lower() == "plugins" for name in stage_entries):
        raise RuntimeError(f"Legacy root plugins/ directory must not exist in stage: {stage / 'plugins'}")
    for legacy_root in ("license", "third_party.md"):
        hit = next((p for name, p in stage_entries.items() if name.lower() == legacy_root), None)
        if hit is not None:
            raise RuntimeError(f"Legacy root license/notice file must not exist at stage root: {hit}")

    opti_dir = _require_exact_child(stage, "OptiScaler", expect_dir=True, dir_cache=dir_cache)
    plugins_dir = _require_exact_child(opti_dir, "plugins", expect_dir=True, dir_cache=dir_cache)
    asi_path = _require_exact_child(plugins_dir, "openrr-amd.asi", expect_dir=False, dir_cache=dir_cache)

    prefetched: dict[Path, bytes] = {}
    asi_bytes = _verify_pe64(asi_path)
    if b"InitializeASI" not in asi_bytes:
        raise RuntimeError(f"Missing InitializeASI export symbol in {asi_path}")
    leaked = [s.decode("ascii") for s in FORBIDDEN_ENV_STRINGS if s in asi_bytes]
    if leaked:
        raise RuntimeError(f"Forbidden test/override strings present in {asi_path}: {leaked}")
    prefetched[asi_path] = asi_bytes

    expected: set[str] = {"OptiScaler/plugins/openrr-amd.asi"}
    for fname in ROOT_FILES:
        f = _require_exact_child(stage, fname, expect_dir=False, dir_cache=dir_cache)
        if f.stat().st_size == 0:
            raise RuntimeError(f"Empty root file: {f}")
        if fname == "openrr-ngx-host.dll":
            prefetched[f] = _verify_pe64(f)
        expected.add(fname)

    data_dir = _require_exact_child(stage, "openrr-amd", expect_dir=True, dir_cache=dir_cache)
    for dname in DATA_DOC_FILES:
        df = _require_exact_child(data_dir, dname, expect_dir=False, dir_cache=dir_cache)
        if df.stat().st_size == 0:
            raise RuntimeError(f"Empty notice/license file: {df}")
        expected.add(df.relative_to(stage).as_posix())
    shaders_dir = _require_exact_child(data_dir, "shaders", expect_dir=True, dir_cache=dir_cache)
    gfx11_dir = _require_exact_child(data_dir, "shaders-gfx11", expect_dir=True, dir_cache=dir_cache)

    spvs = sorted(shaders_dir.glob("*.spv"))
    gfx11_spvs = sorted(gfx11_dir.glob("*.spv"))
    if len(spvs) < 94:
        raise RuntimeError(f"Expected >= 94 shaders in {shaders_dir}, found {len(spvs)}")
    if {p.name for p in spvs} != {p.name for p in gfx11_spvs}:
        raise RuntimeError("Shader filename set mismatch between shaders/ and shaders-gfx11/")
    for p in spvs:
        expected.add(p.relative_to(stage).as_posix())
    for p in gfx11_spvs:
        expected.add(p.relative_to(stage).as_posix())

    aco_dir = _require_exact_child(data_dir, "aco", expect_dir=True, dir_cache=dir_cache)
    nrp_checks: list[tuple[Path, str]] = []
    for target in ACO_TARGETS:
        tgt_dir = _require_exact_child(aco_dir, target, expect_dir=True, dir_cache=dir_cache)
        pkg_dir = _require_exact_child(tgt_dir, "openrr-d3d12-v1", expect_dir=True, dir_cache=dir_cache)
        mani_json = _require_exact_child(pkg_dir, "manifest.json", expect_dir=False, dir_cache=dir_cache)
        mani_mpk = _require_exact_child(pkg_dir, "manifest.msgpack", expect_dir=False, dir_cache=dir_cache)
        if mani_mpk.stat().st_size == 0:
            raise RuntimeError(f"Empty manifest.msgpack: {mani_mpk}")
        mani_bytes = mani_json.read_bytes()
        prefetched[mani_json] = mani_bytes
        manifest = json.loads(mani_bytes.decode("utf-8"))
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
            rec_file = _require_exact_child(pkg_dir, row["record"], expect_dir=False, dir_cache=dir_cache)
            nrp_checks.append((rec_file, row["record_sha256"]))
            expected.add(rec_file.relative_to(stage).as_posix())

    def _check_spv(p: Path) -> bytes:
        raw = p.read_bytes()
        if not raw:
            raise RuntimeError(f"Empty shader binary: {p}")
        return raw

    def _check_gfx11(p: Path) -> bytes:
        raw = p.read_bytes()
        if not raw:
            raise RuntimeError(f"Empty gfx11 shader binary: {p}")
        if b"SPV_EXT_shader_float8" in raw:
            raise RuntimeError(f"gfx11 shader unexpectedly uses SPV_EXT_shader_float8: {p}")
        return raw

    def _check_nrp(item: tuple[Path, str]) -> bytes:
        rec_file, expected_sha = item
        rec_bytes = rec_file.read_bytes()
        if rec_bytes[:8] != b"NRPAL002":
            raise RuntimeError(f"Invalid NRPAL002 magic in {rec_file}")
        if _sha256(rec_bytes) != expected_sha:
            raise RuntimeError(f"record_sha256 mismatch in {rec_file}")
        return rec_bytes

    effective_jobs = max(1, jobs)
    if effective_jobs == 1:
        for p in spvs:
            prefetched[p] = _check_spv(p)
        for p in gfx11_spvs:
            prefetched[p] = _check_gfx11(p)
        for item in nrp_checks:
            prefetched[item[0]] = _check_nrp(item)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=effective_jobs) as ex:
            spv_futs = [(p, ex.submit(_check_spv, p)) for p in spvs]
            gfx_futs = [(p, ex.submit(_check_gfx11, p)) for p in gfx11_spvs]
            nrp_futs = [(item[0], ex.submit(_check_nrp, item)) for item in nrp_checks]
            try:
                for p, fut in spv_futs:
                    prefetched[p] = fut.result()
                for p, fut in gfx_futs:
                    prefetched[p] = fut.result()
                for rec_file, fut in nrp_futs:
                    prefetched[rec_file] = fut.result()
            except BaseException:
                for _, fut in spv_futs:
                    fut.cancel()
                for _, fut in gfx_futs:
                    fut.cancel()
                for _, fut in nrp_futs:
                    fut.cancel()
                raise

    lang_dir = _require_exact_child(data_dir, "lang", expect_dir=True, dir_cache=dir_cache)
    for lname in LANG_FILES:
        lf = _require_exact_child(lang_dir, lname, expect_dir=False, dir_cache=dir_cache)
        if lf.stat().st_size == 0:
            raise RuntimeError(f"Empty language file: {lf}")
        expected.add(lf.relative_to(stage).as_posix())
    fonts_dir = _require_exact_child(lang_dir, "fonts", expect_dir=True, dir_cache=dir_cache)
    for fname in FONT_FILES:
        ff = _require_exact_child(fonts_dir, fname, expect_dir=False, dir_cache=dir_cache)
        if ff.stat().st_size == 0:
            raise RuntimeError(f"Empty font asset: {ff}")
        expected.add(ff.relative_to(stage).as_posix())

    actual_set = set(actual_rel.keys())
    if actual_set != expected:
        diff = sorted(actual_set ^ expected)
        raise RuntimeError(f"Unexpected or missing files in {stage}: {diff[:10]}")

    return sorted((rel, actual_rel[rel]) for rel in expected), prefetched


def validate_stage(stage: Path) -> list[tuple[str, Path]]:
    """Audit staged files for casing, cleanliness, PE invariants, shaders, ACO records, and langs."""
    entries, _ = _validate_stage_with_cache(stage, jobs=1)
    return entries


def _write_deterministic_zip_stdlib(entries: list[tuple[str, Path]], tmp_path: Path, compresslevel: int | None) -> str:
    archive_level = 9 if compresslevel is None else compresslevel
    with zipfile.ZipFile(tmp_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=archive_level) as zf:
        for rel_posix, src in entries:
            zi = zipfile.ZipInfo(filename=rel_posix, date_time=_FIXED_DATE_TIME)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 3
            if compresslevel is not None:
                zi.compress_level = compresslevel
            mode = 0o100755 if src.suffix.lower() in (".asi", ".dll") else 0o100644
            zi.external_attr = mode << 16
            zf.writestr(zi, src.read_bytes())
    return _sha256(tmp_path.read_bytes())


def write_deterministic_zip(
    entries: list[tuple[str, Path]],
    zip_path: Path,
    *,
    jobs: int | None = None,
    compresslevel: int | None = None,
    prefetched: dict[Path, bytes] | None = None,
) -> str:
    """Create a reproducible zip archive with fixed timestamps and POSIX permissions."""
    effective_jobs = _resolve_jobs(jobs)
    zlib_level = zlib.Z_DEFAULT_COMPRESSION if compresslevel is None else compresslevel
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = zip_path.with_suffix(".zip.tmp")

    if len(entries) > _ZIP_MAX_COUNT:
        zip_sha = _write_deterministic_zip_stdlib(entries, tmp_path, compresslevel)
        tmp_path.replace(zip_path)
        sha_path = zip_path.with_name(zip_path.name + ".sha256")
        sha_path.write_text(f"{zip_sha}  {zip_path.name}\n", encoding="utf-8")
        return zip_sha

    cached = prefetched or {}
    dosdate = ((_FIXED_DATE_TIME[0] - 1980) << 9) | (_FIXED_DATE_TIME[1] << 5) | _FIXED_DATE_TIME[2]
    dostime = (_FIXED_DATE_TIME[3] << 11) | (_FIXED_DATE_TIME[4] << 5) | (_FIXED_DATE_TIME[5] // 2)

    def _compress_item(idx_and_item: tuple[int, tuple[str, Path]]):
        idx, (rel_posix, src) = idx_and_item
        data = cached.get(src)
        if data is None:
            data = src.read_bytes()
        raw_size = len(data)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        comp = zlib.compressobj(zlib_level, zlib.DEFLATED, -15)
        cdata = comp.compress(data) + comp.flush()
        return idx, rel_posix, src.suffix.lower(), raw_size, crc, cdata

    indexed = list(enumerate(entries))
    if effective_jobs > 1 and len(indexed) > 1:
        # Longest-job-first: start compressing the large font and ASI immediately while smaller files run in parallel.
        indexed.sort(
            key=lambda x: (
                len(cached[x[1][1]]) if x[1][1] in cached else x[1][1].stat().st_size,
                -x[0],
            ),
            reverse=True,
        )
        compressed = [None] * len(entries)
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(effective_jobs, len(indexed))) as ex:
            futs = [ex.submit(_compress_item, item) for item in indexed]
            try:
                for fut in concurrent.futures.as_completed(futs):
                    res = fut.result()
                    compressed[res[0]] = res
            except BaseException:
                for fut in futs:
                    fut.cancel()
                concurrent.futures.wait(futs)
                raise
    else:
        compressed = [_compress_item(item) for item in indexed]

    records: list[tuple[bytes, zipfile.ZipInfo, bytes, bytes, int, int, int]] = []
    offset = 0
    needs_zip64 = False
    for _, rel_posix, suffix, raw_size, crc, cdata in compressed:
        comp_size = len(cdata)
        if raw_size > _ZIP32_LIMIT or comp_size > _ZIP32_LIMIT or offset > _ZIP32_LIMIT:
            needs_zip64 = True
            break
        zi = zipfile.ZipInfo(filename=rel_posix, date_time=_FIXED_DATE_TIME)
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.create_system = 3
        mode = 0o100755 if suffix in (".asi", ".dll") else 0o100644
        zi.external_attr = mode << 16
        try:
            fname_bytes = rel_posix.encode("ascii")
            flag_bits = 0
        except UnicodeEncodeError:
            fname_bytes = rel_posix.encode("utf-8")
            flag_bits = 0x800
        zi.flag_bits = flag_bits
        zi.CRC = crc
        zi.file_size = raw_size
        zi.compress_size = comp_size
        zi.header_offset = offset
        local_header = zi.FileHeader(zip64=False)
        if offset + len(local_header) + comp_size > _ZIP32_LIMIT:
            needs_zip64 = True
            break
        records.append((local_header, zi, cdata, fname_bytes, raw_size, comp_size, crc))
        offset += len(local_header) + comp_size

    if not needs_zip64:
        cd_offset = offset
        cd_chunks: list[bytes] = []
        for _, zi, _, fname_bytes, raw_size, comp_size, crc in records:
            cd_chunks.append(
                _ZIP_CD_STRUCT.pack(
                    b"PK\x01\x02",
                    zi.create_version,
                    zi.create_system,
                    zi.extract_version,
                    zi.reserved,
                    zi.flag_bits,
                    zi.compress_type,
                    dostime,
                    dosdate,
                    crc,
                    comp_size,
                    raw_size,
                    len(fname_bytes),
                    0,
                    0,
                    0,
                    zi.internal_attr,
                    zi.external_attr,
                    zi.header_offset,
                )
            )
            cd_chunks.append(fname_bytes)
        cd_bytes = b"".join(cd_chunks)
        cd_size = len(cd_bytes)
        if cd_offset > _ZIP32_LIMIT or cd_size > _ZIP32_LIMIT:
            needs_zip64 = True

    if needs_zip64:
        zip_sha = _write_deterministic_zip_stdlib(entries, tmp_path, compresslevel)
    else:
        eocd = _ZIP_EOCD_STRUCT.pack(
            b"PK\x05\x06",
            0,
            0,
            len(records),
            len(records),
            cd_size,
            cd_offset,
            0,
        )
        hasher = hashlib.sha256()
        with tmp_path.open("wb") as fp:
            for local_header, _, cdata, _, _, _, _ in records:
                fp.write(local_header)
                hasher.update(local_header)
                fp.write(cdata)
                hasher.update(cdata)
            fp.write(cd_bytes)
            hasher.update(cd_bytes)
            fp.write(eocd)
            hasher.update(eocd)
        zip_sha = hasher.hexdigest()

    tmp_path.replace(zip_path)
    sha_path = zip_path.with_name(zip_path.name + ".sha256")
    sha_path.write_text(f"{zip_sha}  {zip_path.name}\n", encoding="utf-8")
    return zip_sha


def package_release(
    stage: Path,
    out_dir: Path,
    name: str,
    *,
    jobs: int | None = None,
) -> tuple[Path, Path, str, int, int]:
    effective_jobs = _resolve_jobs(jobs)
    entries, prefetched = _validate_stage_with_cache(stage, jobs=effective_jobs)
    uncompressed = sum(len(prefetched[p]) if p in prefetched else p.stat().st_size for _, p in entries)
    zip_path = out_dir / f"{name}.zip"
    zip_sha = write_deterministic_zip(entries, zip_path, jobs=effective_jobs, prefetched=prefetched)
    sha_path = zip_path.with_name(zip_path.name + ".sha256")
    return zip_path, sha_path, zip_sha, len(entries), uncompressed


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", type=Path, default=Path("build/asi/stage"))
    ap.add_argument("--out-dir", type=Path, default=Path("dist"))
    ap.add_argument("--version", default=None)
    ap.add_argument("--name", default=None)
    ap.add_argument("--jobs", "-j", type=int, default=None)
    ns = ap.parse_args()
    if ns.jobs is not None and ns.jobs < 1:
        ap.error("--jobs must be >= 1")

    version = ns.version or _read_version_header(repo_root)
    name = ns.name or f"OpenRR-v{version}-OptiScaler"
    try:
        zip_path, sha_path, zip_sha, count, raw_bytes = package_release(ns.stage, ns.out_dir, name, jobs=ns.jobs)
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
