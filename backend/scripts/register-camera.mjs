#!/usr/bin/env node
// Registers a camera, links it to a Telegram user and prints a new device key.
//
//   npm run register-camera -- --table <TableName> --cam F4C15606ED3B \
//     --name Garden --user <telegramUserId> [--region eu-central-1]
//
// Re-running for an existing camera rotates its key (the old one stops working)
// and keeps its telemetry. Uses the standard AWS credential chain.

import { createHash, randomBytes } from 'node:crypto';
import { parseArgs } from 'node:util';
import { DynamoDBClient } from '@aws-sdk/client-dynamodb';
import { DynamoDBDocumentClient, GetCommand, PutCommand } from '@aws-sdk/lib-dynamodb';

const { values } = parseArgs({
  options: {
    table: { type: 'string' },
    cam: { type: 'string' },
    name: { type: 'string' },
    user: { type: 'string' },
    region: { type: 'string', default: process.env.AWS_REGION ?? 'eu-central-1' },
  },
});

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

if (!values.table) fail('--table is required (stack output TableName)');
if (!values.cam || !/^[A-Za-z0-9_-]{1,64}$/.test(values.cam)) fail('--cam must be [A-Za-z0-9_-]');
if (!values.user || !/^\d+$/.test(values.user)) fail('--user must be a numeric Telegram user id');

const db = DynamoDBDocumentClient.from(new DynamoDBClient({ region: values.region }), {
  marshallOptions: { removeUndefinedValues: true },
});

const pk = `CAM#${values.cam}`;
const { Item: existing } = await db.send(
  new GetCommand({ TableName: values.table, Key: { PK: pk, SK: 'META' } }),
);

const secret = randomBytes(32).toString('base64url');
const keyHash = createHash('sha256').update(secret).digest('hex');

await db.send(
  new PutCommand({
    TableName: values.table,
    Item: {
      ...existing,
      PK: pk,
      SK: 'META',
      camId: values.cam,
      name: values.name ?? existing?.name ?? values.cam,
      keyHash,
      lastSeen: existing?.lastSeen ?? 0,
    },
  }),
);
await db.send(
  new PutCommand({
    TableName: values.table,
    Item: { PK: `USER#${values.user}`, SK: `CAM#${values.cam}`, camId: values.cam },
  }),
);

console.log(existing ? 'Camera updated, key rotated.' : 'Camera registered.');
console.log(`Linked to Telegram user ${values.user}.`);
console.log('\nDevice key (store it on the hub; it is shown only once):\n');
console.log(`  ${values.cam}.${secret}\n`);
