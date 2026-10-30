import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';

/**
 * Device keys have the form `<camId>.<secret>`. The camera id tells the
 * backend which record to check; only the SHA-256 of the secret is stored.
 */
export function parseDeviceKey(key: string): { camId: string; secret: string } | undefined {
  const dot = key.indexOf('.');
  if (dot <= 0 || dot === key.length - 1) return undefined;
  const camId = key.slice(0, dot);
  const secret = key.slice(dot + 1);
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(camId) || !/^[A-Za-z0-9_-]{16,128}$/.test(secret)) {
    return undefined;
  }
  return { camId, secret };
}

export function hashSecret(secret: string): string {
  return createHash('sha256').update(secret).digest('hex');
}

export function verifySecret(secret: string, keyHash: string): boolean {
  const actual = Buffer.from(hashSecret(secret), 'hex');
  const expected = Buffer.from(keyHash, 'hex');
  return actual.length === expected.length && timingSafeEqual(actual, expected);
}

/** New random key for a camera: 32 bytes, base64url. */
export function generateDeviceKey(camId: string): { key: string; keyHash: string } {
  const secret = randomBytes(32).toString('base64url');
  return { key: `${camId}.${secret}`, keyHash: hashSecret(secret) };
}
