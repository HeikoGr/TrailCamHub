#!/usr/bin/env node
// Registers a hub, links Telegram users to it, and prints its key.
//
//   npm run register-hub -- --hub garden-hub [--name "Garden"] \
//     [--user <telegramUserId>]... [--rotate-key] [--pair [--pair-hours <h>]] \
//     [--stack trailcamhub | --function <name>] [--region <region>]
//
// - A new hub gets a key, printed once.
// - For an existing hub, --user links more people (family members) without
//   touching the key; --rotate-key issues a new key (the old one stops working).
// - --pair prints a one-time link (https://t.me/<bot>?start=<code>, valid for
//   a week or --pair-hours): whoever opens it in Telegram is linked to the
//   hub, without looking up their user id. Send it to yourself or a family
//   member; each link works once.
// - Cameras are not registered by hand: they appear automatically when the
//   hub reports them, and every user linked to the hub sees them.
//
// The script invokes the backend's Lambda function through the AWS CLI, and
// the function writes to the table. AWS projects deny people direct access to
// DynamoDB data, but the function may still use its table. Needs the AWS CLI
// with credentials allowed lambda:InvokeFunction (and, to find the function,
// cloudformation:DescribeStackResources). Without --region, the AWS CLI's
// own region applies (AWS_REGION, AWS_DEFAULT_REGION or the profile), so the
// script works in an AWS project's home Region without extra flags.

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';

const { values } = parseArgs({
  options: {
    hub: { type: 'string' },
    name: { type: 'string' },
    user: { type: 'string', multiple: true, default: [] },
    'rotate-key': { type: 'boolean', default: false },
    pair: { type: 'boolean', default: false },
    'pair-hours': { type: 'string' },
    stack: { type: 'string', default: 'trailcamhub' },
    function: { type: 'string' },
    table: { type: 'string' }, // no longer needed; accepted for old command lines
    region: { type: 'string' },
  },
});

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

if (!values.hub || !/^[A-Za-z0-9_-]{1,64}$/.test(values.hub)) {
  fail('--hub must be 1-64 characters [A-Za-z0-9_-]');
}
for (const user of values.user) {
  if (!/^\d+$/.test(user)) fail(`--user must be a numeric Telegram user id (got ${user})`);
}
if (values['pair-hours'] !== undefined && !/^\d+$/.test(values['pair-hours'])) {
  fail('--pair-hours must be a number of hours (1-720)');
}
if (values.table) console.warn('note: --table is no longer needed and is ignored.');

function aws(args) {
  try {
    return execFileSync('aws', values.region ? [...args, '--region', values.region] : args, {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'inherit'],
    }).trim();
  } catch (error) {
    if (error.code === 'ENOENT') fail('the AWS CLI (aws) is not installed or not in PATH');
    fail(`aws ${args.slice(0, 2).join(' ')} failed`);
  }
}

function findFunction() {
  const name = aws([
    'cloudformation',
    'describe-stack-resources',
    '--stack-name',
    values.stack,
    '--query',
    "StackResources[?ResourceType=='AWS::Lambda::Function'].PhysicalResourceId | [0]",
    '--output',
    'text',
  ]);
  if (!name || name === 'None') fail(`no Lambda function in stack ${values.stack}`);
  return name;
}

const functionName = values.function ?? findFunction();

/** Runs an admin task in the function and returns its JSON result. */
function invoke(event) {
  // The response may hold the hub key, so it goes to a private temporary
  // file rather than a fixed path.
  const dir = mkdtempSync(join(tmpdir(), 'register-hub-'));
  const outFile = join(dir, 'response.json');
  try {
    const functionError = aws([
      'lambda',
      'invoke',
      '--function-name',
      functionName,
      '--cli-binary-format',
      'raw-in-base64-out',
      '--payload',
      JSON.stringify(event),
      '--query',
      'FunctionError',
      '--output',
      'text',
      outFile,
    ]);
    const text = readFileSync(outFile, 'utf8');
    if (functionError && functionError !== 'None') {
      fail(`function ${functionName} failed: ${text}`);
    }
    return JSON.parse(text);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

const result = invoke({
  admin: 'registerHub',
  hubId: values.hub,
  name: values.name,
  users: values.user.map(Number),
  rotateKey: values['rotate-key'],
});

if (!result.ok) fail(result.error);

for (const user of result.linkedUsers) {
  console.log(`Linked Telegram user ${user} to hub ${values.hub}.`);
}
if (result.created) {
  console.log(`Hub ${values.hub} registered.`);
} else if (result.key) {
  console.log(`Key of hub ${values.hub} rotated; the old key no longer works.`);
}
if (result.key) {
  console.log('\nHub key (enter it as "Device key" in the web installer; shown only once):\n');
  console.log(`  ${result.key}\n`);
} else if (result.linkedUsers.length === 0 && !values.name && !values.pair) {
  console.log('Nothing to do: the hub exists. Use --user, --name, --rotate-key or --pair.');
}

if (values.pair) {
  const pairing = invoke({
    admin: 'createPairingCode',
    hubId: values.hub,
    ttlHours: values['pair-hours'] === undefined ? undefined : Number(values['pair-hours']),
  });
  if (!pairing.ok) fail(pairing.error);
  const until = new Date(pairing.expiresAt * 1000).toISOString().replace('T', ' ').slice(0, 16);
  console.log(`\nPairing link for hub ${values.hub} (works once, until ${until} UTC):\n`);
  console.log(`  ${pairing.link ?? `code ${pairing.code} (bot not configured)`}\n`);
}
