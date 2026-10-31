import type { APIGatewayProxyEventV2 } from 'aws-lambda';
import { describe, expect, it } from 'vitest';
import { botToken, toApiRequest } from './handler';

function event(overrides: Partial<APIGatewayProxyEventV2>): APIGatewayProxyEventV2 {
  return {
    version: '2.0',
    routeKey: '$default',
    rawPath: '/v1/cameras',
    rawQueryString: '',
    headers: {},
    isBase64Encoded: false,
    requestContext: { http: { method: 'get' } },
    ...overrides,
  } as APIGatewayProxyEventV2;
}

describe('toApiRequest', () => {
  it('normalizes method and header names', () => {
    const req = toApiRequest(event({ headers: { Authorization: 'tma abc' } }));
    expect(req).toEqual({
      method: 'GET',
      path: '/v1/cameras',
      headers: { authorization: 'tma abc' },
      body: undefined,
    });
  });

  it('decodes base64 bodies', () => {
    const req = toApiRequest(
      event({ body: Buffer.from('{"a":1}').toString('base64'), isBase64Encoded: true }),
    );
    expect(req.body).toBe('{"a":1}');
  });
});

describe('botToken', () => {
  it('prefers the token from the environment', async () => {
    await expect(botToken({ TELEGRAM_BOT_TOKEN: ' 123:abc ' })).resolves.toBe('123:abc');
  });

  it('fails clearly when no token source is configured', async () => {
    await expect(botToken({ TELEGRAM_BOT_TOKEN: '' })).rejects.toThrow('No Telegram bot token');
  });
});
