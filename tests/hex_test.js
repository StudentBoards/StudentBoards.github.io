// Intel HEX parser, extracted for testing.
function parseIntelHex(text, maxSize) {
  const out = new Uint8Array(maxSize).fill(0xFF);
  let high = 0, top = 0, seen = false, records = 0;
  const lines = text.split(/\r?\n/);

  for (let n = 0; n < lines.length; n++) {
    const line = lines[n].trim();
    if (!line) continue;
    if (line[0] !== ':') throw new Error(`line ${n + 1}: missing ':'`);
    if (line.length < 11) throw new Error(`line ${n + 1}: record too short`);

    const b = [];
    for (let i = 1; i + 1 < line.length; i += 2) {
      const v = parseInt(line.substr(i, 2), 16);
      if (Number.isNaN(v)) throw new Error(`line ${n + 1}: bad hex`);
      b.push(v);
    }
    const len = b[0], addr = (b[1] << 8) | b[2], type = b[3];
    if (b.length !== len + 5) throw new Error(`line ${n + 1}: length mismatch`);

    // Checksum is two's complement of the sum of all preceding bytes.
    let sum = 0;
    for (let i = 0; i < b.length - 1; i++) sum += b[i];
    if (((~sum + 1) & 0xFF) !== b[b.length - 1]) {
      throw new Error(`line ${n + 1}: checksum error`);
    }
    records++;

    if (type === 0x00) {
      const base = high + addr;
      for (let i = 0; i < len; i++) {
        const a = base + i;
        if (a >= maxSize) throw new Error(`address 0x${a.toString(16)} past end of flash`);
        out[a] = b[4 + i];
        if (a + 1 > top) top = a + 1;
        seen = true;
      }
    } else if (type === 0x01) {
      break;                                  // end of file
    } else if (type === 0x04) {
      high = ((b[4] << 8) | b[5]) << 16;      // extended linear address
    } else if (type === 0x02) {
      high = ((b[4] << 8) | b[5]) << 4;       // extended segment address
    }
    // Types 03/05 are entry-point records; harmless to skip.
  }
  if (!seen) throw new Error('no data records found');
  return { data: out.subarray(0, top), length: top, records };
}

// ---- tests ----
function mkrec(type, addr, bytes) {
  const b = [bytes.length, (addr >> 8) & 0xFF, addr & 0xFF, type, ...bytes];
  let s = 0; for (const x of b) s += x;
  b.push((~s + 1) & 0xFF);
  return ':' + b.map(x => x.toString(16).toUpperCase().padStart(2, '0')).join('');
}

let pass = 0, fail = 0;
function t(name, fn, expectErr) {
  try { fn(); if (expectErr) { console.log(`FAIL ${name} (expected error)`); fail++; } else { console.log(`PASS ${name}`); pass++; } }
  catch (e) { if (expectErr) { console.log(`PASS ${name} -> ${e.message}`); pass++; } else { console.log(`FAIL ${name}: ${e.message}`); fail++; } }
}

t('simple two records', () => {
  const hex = [mkrec(0, 0, [0x0C, 0x94, 0x2A, 0x00]), mkrec(0, 4, [0xFF, 0xFF]), ':00000001FF'].join('\n');
  const r = parseIntelHex(hex, 32768);
  if (r.length !== 6) throw new Error('length ' + r.length);
  if (r.data[0] !== 0x0C || r.data[2] !== 0x2A) throw new Error('data wrong');
});
t('sparse addresses padded with 0xFF', () => {
  const hex = [mkrec(0, 0, [0xAA]), mkrec(0, 100, [0xBB]), ':00000001FF'].join('\n');
  const r = parseIntelHex(hex, 32768);
  if (r.data[0] !== 0xAA || r.data[100] !== 0xBB || r.data[50] !== 0xFF) throw new Error('padding wrong');
  if (r.length !== 101) throw new Error('length ' + r.length);
});
t('CRLF line endings', () => {
  const hex = mkrec(0, 0, [1,2,3,4]) + '\r\n' + ':00000001FF\r\n';
  if (parseIntelHex(hex, 32768).length !== 4) throw new Error('bad');
});
t('extended linear address record', () => {
  const hex = [':020000040000FA', mkrec(0, 0x10, [0x42]), ':00000001FF'].join('\n');
  if (parseIntelHex(hex, 32768).data[0x10] !== 0x42) throw new Error('bad');
});
t('bad checksum rejected', () => parseIntelHex(':0400000000112233FF\n', 32768), true);
t('missing colon rejected', () => parseIntelHex('0400000000112233\n', 32768), true);
t('non-hex rejected', () => parseIntelHex(':04000000ZZ112233\n', 32768), true);
t('address past flash rejected', () => parseIntelHex(mkrec(0, 0x100, [1,2,3,4]), 64), true);
t('empty file rejected', () => parseIntelHex('\n\n', 32768), true);
t('image larger than flash rejected', () => {
  const recs = [];
  for (let a = 0; a < 40000; a += 16) recs.push(mkrec(0, a & 0xFFFF, new Array(16).fill(0xAA)));
  parseIntelHex(recs.join('\n'), 32768);
}, true);

console.log(`\n${pass} passed, ${fail} failed`);
