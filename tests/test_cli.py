"""Тесты CLI на собственных синтетических данных, без игровых файлов."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else ""
if len(sys.argv) > 1:
    del sys.argv[1]


class R12(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "game"
        for name in ("MAPS", "TERRAIN", "GEODATA"):
            (self.root / name).mkdir(parents=True)
        (self.root / "MAPS" / "TEST.MAP").write_bytes(bytes((2, 1, 1)) + bytes(8))
        (self.root / "TERRAIN" / "TEST.MCD").write_bytes(bytes(124))
        (self.root / "GEODATA" / "LOFTEMPS.DAT").write_bytes(bytes(64))

    def run_cli(self, mode="validate", *options):
        p = subprocess.run([BINARY, mode, "--source", str(self.root), *map(str, options)],
                           text=True, capture_output=True, encoding="utf-8")
        body = json.loads(p.stdout) if p.stdout else None
        return p, body

    def test_valid_formats(self):
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(report["checks"], {"files": 3, "passed": 3, "failed": 0})
        self.assertEqual([x["type"] for x in report["resources"]], ["LOFTEMPS", "MAP", "MCD"])

    def test_invalid_map_size(self):
        (self.root / "MAPS" / "TEST.MAP").write_bytes(bytes((2, 1, 1)) + bytes(7))
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertEqual(report["checks"]["failed"], 1)
        self.assertIn("длина", report["resources"][1]["diagnosis"])

    def test_zero_dimension(self):
        (self.root / "MAPS" / "TEST.MAP").write_bytes(bytes((0, 1, 1)))
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertIn("нулевая", report["resources"][1]["diagnosis"])

    def test_invalid_mcd_and_loft(self):
        (self.root / "TERRAIN" / "TEST.MCD").write_bytes(bytes(63))
        (self.root / "GEODATA" / "LOFTEMPS.DAT").write_bytes(bytes(31))
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertEqual(report["checks"]["failed"], 2)

    def test_unrelated_files_are_ignored(self):
        (self.root / "MAPS" / "MALWARE.EXE").write_bytes(b"unrelated")
        (self.root / "cloud_saves").mkdir()
        (self.root / "cloud_saves" / "SAVE.DAT").write_bytes(b"ignore")
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 0)
        self.assertEqual(report["checks"]["files"], 3)

    def test_missing_required_directory(self):
        (self.root / "GEODATA" / "LOFTEMPS.DAT").unlink()
        (self.root / "GEODATA").rmdir()
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertIn("GEODATA", report["diagnostics"][0])

    def test_empty_required_resource_category(self):
        (self.root / "GEODATA" / "LOFTEMPS.DAT").unlink()
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertTrue(any("GEODATA" in x for x in report["diagnostics"]))

    def test_inspect_can_report_invalid_files(self):
        (self.root / "MAPS" / "TEST.MAP").write_bytes(b"broken")
        p, report = self.run_cli("inspect")
        self.assertEqual(p.returncode, 0)
        self.assertEqual(report["checks"]["failed"], 1)

    def test_report_writes_outside_source_only(self):
        target = Path(self.temp.name) / "report.json"
        p, _ = self.run_cli("validate", "--report", target)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(json.loads(target.read_text(encoding="utf-8"))["checks"]["passed"], 3)
        p, _ = self.run_cli("validate", "--report", self.root / "MAPS" / "TEST.MAP")
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual((self.root / "MAPS" / "TEST.MAP").stat().st_size, 11)

    def test_report_does_not_leak_absolute_path(self):
        p, _ = self.run_cli()
        self.assertNotIn(str(self.root), p.stdout)

    @unittest.skipUnless(hasattr(os, "symlink"), "Нужны символические ссылки")
    def test_symbolic_link_is_not_read(self):
        (self.root / "MAPS" / "TEST.MAP").unlink()
        try:
            (self.root / "MAPS" / "TEST.MAP").symlink_to(self.root / "TERRAIN" / "TEST.MCD")
        except (OSError, NotImplementedError):
            self.skipTest("ОС не разрешает символьные ссылки")
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertEqual(report["checks"]["failed"], 1)


if __name__ == "__main__":
    unittest.main()
