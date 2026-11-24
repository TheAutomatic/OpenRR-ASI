# SPDX-License-Identifier: MIT
"""Unit tests for asi/build/package_release.py release audit and deterministic archiving."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
import package_release  # noqa: E402
from build_aco_records import fnv, msgpack  # noqa: E402


def _make_pe64(extra: bytes = b"") -> bytes:
    pe_off = 0x40
    hdr = bytearray(pe_off + 6)
    hdr[:2] = b"MZ"
    struct.pack_into("<I", hdr, 60, pe_off)
    hdr[pe_off:pe_off + 4] = b"PE\0\0"
    struct.pack_into("<H", hdr, pe_off + 4, 0x8664)
    return bytes(hdr) + extra


def _build_valid_stage(root: Path, shader_count: int = 94) -> Path:
    stage = root / "stage"
    (stage / "OptiScaler" / "plugins").mkdir(parents=True)
    (stage / "openrr-amd" / "shaders").mkdir(parents=True)
    (stage / "openrr-amd" / "shaders-gfx11").mkdir(parents=True)
    (stage / "openrr-amd" / "lang" / "fonts").mkdir(parents=True)

    (stage / "OptiScaler" / "plugins" / "openrr-amd.asi").write_bytes(
        _make_pe64(b"\0InitializeASI\0")
    )
    (stage / "openrr-ngx-host.dll").write_bytes(_make_pe64())
    (stage / "openrr-amd.ini").write_text("openrr-amd.ini content\n", encoding="utf-8")
    for dname in package_release.DATA_DOC_FILES:
        (stage / "openrr-amd" / dname).write_text(f"{dname} content\n", encoding="utf-8")

    for lname in package_release.LANG_FILES:
        (stage / "openrr-amd" / "lang" / lname).write_text('msgid "k"\nmsgstr "v"\n', encoding="utf-8")
    for fname in package_release.FONT_FILES:
        (stage / "openrr-amd" / "lang" / "fonts" / fname).write_bytes(b"FONTDATA01")

    records_by_hash = {}
    for idx in range(shader_count):
        stem = f"kernel_{idx:03d}"
        spv = struct.pack("<5I", 0x07230203, 0x00010300, 0, 10 + idx, 0) + struct.pack("<I", idx)
        (stage / "openrr-amd" / "shaders" / f"{stem}.spv").write_bytes(spv)
        (stage / "openrr-amd" / "shaders-gfx11" / f"{stem}.spv").write_bytes(spv)

        key = f"{fnv(spv):016x}"
        nrp_body = bytearray(104)
        struct.pack_into("<8sQQ", nrp_body, 0, b"NRPAL002", int(key, 16), 0x1234 + idx)
        nrp_sha = hashlib.sha256(nrp_body).hexdigest()
        records_by_hash[key] = (
            bytes(nrp_body),
            {
                "spv": f"{stem}.spv",
                "spv_hash": key,
                "code_hash": f"{0x1234 + idx:016x}",
                "workgroup": [8, 8, 1],
                "layout": "sampled",
                "record": f"{key}.nrp",
                "record_sha256": nrp_sha,
            },
        )

    for tgt in package_release.ACO_TARGETS:
        pkg = stage / "openrr-amd" / "aco" / tgt / "openrr-d3d12-v1"
        pkg.mkdir(parents=True)
        mani = {
            "schema": 1,
            "target": tgt,
            "layout_version": 1,
            "wave_size": 32,
            "layout_push_bytes": 244,
            "mesa_commit": "0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4",
            "patch_version": 5,
            "failures": [],
            "records": {},
        }
        for key, (nrp_bytes, row) in records_by_hash.items():
            (pkg / row["record"]).write_bytes(nrp_bytes)
            mani["records"][key] = row
        mani_bytes = json.dumps(mani, indent=2).encode("utf-8")
        (pkg / "manifest.json").write_bytes(mani_bytes)
        idx_mpk = {k: mani[k] for k in ("schema", "target", "layout_version", "wave_size", "layout_push_bytes", "mesa_commit", "patch_version")}
        idx_mpk["manifest_hash"] = fnv(mani_bytes)
        idx_mpk["records"] = {
            k: {f: r[f] for f in ("spv_hash", "code_hash", "workgroup", "layout", "record")}
            for k, r in mani["records"].items()
        }
        (pkg / "manifest.msgpack").write_bytes(msgpack(idx_mpk))

    return stage


class PackageReleaseTests(unittest.TestCase):
    def test_valid_stage_produces_deterministic_zip_and_sidecar(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage = _build_valid_stage(root)
            out1 = root / "out1"
            out2 = root / "out2"
            z1, s1, h1, count, _ = package_release.package_release(stage, out1, "OpenRR-test")
            z2, s2, h2, _, _ = package_release.package_release(stage, out2, "OpenRR-test")
            self.assertEqual(count, 392)
            self.assertEqual(h1, h2)
            self.assertEqual(z1.read_bytes(), z2.read_bytes())
            self.assertEqual(s1.read_text(encoding="utf-8"), f"{h1}  OpenRR-test.zip\n")
            with zipfile.ZipFile(z1, "r") as zf:
                names = zf.namelist()
            self.assertEqual(package_release._read_version_header(Path(__file__).resolve().parents[2]), "0.1.7plus5")
            self.assertEqual(len(names), 392)
            self.assertIn("OptiScaler/plugins/openrr-amd.asi", names)
            self.assertIn("openrr-amd/OpenRR_LICENSE.txt", names)
            self.assertIn("openrr-amd/OpenRR_THIRD_PARTY.md", names)
            self.assertNotIn("LICENSE", names)
            self.assertNotIn("THIRD_PARTY.md", names)
            self.assertNotIn("plugins/openrr-amd.asi", names)
            self.assertTrue(all("\\" not in n for n in names))

    def test_rejects_root_license_or_wrong_doc_casing(self):
        with tempfile.TemporaryDirectory() as td:
            stage = _build_valid_stage(Path(td))
            (stage / "THIRD_PARTY.md").write_text("legacy\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "Legacy root license/notice file"):
                package_release.validate_stage(stage)
            (stage / "THIRD_PARTY.md").unlink()

            (stage / "LICENSE").write_text("legacy\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "Legacy root license/notice file"):
                package_release.validate_stage(stage)
            (stage / "LICENSE").unlink()

            tmp_doc = stage / "openrr-amd" / "_tmp_tp.md"
            (stage / "openrr-amd" / "OpenRR_THIRD_PARTY.md").rename(tmp_doc)
            tmp_doc.rename(stage / "openrr-amd" / "openrr_third_party.md")
            with self.assertRaisesRegex(RuntimeError, "Exact case mismatch"):
                package_release.validate_stage(stage)

    def test_rejects_wrong_folder_casing(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage = _build_valid_stage(root)
            tmp_rename = stage / "_tmp_opti"
            (stage / "OptiScaler").rename(tmp_rename)
            tmp_rename.rename(stage / "optiscaler")
            with self.assertRaisesRegex(RuntimeError, "Exact case mismatch"):
                package_release.validate_stage(stage)

    def test_rejects_log_residue(self):
        with tempfile.TemporaryDirectory() as td:
            stage = _build_valid_stage(Path(td))
            (stage / "OptiScaler" / "plugins" / "openrr-amd.log").write_text("leftover\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "Forbidden staged artifact"):
                package_release.validate_stage(stage)

    def test_rejects_forbidden_env_string_in_asi(self):
        with tempfile.TemporaryDirectory() as td:
            stage = _build_valid_stage(Path(td))
            asi = stage / "OptiScaler" / "plugins" / "openrr-amd.asi"
            asi.write_bytes(_make_pe64(b"\0InitializeASI\0ONR_RR_COMPILER\0"))
            with self.assertRaisesRegex(RuntimeError, "Forbidden test/override strings"):
                package_release.validate_stage(stage)

    def test_rejects_gfx11_fp8_extension_and_corrupt_nrp(self):
        with tempfile.TemporaryDirectory() as td:
            stage = _build_valid_stage(Path(td))
            gfx11_spv = stage / "openrr-amd" / "shaders-gfx11" / "kernel_000.spv"
            orig = gfx11_spv.read_bytes()
            gfx11_spv.write_bytes(orig + b"\0SPV_EXT_shader_float8\0")
            with self.assertRaisesRegex(RuntimeError, "SPV_EXT_shader_float8"):
                package_release.validate_stage(stage)

            gfx11_spv.write_bytes(orig)
            nrp = next((stage / "openrr-amd" / "aco" / "gfx1200" / "openrr-d3d12-v1").glob("*.nrp"))
            nrp.write_bytes(b"CORRUPT!" + nrp.read_bytes()[8:])
            with self.assertRaisesRegex(RuntimeError, "NRPAL002"):
                package_release.validate_stage(stage)

    def test_parallel_zip_matches_stdlib_zipfile_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage = _build_valid_stage(root)
            entries = package_release.validate_stage(stage)

            z_ref = root / "ref.zip"
            with zipfile.ZipFile(z_ref, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
                for rel_posix, src in entries:
                    zi = zipfile.ZipInfo(filename=rel_posix, date_time=(2026, 1, 1, 0, 0, 0))
                    zi.compress_type = zipfile.ZIP_DEFLATED
                    zi.create_system = 3
                    mode = 0o100755 if src.suffix.lower() in (".asi", ".dll") else 0o100644
                    zi.external_attr = mode << 16
                    zf.writestr(zi, src.read_bytes())
            ref_bytes = z_ref.read_bytes()
            ref_sha = hashlib.sha256(ref_bytes).hexdigest()

            z1 = root / "j1.zip"
            z4 = root / "j4.zip"
            h1 = package_release.write_deterministic_zip(entries, z1, jobs=1)
            h4 = package_release.write_deterministic_zip(entries, z4, jobs=4)

            self.assertEqual(h1, ref_sha)
            self.assertEqual(h4, ref_sha)
            self.assertEqual(z1.read_bytes(), ref_bytes)
            self.assertEqual(z4.read_bytes(), ref_bytes)
            self.assertEqual(z4.with_name("j4.zip.sha256").read_text(encoding="utf-8"), f"{ref_sha}  j4.zip\n")

            with zipfile.ZipFile(z4, "r") as zf:
                self.assertIsNone(zf.testzip())
                for info in zf.infolist():
                    self.assertFalse(info.filename.startswith("./"))
                    self.assertNotIn("\\", info.filename)
                    self.assertEqual(info.date_time, (2026, 1, 1, 0, 0, 0))
                    expected_mode = 0o100755 if info.filename.endswith((".asi", ".dll")) else 0o100644
                    self.assertEqual(info.external_attr >> 16, expected_mode)


if __name__ == "__main__":
    unittest.main()
