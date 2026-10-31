import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';

/**
 * Hub keys have the form `<hubId>.<secret>`. The id tells the backend which
 * record to check; only the SHA-256 of the secret is stored.
 */
export function parseDeviceKey(key: string): { id: string; secret: string } | undefined {
  const dot = key.indexOf('.');
  if (dot <= 0 || dot === key.length - 1) return undefined;
  const id = key.slice(0, dot);
  const secret = key.slice(dot + 1);
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(id) || !/^[A-Za-z0-9_-]{16,128}$/.test(secret)) {
    return undefined;
  }
  return { id, secret };
}

export function hashSecret(secret: string): string {
  return createHash('sha256').update(secret).digest('hex');
}

export function verifySecret(secret: string, keyHash: string): boolean {
  const actual = Buffer.from(hashSecret(secret), 'hex');
  const expected = Buffer.from(keyHash, 'hex');
  return actual.length === expected.length && timingSafeEqual(actual, expected);
}

/** New random key for a hub: 32 bytes, base64url. */
export function generateDeviceKey(hubId: string): { key: string; keyHash: string } {
  const secret = randomBytes(32).toString('base64url');
  return { key: `${hubId}.${secret}`, keyHash: hashSecret(secret) };
}
