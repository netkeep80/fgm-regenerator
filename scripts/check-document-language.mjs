#!/usr/bin/env node
// Проверка языка документации: без внешних зависимостей.
// Формулировки на русском, технические имена и машинные фрагменты допускаются.
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const CYR = /[А-Яа-яЁё]/;
const TICK = String.fromCharCode(96);
const TERMS = new Set([
  "fgm", "fgm-core", "fgm-regenerator", "fgm-renderer", "ufo-fractal",
  "ufo", "tftd", "openxcom", "isocubic", "fractalgalaxy", "nnets",
  "webgpu", "webgl", "wgsl", "wasm", "webassembly", "typescript", "javascript",
  "pytorch", "python", "sdl", "github", "ci", "cpu", "gpu", "api", "cli",
  "readme", "license", "unlicense", "gpl", "spdx", "sha", "json", "yaml",
  "node", "npm", "mcd", "map", "pck", "tab", "loft", "loftemps", "sdf",
  "lod", "cpp", "c", "c++", "id", "uuid", "url", "https", "http", "md",
]);
const excluded = /^(vendor|third_party|third-party|node_modules|build|dist|\.tools)\//;

function plainText(line) {
  let out = line.replace(/https?:\/\/[^\s)>]+/g, " ");
  out = out.replace(new RegExp(TICK + "[^" + TICK + "]*" + TICK, "g"), " ");
  out = out.replace(/!\[[^\]]*\]\([^)]*\)/g, " ");
  out = out.replace(/\]\([^)]*\)/g, "]");
  out = out.replace(/<!--.*?-->/g, " ");
  return out.replace(/[#>*_|~![\](){}]/g, " ").trim();
}

function englishProse(line) {
  const text = plainText(line);
  if (!text || CYR.test(text)) return false;
  const tokens = text.match(/[A-Za-z][A-Za-z0-9+_.-]*/g) || [];
  return tokens.some(token => {
    const word = token.toLowerCase();
    return !TERMS.has(word) && !/^[A-Z0-9_.+-]{2,}$/.test(token)
      && !/^(?:fgm|ufof|fgmc|fgmr|fgmv|rg)-[a-z0-9-]+$/i.test(token);
  });
}

function inspectMarkdown(path, source) {
  const errors = [];
  const lines = source.split(/\r?\n/);
  let fence = null;
  let htmlComment = false;
  lines.forEach((line, index) => {
    const trim = line.trim();
    if (trim.startsWith("<!--")) htmlComment = true;
    if (htmlComment) {
      if (trim.includes("-->")) htmlComment = false;
      return;
    }
    const marker = trim.startsWith(TICK.repeat(3)) ? TICK : trim.startsWith("~~~") ? "~" : null;
    if (marker) {
      if (fence === null) fence = marker;
      else if (fence === marker) fence = null;
      return;
    }
    if (fence || /^\s{4,}\S/.test(line) || /^\s*\|?\s*:?-{3,}:?\s*\|/.test(line)) return;
    if (englishProse(line)) errors.push(path + ":" + (index + 1) + ": англоязычный текст документации");
  });
  if (fence) errors.push(path + ": незакрытый блок кода");
  return errors;
}

function readNormative(path, source) {
  if (path.endsWith(".rg.json")) {
    const obj = JSON.parse(source);
    return typeof obj.normative === "string" ? obj.normative : null;
  }
  const lines = source.split(/\r?\n/);
  const at = lines.findIndex(line => /^normative:\s*/.test(line));
  if (at < 0) return null;
  const raw = lines[at].replace(/^normative:\s*/, "").trim();
  if (raw.startsWith('"')) return JSON.parse(raw);
  if (raw.startsWith("'") && raw.endsWith("'")) return raw.slice(1, -1).replace(/''/g, "'");
  if (raw === "|" || raw === ">" || raw.startsWith("|-") || raw.startsWith(">-")) {
    const linesOut = [];
    for (let i = at + 1; i < lines.length; ++i) {
      if (lines[i].trim() && !/^\s/.test(lines[i])) break;
      linesOut.push(lines[i].replace(/^\s{2}/, ""));
    }
    return linesOut.join("\n");
  }
  return null;
}

export function audit(files) {
  const errors = [];
  for (const [path, source] of files) {
    if (excluded.test(path)) continue;
    if (/\.md$/i.test(path)) errors.push(...inspectMarkdown(path, source));
    if (/^requirements\/.*\.rg\.(yaml|json)$/.test(path)) {
      try {
        const normative = readNormative(path, source);
        if (typeof normative !== "string" || !CYR.test(normative))
          errors.push(path + ": нормативный текст отсутствует или не содержит русского языка");
      } catch (error) {
        errors.push(path + ": невозможно прочитать normative: " + String(error.message));
      }
    }
  }
  return errors;
}

export function auditTracked(root) {
  const names = execFileSync("git", ["-C", root, "ls-files", "-z"], {encoding:"utf8"}).split("\0").filter(Boolean);
  const files = new Map();
  for (const name of names) {
    if (excluded.test(name)) continue;
    if (!/\.md$/i.test(name) && !/^requirements\/.*\.rg\.(yaml|json)$/.test(name)) continue;
    files.set(name, readFileSync(resolve(root, name), "utf8"));
  }
  return {count:files.size, errors:audit(files)};
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const root = resolve(process.argv[2] || ".");
  const result = auditTracked(root);
  for (const error of result.errors) console.error("ОШИБКА: " + error);
  console.log("Проверка языка документации: " + result.count + " файлов; нарушений: " + result.errors.length);
  process.exitCode = result.errors.length ? 1 : 0;
}
