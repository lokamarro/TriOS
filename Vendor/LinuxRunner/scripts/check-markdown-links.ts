#!/usr/bin/env bun

import { existsSync, lstatSync, readFileSync, readdirSync } from "node:fs";
import { dirname, extname, join, normalize, relative, resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const starts = process.argv.slice(2);
const roots = starts.length > 0 ? starts : ["README.md", "docs"];

function walk(path: string): string[] {
  const full = resolve(root, path);
  if (!existsSync(full)) return [full];
  if (!lstatSync(full).isDirectory()) return extname(full) === ".md" ? [full] : [];
  return readdirSync(full, { withFileTypes: true }).flatMap((entry) =>
    entry.name.startsWith(".")
      ? []
      : walk(relative(root, join(full, entry.name))),
  );
}

function decodeTarget(target: string): string {
  try {
    return decodeURIComponent(target);
  } catch {
    return target;
  }
}

const files = roots.flatMap(walk);
const failures: string[] = [];
const linkPattern = /!?(?:\[[^\]]*\])\(([^)]+)\)/g;

for (const file of files) {
  const lines = readFileSync(file, "utf8").split("\n");
  lines.forEach((line, index) => {
    for (const match of line.matchAll(linkPattern)) {
      let target = match[1].trim();
      if (target.startsWith("<") && target.endsWith(">")) target = target.slice(1, -1);
      if (/^(?:[a-z][a-z0-9+.-]*:|#)/i.test(target)) continue;
      target = decodeTarget(target.split("#", 1)[0]);
      if (target.length === 0) continue;
      const destination = normalize(resolve(dirname(file), target));
      if (!destination.startsWith(root + "/") && destination !== root) {
        failures.push(`${relative(root, file)}:${index + 1}: target leaves repository: ${target}`);
      } else if (!existsSync(destination)) {
        failures.push(`${relative(root, file)}:${index + 1}: missing target: ${target}`);
      }
    }
  });
}

if (failures.length > 0) {
  console.error(failures.join("\n"));
  process.exit(1);
}

console.log(`Checked ${files.length} Markdown files; all local targets exist.`);
