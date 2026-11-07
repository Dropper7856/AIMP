// Keep every Scaleform-specific visual tag intact. FFDec is used only to
// compile ActionScript; its rewritten display list must never be shipped.
const fs = require('node:fs');
const zlib = require('node:zlib');
const [originalPath, compiledPath, outputPath] = process.argv.slice(2);
if (!outputPath) throw new Error('Usage: node merge-frontend-bytecode.cjs original.GFX compiled.GFX output.GFX');

function unpack(data, preserveOpaqueTail = false) {
  const signature = data.toString('ascii', 0, 3);
  if (!['CFX', 'GFX', 'CWS', 'FWS'].includes(signature)) throw new Error('Unsupported GFX/SWF signature');
  const raw = ['CFX', 'CWS'].includes(signature) ? zlib.inflateSync(data.subarray(8)) : data.subarray(8);
  if (raw.length + 8 !== data.readUInt32LE(4)) throw new Error('Invalid uncompressed file length');
  let offset = Math.ceil((5 + 4 * (raw[0] >> 3)) / 8) + 4;
  const abc = [];
  while (offset + 2 <= raw.length) {
    const start = offset, header = raw.readUInt16LE(offset);
    offset += 2;
    const kind = header >> 6;
    let length = header & 63;
    if (length === 63) {
      if (offset + 4 > raw.length) throw new Error('Truncated tag header');
      length = raw.readUInt32LE(offset); offset += 4;
    }
    const end = offset + length;
    if (end > raw.length) {
      // The retail GFX has an opaque Scaleform tail after DoABC which Flash
      // readers cannot parse. Retain it verbatim; never let FFDec rebuild it.
      if (preserveOpaqueTail && abc.length === 1) break;
      throw new Error('Truncated tag');
    }
    if (kind === 82) abc.push({start, end, payload: raw.subarray(offset, end)});
    offset = end;
    if (kind === 0) break;
  }
  if (abc.length !== 1) throw new Error('Expected exactly one DoABC tag');
  return {data, raw, abc: abc[0], signature};
}

const original = unpack(fs.readFileSync(originalPath), true);
const compiled = unpack(fs.readFileSync(compiledPath), true);
for (const value of ['Match Type', 'Alien vs Survivors', 'SET_ALIEN_ROLE', 'alienVsSurvivors']) {
  if (!compiled.abc.payload.includes(Buffer.from(value))) throw new Error(`Compiled menu missing ${value}`);
}
const bytecode = compiled.raw.subarray(compiled.abc.start, compiled.abc.end);
const raw = Buffer.concat([original.raw.subarray(0, original.abc.start), bytecode, original.raw.subarray(original.abc.end)]);
const header = Buffer.from(original.data.subarray(0, 8));
header.writeUInt32LE(raw.length + 8, 4);
const result = Buffer.concat([header, ['CFX', 'CWS'].includes(original.signature) ? zlib.deflateSync(raw) : raw]);
const check = unpack(result, true);
if (!check.raw.subarray(0, check.abc.start).equals(original.raw.subarray(0, original.abc.start)) ||
    !check.raw.subarray(check.abc.end).equals(original.raw.subarray(original.abc.end))) {
  throw new Error('Visual tags changed');
}
if (!fs.readFileSync(originalPath).equals(result) || outputPath !== originalPath) fs.writeFileSync(outputPath, result);
console.log('Alien menu bytecode compiled; original Scaleform tags and tail preserved.');
