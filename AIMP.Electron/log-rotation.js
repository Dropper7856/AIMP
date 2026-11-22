'use strict';

// Size-based rotation for the Electron launcher's append-only logs.
//
// At startup, a log that has grown past ROTATE_BYTES is renamed to
// <name>.<UTC stamp><ext> (synchronous, so this session starts a fresh file),
// then gzipped in the background. The original is removed only after the
// archive decompresses back to identical bytes. Only the newest MAX_ARCHIVES
// archives per log are kept.

const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const zlib = require('node:zlib');
const { promisify } = require('node:util');

const MAX_ARCHIVES = 10;
const ROTATE_BYTES = 1024 * 1024;
const gzip = promisify(zlib.gzip);
const gunzip = promisify(zlib.gunzip);

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function describe(logPath) {
  const ext = path.extname(logPath);
  const base = path.basename(logPath, ext);
  const pattern = new RegExp(`^${escapeRegExp(base)}\\.\\d{8}T\\d{6}Z(?:-\\d+)?${escapeRegExp(ext)}(\\.gz)?$`, 'i');
  return { dir: path.dirname(logPath), ext, base, pattern };
}

function stamp(date) {
  return date.toISOString().replace(/[-:]/g, '').replace(/\.\d{3}/, '');
}

// Renames logPath out of the way when it is at least maxBytes. Returns the
// rotated path, or null when nothing was rotated.
function rollLog(logPath, { maxBytes = ROTATE_BYTES, now = new Date() } = {}) {
  let size;
  try {
    size = fs.statSync(logPath).size;
  } catch {
    return null;
  }
  if (size < maxBytes) return null;
  const { dir, ext, base } = describe(logPath);
  let target = path.join(dir, `${base}.${stamp(now)}${ext}`);
  for (let index = 2; fs.existsSync(target) || fs.existsSync(`${target}.gz`); index += 1) {
    target = path.join(dir, `${base}.${stamp(now)}-${index}${ext}`);
  }
  fs.renameSync(logPath, target);
  return target;
}

async function compressFile(file) {
  const data = await fsp.readFile(file);
  const packed = await gzip(data, { level: zlib.constants.Z_BEST_COMPRESSION });
  const roundTrip = await gunzip(packed);
  if (!roundTrip.equals(data)) throw new Error(`Compressed copy of ${file} did not match; original kept.`);
  const { atime, mtime } = await fsp.stat(file);
  const temporary = `${file}.gz.${process.pid}.tmp`;
  try {
    await fsp.writeFile(temporary, packed, { flag: 'w' });
    await fsp.utimes(temporary, atime, mtime);
    await fsp.rename(temporary, `${file}.gz`);
  } finally {
    await fsp.rm(temporary, { force: true });
  }
  await fsp.unlink(file);
}

async function rotatedEntries(logPath) {
  const { dir, pattern } = describe(logPath);
  const names = (await fsp.readdir(dir)).filter((name) => pattern.test(name));
  const entries = await Promise.all(names.map(async (name) => {
    const file = path.join(dir, name);
    return { file, name, mtimeMs: (await fsp.stat(file)).mtimeMs, compressed: name.toLowerCase().endsWith('.gz') };
  }));
  return entries.sort((a, b) => (b.mtimeMs - a.mtimeMs) || b.name.localeCompare(a.name));
}

// Compresses every rotated-but-uncompressed copy (including leftovers from an
// interrupted earlier run) and deletes archives beyond maxArchives.
async function compressAndPrune(logPath, { maxArchives = MAX_ARCHIVES } = {}) {
  for (const entry of await rotatedEntries(logPath)) {
    if (!entry.compressed) await compressFile(entry.file);
  }
  const archives = (await rotatedEntries(logPath)).filter((entry) => entry.compressed);
  for (const entry of archives.slice(maxArchives)) {
    await fsp.unlink(entry.file);
  }
}

// Rolls each log synchronously, then compresses/prunes in the background.
// The returned promise never rejects; failures are reported through log().
function rotateLogs(logPaths, { log = () => {}, maxBytes, maxArchives, now } = {}) {
  for (const logPath of logPaths) {
    try {
      const rotated = rollLog(logPath, { maxBytes, now });
      if (rotated) log(`Rotated log ${logPath} -> ${path.basename(rotated)}`);
    } catch (error) {
      log(`Unable to rotate log ${logPath}: ${error.message}`);
    }
  }
  return Promise.all(logPaths.map((logPath) => compressAndPrune(logPath, { maxArchives })
    .catch((error) => log(`Unable to compress logs for ${logPath}: ${error.message}`))));
}

module.exports = { MAX_ARCHIVES, ROTATE_BYTES, rollLog, compressAndPrune, rotateLogs };
