import { describe, expect, it } from 'vitest';
import { matchPath } from './http';

describe('matchPath', () => {
  it('extracts parameters', () => {
    expect(matchPath('/v1/cameras/:camId/media', '/v1/cameras/ABC/media')).toEqual({
      camId: 'ABC',
    });
  });

  it('ignores a trailing slash', () => {
    expect(matchPath('/v1/cameras', '/v1/cameras/')).toEqual({});
  });

  it('rejects different paths and empty parameters', () => {
    expect(matchPath('/v1/cameras', '/v1/camera')).toBeUndefined();
    expect(matchPath('/v1/cameras/:camId/media', '/v1/cameras//media')).toBeUndefined();
    expect(matchPath('/v1/cameras/:camId/media', '/v1/cameras/ABC')).toBeUndefined();
  });
});
