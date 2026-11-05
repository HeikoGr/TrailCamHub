import { deflateSync } from 'node:zlib';

/**
 * A small QR code encoder (ISO/IEC 18004, byte mode, versions 1-10) and a PNG
 * writer for it, so the bot can send pairing links as a scannable picture
 * without a third-party library or QR web service.
 *
 * Follows the structure of Project Nayuki's reference implementation (MIT).
 */

export type ErrorCorrection = 'L' | 'M' | 'Q' | 'H';

const MAX_VERSION = 10;

// Indexed by version (0 unused).
const ECC_CODEWORDS_PER_BLOCK: Record<ErrorCorrection, number[]> = {
  L: [-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18],
  M: [-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26],
  Q: [-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24],
  H: [-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28],
};
const ERROR_CORRECTION_BLOCKS: Record<ErrorCorrection, number[]> = {
  L: [-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4],
  M: [-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5],
  Q: [-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8],
  H: [-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8],
};
const FORMAT_BITS: Record<ErrorCorrection, number> = { L: 1, M: 0, Q: 3, H: 2 };

/** Modules of a version that hold data and error correction, in bits. */
function rawDataModules(version: number): number {
  let result = (16 * version + 128) * version + 64;
  if (version >= 2) {
    const align = Math.floor(version / 7) + 2;
    result -= (25 * align - 10) * align - 55;
    if (version >= 7) result -= 36;
  }
  return result;
}

function dataCodewords(version: number, ecl: ErrorCorrection): number {
  return (
    Math.floor(rawDataModules(version) / 8) -
    (ECC_CODEWORDS_PER_BLOCK[ecl][version] ?? 0) * (ERROR_CORRECTION_BLOCKS[ecl][version] ?? 0)
  );
}

// ---- Reed-Solomon over GF(2^8), polynomial 0x11D ------------------------------

function gfMultiply(x: number, y: number): number {
  let z = 0;
  for (let i = 7; i >= 0; i--) {
    z = (z << 1) ^ ((z >>> 7) * 0x11d);
    z ^= ((y >>> i) & 1) * x;
  }
  return z & 0xff;
}

function rsDivisor(degree: number): number[] {
  const result = new Array<number>(degree).fill(0);
  result[degree - 1] = 1;
  let root = 1;
  for (let i = 0; i < degree; i++) {
    for (let j = 0; j < degree; j++) {
      result[j] = gfMultiply(result[j] ?? 0, root);
      if (j + 1 < degree) result[j] = (result[j] ?? 0) ^ (result[j + 1] ?? 0);
    }
    root = gfMultiply(root, 0x02);
  }
  return result;
}

function rsRemainder(data: number[], divisor: number[]): number[] {
  const result = new Array<number>(divisor.length).fill(0);
  for (const b of data) {
    const factor = b ^ (result.shift() ?? 0);
    result.push(0);
    divisor.forEach((coef, i) => {
      result[i] = (result[i] ?? 0) ^ gfMultiply(coef, factor);
    });
  }
  return result;
}

// ---- Encoding ------------------------------------------------------------------

/** Data codewords plus interleaved error correction, as the symbol stores them. */
function codewords(bytes: Uint8Array, version: number, ecl: ErrorCorrection): number[] {
  const capacityBits = dataCodewords(version, ecl) * 8;
  const bits: number[] = [];
  const append = (value: number, length: number) => {
    for (let i = length - 1; i >= 0; i--) bits.push((value >>> i) & 1);
  };
  append(0b0100, 4); // byte mode
  append(bytes.length, version <= 9 ? 8 : 16);
  for (const b of bytes) append(b, 8);
  append(0, Math.min(4, capacityBits - bits.length)); // terminator
  append(0, (8 - (bits.length % 8)) % 8);
  for (let pad = 0xec; bits.length < capacityBits; pad ^= 0xec ^ 0x11) append(pad, 8);

  const data: number[] = [];
  for (let i = 0; i < bits.length; i += 8) {
    data.push(bits.slice(i, i + 8).reduce((acc, bit) => (acc << 1) | bit, 0));
  }

  const blocks = ERROR_CORRECTION_BLOCKS[ecl][version] ?? 1;
  const eccLength = ECC_CODEWORDS_PER_BLOCK[ecl][version] ?? 0;
  const raw = Math.floor(rawDataModules(version) / 8);
  const shortBlocks = blocks - (raw % blocks);
  const shortLength = Math.floor(raw / blocks);
  const divisor = rsDivisor(eccLength);
  const split: number[][] = [];
  for (let i = 0, k = 0; i < blocks; i++) {
    const length = shortLength - eccLength + (i < shortBlocks ? 0 : 1);
    const block = data.slice(k, k + length);
    k += length;
    const ecc = rsRemainder(block, divisor);
    if (i < shortBlocks) block.push(0); // placeholder, skipped below
    split.push([...block, ...ecc]);
  }
  const result: number[] = [];
  for (let i = 0; i < (split[0]?.length ?? 0); i++) {
    split.forEach((block, j) => {
      if (i !== shortLength - eccLength || j >= shortBlocks) result.push(block[i] ?? 0);
    });
  }
  return result;
}

function alignmentPositions(version: number): number[] {
  if (version === 1) return [];
  const count = Math.floor(version / 7) + 2;
  const step = Math.ceil((version * 4 + 4) / (count * 2 - 2)) * 2;
  const result = [6];
  for (let pos = version * 4 + 17 - 7; result.length < count; pos -= step) {
    result.splice(1, 0, pos);
  }
  return result;
}

const MASKS: Array<(x: number, y: number) => boolean> = [
  (x, y) => (x + y) % 2 === 0,
  (_x, y) => y % 2 === 0,
  (x) => x % 3 === 0,
  (x, y) => (x + y) % 3 === 0,
  (x, y) => (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0,
  (x, y) => ((x * y) % 2) + ((x * y) % 3) === 0,
  (x, y) => (((x * y) % 2) + ((x * y) % 3)) % 2 === 0,
  (x, y) => (((x + y) % 2) + ((x * y) % 3)) % 2 === 0,
];

class QrSymbol {
  readonly size: number;
  readonly modules: boolean[][];
  private readonly isFunction: boolean[][];

  constructor(
    private readonly version: number,
    private readonly ecl: ErrorCorrection,
  ) {
    this.size = version * 4 + 17;
    this.modules = Array.from({ length: this.size }, () =>
      new Array<boolean>(this.size).fill(false),
    );
    this.isFunction = Array.from({ length: this.size }, () =>
      new Array<boolean>(this.size).fill(false),
    );
    this.drawFunctionPatterns();
  }

  private setFunction(x: number, y: number, dark: boolean) {
    (this.modules[y] as boolean[])[x] = dark;
    (this.isFunction[y] as boolean[])[x] = true;
  }

  private drawFunctionPatterns() {
    const size = this.size;
    for (let i = 0; i < size; i++) {
      this.setFunction(6, i, i % 2 === 0);
      this.setFunction(i, 6, i % 2 === 0);
    }
    for (const [cx, cy] of [
      [3, 3],
      [size - 4, 3],
      [3, size - 4],
    ] as const) {
      for (let dy = -4; dy <= 4; dy++) {
        for (let dx = -4; dx <= 4; dx++) {
          const x = cx + dx;
          const y = cy + dy;
          const dist = Math.max(Math.abs(dx), Math.abs(dy));
          if (x >= 0 && x < size && y >= 0 && y < size)
            this.setFunction(x, y, dist !== 2 && dist !== 4);
        }
      }
    }
    const align = alignmentPositions(this.version);
    const last = align.length - 1;
    align.forEach((ax, i) => {
      align.forEach((ay, j) => {
        if ((i === 0 && j === 0) || (i === 0 && j === last) || (i === last && j === 0)) return;
        for (let dy = -2; dy <= 2; dy++) {
          for (let dx = -2; dx <= 2; dx++) {
            this.setFunction(ax + dx, ay + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
          }
        }
      });
    });
    this.drawFormatBits(0); // reserves the area; redrawn with the real mask
    if (this.version >= 7) {
      let rem = this.version;
      for (let i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >>> 11) * 0x1f25);
      const bits = (this.version << 12) | rem;
      for (let i = 0; i < 18; i++) {
        const dark = ((bits >>> i) & 1) !== 0;
        const a = size - 11 + (i % 3);
        const b = Math.floor(i / 3);
        this.setFunction(a, b, dark);
        this.setFunction(b, a, dark);
      }
    }
  }

  drawFormatBits(mask: number) {
    const size = this.size;
    const data = (FORMAT_BITS[this.ecl] << 3) | mask;
    let rem = data;
    for (let i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
    const bits = ((data << 10) | rem) ^ 0x5412;
    const bit = (i: number) => ((bits >>> i) & 1) !== 0;
    for (let i = 0; i <= 5; i++) this.setFunction(8, i, bit(i));
    this.setFunction(8, 7, bit(6));
    this.setFunction(8, 8, bit(7));
    this.setFunction(7, 8, bit(8));
    for (let i = 9; i < 15; i++) this.setFunction(14 - i, 8, bit(i));
    for (let i = 0; i < 8; i++) this.setFunction(size - 1 - i, 8, bit(i));
    for (let i = 8; i < 15; i++) this.setFunction(8, size - 15 + i, bit(i));
    this.setFunction(8, size - 8, true);
  }

  drawCodewords(data: number[]) {
    const size = this.size;
    let i = 0;
    for (let right = size - 1; right >= 1; right -= 2) {
      if (right === 6) right = 5;
      for (let vert = 0; vert < size; vert++) {
        for (let j = 0; j < 2; j++) {
          const x = right - j;
          const upward = ((right + 1) & 2) === 0;
          const y = upward ? size - 1 - vert : vert;
          if (!this.isFunction[y]?.[x] && i < data.length * 8) {
            (this.modules[y] as boolean[])[x] =
              (((data[i >>> 3] ?? 0) >>> (7 - (i & 7))) & 1) !== 0;
            i++;
          }
        }
      }
    }
  }

  applyMask(mask: number) {
    const test = MASKS[mask] as (x: number, y: number) => boolean;
    for (let y = 0; y < this.size; y++) {
      for (let x = 0; x < this.size; x++) {
        if (!this.isFunction[y]?.[x] && test(x, y)) {
          (this.modules[y] as boolean[])[x] = !this.modules[y]?.[x];
        }
      }
    }
  }

  /** Penalty score of the current pattern (lower scans better). */
  penalty(): number {
    const size = this.size;
    const at = (x: number, y: number) => this.modules[y]?.[x] === true;
    let result = 0;
    const lines: boolean[][] = [];
    for (let i = 0; i < size; i++) {
      lines.push(Array.from({ length: size }, (_, k) => at(k, i)));
      lines.push(Array.from({ length: size }, (_, k) => at(i, k)));
    }
    const finderLike = [true, false, true, true, true, false, true];
    for (const line of lines) {
      // Rule 1: runs of five or more modules of one colour.
      let run = 1;
      for (let k = 1; k <= size; k++) {
        if (k < size && line[k] === line[k - 1]) {
          run++;
        } else {
          if (run >= 5) result += run - 2;
          run = 1;
        }
      }
      // Rule 3: finder-like patterns with four light modules on one side.
      const padded = [false, false, false, false, ...line, false, false, false, false];
      for (let k = 0; k + 11 <= padded.length; k++) {
        const core = finderLike.every((v, n) => padded[k + 4 + n] === v);
        if (!core) continue;
        const before = [0, 1, 2, 3].every((n) => padded[k + n] === false);
        const after = [0, 1, 2, 3].every((n) => padded[k + 11 + n] === false);
        if (before || after) result += 40;
      }
    }
    // Rule 2: 2x2 blocks of one colour.
    for (let y = 0; y < size - 1; y++) {
      for (let x = 0; x < size - 1; x++) {
        const c = at(x, y);
        if (c === at(x + 1, y) && c === at(x, y + 1) && c === at(x + 1, y + 1)) result += 3;
      }
    }
    // Rule 4: balance of dark and light modules.
    let dark = 0;
    for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) if (at(x, y)) dark++;
    const total = size * size;
    result += (Math.ceil(Math.abs(dark * 20 - total * 10) / total) - 1) * 10;
    return result;
  }
}

/**
 * Encodes `text` (UTF-8, byte mode) in the smallest version that fits, with
 * the mask of lowest penalty. Returns rows of modules, `true` = dark.
 */
export function encodeQr(text: string, ecl: ErrorCorrection = 'M'): boolean[][] {
  const bytes = new TextEncoder().encode(text);
  let version = 1;
  for (; version <= MAX_VERSION; version++) {
    const headerBits = 4 + (version <= 9 ? 8 : 16);
    if (headerBits + bytes.length * 8 <= dataCodewords(version, ecl) * 8) break;
  }
  if (version > MAX_VERSION) throw new Error(`Text too long for a QR code (${bytes.length} bytes)`);

  const data = codewords(bytes, version, ecl);
  let best: { mask: number; penalty: number } | undefined;
  for (let mask = 0; mask < 8; mask++) {
    const symbol = new QrSymbol(version, ecl);
    symbol.drawCodewords(data);
    symbol.applyMask(mask);
    symbol.drawFormatBits(mask);
    const penalty = symbol.penalty();
    if (!best || penalty < best.penalty) best = { mask, penalty };
  }
  const symbol = new QrSymbol(version, ecl);
  symbol.drawCodewords(data);
  symbol.applyMask(best?.mask ?? 0);
  symbol.drawFormatBits(best?.mask ?? 0);
  return symbol.modules;
}

// ---- PNG -----------------------------------------------------------------------

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

function crc32(data: Buffer): number {
  let c = 0xffffffff;
  for (const b of data) c = (CRC_TABLE[(c ^ b) & 0xff] ?? 0) ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Buffer): Buffer {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([length, body, crc]);
}

/**
 * Renders a QR code as an 8-bit greyscale PNG with the standard quiet zone of
 * four modules.
 */
export function qrPng(text: string, { scale = 10, border = 4 } = {}): Buffer {
  const modules = encodeQr(text);
  const width = (modules.length + border * 2) * scale;
  const rows: Buffer[] = [];
  for (let py = 0; py < width; py++) {
    const row = Buffer.alloc(width + 1, 0xff);
    row[0] = 0; // filter: none
    const my = Math.floor(py / scale) - border;
    for (let px = 0; px < width; px++) {
      const mx = Math.floor(px / scale) - border;
      if (modules[my]?.[mx]) row[px + 1] = 0;
    }
    rows.push(row);
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(width, 4);
  header[8] = 8; // bit depth
  header[9] = 0; // greyscale
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', header),
    chunk('IDAT', deflateSync(Buffer.concat(rows))),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}
