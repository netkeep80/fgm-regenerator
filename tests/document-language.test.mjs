import test from "node:test";
import assert from "node:assert/strict";
import { audit } from "../scripts/check-document-language.mjs";

test("русская документация и технические идентификаторы допускаются", () => {
  const files = new Map([
    ["README.md", "# Назначение FGM\n\nГеометрия строится по правилам FGM и WebGPU.\n\n- [fgm-core](https://github.com/netkeep80/fgm-core)\n"],
    ["docs/архитектура.md", "## Цели\n\nРусский текст с упоминанием GPU.\n"],
    ["requirements/TEST-001.rg.yaml", 'format: repo-guard/requirement-v1\nid: TEST-001\nnormative: "Обязательное требование на русском языке"\n'],
  ]);
  assert.deepEqual(audit(files), []);
});

test("английский заголовок должен вызывать отказ", () => {
  assert.ok(audit(new Map([["README.md", "# Installation\n\nИнструкция по запуску.\n"]])).length > 0);
});

test("английский абзац должен вызывать отказ", () => {
  assert.ok(audit(new Map([["docs/guide.md", "# Руководство\n\nThis is a full English sentence.\n"]])).length > 0);
});

test("английское нормативное требование должно вызывать отказ", () => {
  assert.ok(audit(new Map([["requirements/TEST-001.rg.yaml", 'format: repo-guard/requirement-v1\nid: TEST-001\nnormative: "Documentation must be in Russian"\n']])).length > 0);
});

test("технический блок кода не считается переводимым текстом", () => {
  const tick = String.fromCharCode(96).repeat(3);
  assert.deepEqual(audit(new Map([["README.md", "# Пример\n\n" + tick + "bash\nnpm run build\n" + tick + "\n"]])), []);
});
