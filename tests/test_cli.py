"""Тесты CLI на собственных синтетических данных, без игровых файлов."""
from concurrent.futures import ThreadPoolExecutor
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
                           text=True, capture_output=True, encoding="utf-8", timeout=20)
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
        new_input_file = self.root / "MAPS" / "report.json"
        p, _ = self.run_cli("validate", "--report", new_input_file)
        self.assertEqual(p.returncode, 64)
        self.assertFalse(new_input_file.exists())
        p, _ = self.run_cli("validate", "--report", self.root / "MAPS" / "TEST.MAP")
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual((self.root / "MAPS" / "TEST.MAP").stat().st_size, 11)

    def test_existing_report_is_preserved(self):
        target = Path(self.temp.name) / "report.json"
        target.write_bytes(b"previous report")
        p, _ = self.run_cli("validate", "--report", target)
        self.assertEqual(p.returncode, 73)
        self.assertEqual(target.read_bytes(), b"previous report")

    @unittest.skipUnless(os.name == "nt", "Проверка путей Windows")
    def test_report_is_rejected_inside_source_with_different_case(self):
        alias = self.root.with_name("GAME")
        if not alias.is_dir():
            self.skipTest("Файловая система различает регистр имён")
        target = alias / "report.json"
        p, _ = self.run_cli("validate", "--report", target)
        self.assertEqual(p.returncode, 64)
        self.assertFalse(target.exists())

    def test_concurrent_reports_publish_only_one_result(self):
        target = Path(self.temp.name) / "report.json"
        modes = ("inspect", "validate")
        with ThreadPoolExecutor(max_workers=2) as workers:
            runs = [workers.submit(self.run_cli, mode, "--report", target) for mode in modes]
        results = [run.result()[0] for run in runs]
        self.assertEqual(sorted(p.returncode for p in results), [0, 73])
        winner = modes[next(index for index, p in enumerate(results) if p.returncode == 0)]
        report = json.loads(target.read_text(encoding="utf-8"))
        self.assertEqual(report["mode"], winner)
        self.assertEqual(report["checks"], {"files": 3, "passed": 3, "failed": 0})
        self.assertEqual({p.name for p in target.parent.iterdir()}, {"game", target.name})

    @unittest.skipUnless(sys.platform.startswith("linux") and os.geteuid() != 0,
                         "Проверка прав доступа Linux без привилегий")
    def test_path_traversal_requires_no_directory_listing(self):
        parent = Path(self.temp.name) / "inputs"
        output = Path(self.temp.name) / "outputs"
        parent.mkdir()
        output.mkdir()
        self.root = self.root.rename(parent / self.root.name)
        for folder, mode in ((parent, 0o100), (output, 0o300)):
            self.addCleanup(folder.chmod, 0o700)
            folder.chmod(mode)
        target = output / "report.json"
        p, _ = self.run_cli("validate", "--report", target)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(target.read_text(encoding="utf-8"))["checks"]["passed"], 3)

    def test_report_path_and_names_support_unicode(self):
        source = self.root.with_name("Игра-地形")
        self.root.rename(source)
        self.root = source
        (self.root / "MAPS" / "TEST.MAP").rename(self.root / "MAPS" / "Карта-地形.MAP")
        target = Path(self.temp.name) / "Отчёт-地形.json"
        p, _ = self.run_cli("validate", "--report", target)
        self.assertEqual(p.returncode, 0, p.stderr)
        report = json.loads(target.read_text(encoding="utf-8"))
        self.assertEqual(report["resources"][1]["name"], "Карта-地形.MAP")
        self.assertNotIn(str(source), target.read_text(encoding="utf-8"))

    @unittest.skipUnless(os.name == "posix", "Имя из байтов без кодировки доступно в POSIX")
    def test_invalid_filename_encoding_keeps_report_valid(self):
        name = os.fsencode(self.root / "MAPS") + b"/name_\xff.MAP"
        with open(name, "wb") as fixture:
            fixture.write(bytes((1, 1, 1)) + bytes(4))
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertEqual(report["checks"], {"files": 3, "passed": 3, "failed": 1})
        self.assertTrue(any("UTF-8" in item for item in report["diagnostics"]))

    @unittest.skipUnless(os.name == "nt", "Проверка имён файлов Windows")
    def test_unpaired_surrogate_keeps_report_valid(self):
        path = self.root / "MAPS" / "name_\ud800.MAP"
        try:
            path.write_bytes(bytes((1, 1, 1)) + bytes(4))
        except OSError:
            self.skipTest("Файловая система не поддерживает такое имя")
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertEqual(report["checks"], {"files": 3, "passed": 3, "failed": 1})
        self.assertTrue(any("UTF-8" in item for item in report["diagnostics"]))

    def test_hidden_resources_are_ignored(self):
        (self.root / "MAPS" / ".hidden.MAP").write_bytes(b"hidden")
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 0)
        self.assertEqual(report["checks"]["files"], 3)

    def test_single_loft_resource_is_required(self):
        second = self.root / "GEODATA" / "loftemps.dat"
        if second.exists():
            self.skipTest("Файловая система не различает регистр имён")
        second.write_bytes(bytes(32))
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertTrue(any("LOFTEMPS" in item for item in report["diagnostics"]))

    def test_duplicate_category_is_rejected(self):
        second = self.root / "maps"
        if second.exists():
            self.skipTest("Файловая система не различает регистр имён")
        second.mkdir()
        (second / "OTHER.MAP").write_bytes(bytes((1, 1, 1)) + bytes(4))
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertTrue(any("MAPS" in item for item in report["diagnostics"]))

    def test_file_size_limit(self):
        with (self.root / "GEODATA" / "LOFTEMPS.DAT").open("r+b") as fixture:
            fixture.truncate(16 * 1024 * 1024 + 32)
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertIn("16 МиБ", report["resources"][0]["diagnosis"])

    def test_directory_limit_counts_ignored_entries(self):
        folder = self.root / "MAPS"
        for index in range(10_000):
            (folder / f"{index}.TXT").touch()
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 2)
        self.assertTrue(any("10000" in item and "MAPS" in item for item in report["diagnostics"]))

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

    @unittest.skipUnless(hasattr(os, "symlink"), "Нужны символические ссылки")
    def test_symbolic_source_parent_is_rejected(self):
        parent = Path(self.temp.name) / "inputs"
        parent.mkdir()
        self.root.rename(parent / self.root.name)
        alias = Path(self.temp.name) / "alias"
        try:
            alias.symlink_to(parent, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("ОС не разрешает символьные ссылки")
        self.root = alias / self.root.name
        p, report = self.run_cli()
        self.assertEqual(p.returncode, 64)
        self.assertIsNone(report)


if __name__ == "__main__":
    unittest.main()
