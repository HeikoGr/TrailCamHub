#!/usr/bin/env node
// Registers a hub, links Telegram users to it, and prints its key.
//
//   npm run register-hub -- --table <TableName> --hub garden-hub \
//     [--name "Garden"] [--user <telegramUserId>]... [--rotate-key] [--region eu-central-1]
//
// - A new hub gets a key, printed once.
// - For an existing hub, --user links more people (family members) without
//   touching the key; --rotate-key issues a new key (the old one stops working).
// - Cameras are not registered by hand: they appear automatically when the
//   hub reports them, and every user linked to the hub sees them.
//
// Uses the standard AWS credential chain.

import { createHash, randomBytes } from 'node:crypto';
import { parseArgs } from 'node:util';
import { DynamoDBClient } from '@aws-sdk/client-dynamodb';
import { DynamoDBDocumentClient, GetCommand, PutCommand } from '@aws-sdk/lib-dynamodb';

const { values } = parseArgs({
  options: {
    table: { type: 'string' },
    hub: { type: 'string' },
    name: { type: 'string' },
    user: { type: 'string', multiple: true, default: [] },
    'rotate-key': { type: 'boolean', default: false },
    region: { type: 'string', default: process.env.AWS_REGION ?? 'eu-central-1' },
  },
});

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

if (!values.table) fail('--table is required (stack output TableName)');
if (!values.hub || !/^[A-Za-z0-9_-]{1,64}$/.test(values.hub)) {
  fail('--hub must be 1-64 characters [A-Za-z0-9_-]');
}
for (const user of values.user) {
  if (!/^\d+$/.test(user)) fail(`--user must be a numeric Telegram user id (got ${user})`);
}

const db = DynamoDBDocumentClient.from(new DynamoDBClient({ region: values.region }), {
  marshallOptions: { removeUndefinedValues: true },
});

const pk = `HUB#${values.hub}`;
const { Item: existing } = await db.send(
  new GetCommand({ TableName: values.table, Key: { PK: pk, SK: 'META' } }),
);

let key;
if (!existing || values['rotate-key'] || values.name) {
  let keyHash = existing?.keyHash;
  if (!existing || values['rotate-key']) {
    const secret = randomBytes(32).toString('base64url');
    keyHash = createHash('sha256').update(secret).digest('hex');
    key = `${values.hub}.${secret}`;
  }
  await db.send(
    new PutCommand({
      TableName: values.table,
      Item: {
        ...existing,
        PK: pk,
        SK: 'META',
        hubId: values.hub,
        name: values.name ?? existing?.name ?? values.hub,
        keyHash,
        lastSeen: existing?.lastSeen ?? 0,
      },
    }),
  );
}

for (const user of values.user) {
  await db.send(
    new PutCommand({
      TableName: values.table,
      Item: { PK: `USER#${user}`, SK: pk, hubId: values.hub },
    }),
  );
  console.log(`Linked Telegram user ${user} to hub ${values.hub}.`);
}

if (!existing) {
  console.log(`Hub ${values.hub} registered.`);
} else if (key) {
  console.log(`Key of hub ${values.hub} rotated; the old key no longer works.`);
}
if (key) {
  console.log('\nHub key (enter it as "Device key" in the web installer; shown only once):\n');
  console.log(`  ${key}\n`);
} else if (values.user.length === 0 && !values.name) {
  console.log('Nothing to do: the hub exists. Use --user, --name or --rotate-key.');
}
