import { inflateSync } from 'node:zlib';
import { describe, expect, it } from 'vitest';
import { encodeQr, qrPng } from './qr';

const LINK = 'https://t.me/TrailCamHubBot?start=Ab3dE9fGh2Kq';

/** The 7x7 finder pattern: dark ring, light ring, dark 3x3 core. */
function hasFinder(modules: boolean[][], x0: number, y0: number): boolean {
  for (let y = 0; y < 7; y++) {
    for (let x = 0; x < 7; x++) {
      const ring = Math.max(Math.abs(x - 3), Math.abs(y - 3));
      if (modules[y0 + y]?.[x0 + x] !== (ring !== 2)) return false;
    }
  }
  return true;
}

// Decoding was checked with OpenCV's QRCodeDetector for versions 1-10 when
// the encoder was written; these tests guard the structure.
describe('QR code', () => {
  it('picks the smallest version that fits', () => {
    expect(encodeQr('hi')).toHaveLength(21); // version 1
    expect(encodeQr(LINK)).toHaveLength(33); // version 4 at level M
    expect(encodeQr('y'.repeat(200))).toHaveLength(57); // version 10
    expect(() => encodeQr('z'.repeat(300))).toThrow('too long');
  });

  it('has the finder, timing and dark modules in place', () => {
    const modules = encodeQr(LINK);
    const size = modules.length;
    expect(hasFinder(modules, 0, 0)).toBe(true);
    expect(hasFinder(modules, size - 7, 0)).toBe(true);
    expect(hasFinder(modules, 0, size - 7)).toBe(true);
    for (let i = 8; i < size - 8; i++) {
      expect(modules[6]?.[i]).toBe(i % 2 === 0);
      expect(modules[i]?.[6]).toBe(i % 2 === 0);
    }
    expect(modules[size - 8]?.[8]).toBe(true);
  });

  it('is deterministic', () => {
    expect(encodeQr(LINK)).toEqual(encodeQr(LINK));
    expect(encodeQr(LINK)).not.toEqual(encodeQr(`${LINK}x`));
  });

  it('renders a greyscale PNG with a quiet zone', () => {
    const png = qrPng('hi', { scale: 2, border: 4 });
    expect([...png.subarray(0, 8)]).toEqual([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const width = (21 + 8) * 2;
    expect(png.readUInt32BE(16)).toBe(width);
    expect(png.readUInt32BE(20)).toBe(width);
    const idatLength = png.readUInt32BE(33);
    expect(png.subarray(37, 41).toString('ascii')).toBe('IDAT');
    const pixels = inflateSync(png.subarray(41, 41 + idatLength));
    expect(pixels).toHaveLength(width * (width + 1));
    // Quiet zone light, top-left finder corner dark.
    expect(pixels[1]).toBe(0xff);
    const row = 8 * (width + 1);
    expect(pixels[row + 1 + 8]).toBe(0);
  });
});
