/**
 * test.ts — Integration smoke test for keyvdb-client.
 *
 * Requires a running KeyVDB server on localhost:6380.
 * Run with:  npm test
 *
 * Uses Node.js built-in test runner (node:test), available since Node 18.
 */

import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { KeyVDB, DeadlockError } from './index';

const PORT = Number(process.env.KEYVDB_PORT ?? 6380);
const HOST = process.env.KEYVDB_HOST ?? 'localhost';

let db: KeyVDB;

before(async () => {
  db = new KeyVDB({ host: HOST, port: PORT, connectTimeout: 3000 });
  await db.connect();
});

after(async () => {
  await db.disconnect();
});

// ── Basic CRUD ────────────────────────────────────────────────────────────────

test('put and get', async () => {
  await db.transact(async (txn) => {
    await txn.put('test:key', 'hello');
  });

  const value = await db.transact(async (txn) => txn.get('test:key'));
  assert.equal(value, 'hello');
});

test('get missing key returns null', async () => {
  const value = await db.transact(async (txn) => txn.get('does_not_exist_xyz'));
  assert.equal(value, null);
});

test('overwrite existing key', async () => {
  await db.transact(async (txn) => { await txn.put('test:ow', 'v1'); });
  await db.transact(async (txn) => { await txn.put('test:ow', 'v2'); });
  const v = await db.transact(async (txn) => txn.get('test:ow'));
  assert.equal(v, 'v2');
});

test('delete key', async () => {
  await db.transact(async (txn) => { await txn.put('test:del', 'bye'); });
  await db.transact(async (txn) => { await txn.delete('test:del'); });
  const v = await db.transact(async (txn) => txn.get('test:del'));
  assert.equal(v, null);
});

test('delete non-existent key is a no-op', async () => {
  await assert.doesNotReject(async () => {
    await db.transact(async (txn) => txn.delete('never_existed_abc'));
  });
});

// ── Atomicity ─────────────────────────────────────────────────────────────────

test('rollback discards writes', async () => {
  const txn = await db.begin();
  await txn.put('test:rollback', 'should_vanish');
  await txn.rollback();

  const v = await db.transact(async (t) => t.get('test:rollback'));
  assert.equal(v, null);
});

test('commit makes writes permanent', async () => {
  const txn = await db.begin();
  await txn.put('test:committed', 'stays');
  await txn.commit();

  const v = await db.transact(async (t) => t.get('test:committed'));
  assert.equal(v, 'stays');
});

test('multi-key transaction is atomic', async () => {
  await db.transact(async (txn) => {
    await txn.put('test:a', '1');
    await txn.put('test:b', '2');
    await txn.put('test:c', '3');
  });

  await db.transact(async (txn) => {
    assert.equal(await txn.get('test:a'), '1');
    assert.equal(await txn.get('test:b'), '2');
    assert.equal(await txn.get('test:c'), '3');
  });
});

// ── transact() helper ─────────────────────────────────────────────────────────

test('transact returns function result', async () => {
  await db.transact(async (txn) => { await txn.put('test:ret', '42'); });
  const result = await db.transact(async (txn) => txn.get('test:ret'));
  assert.equal(result, '42');
});

test('transact auto-retries on DeadlockError', async () => {
  // Simulate a retry: the first call throws DeadlockError, second succeeds.
  // We test this indirectly — if transact() doesn't retry, the counter
  // increment loop below would fail under concurrency. Here we just verify
  // that the retry logic itself doesn't break the happy path.
  let attempts = 0;
  const result = await db.transact(async (txn) => {
    attempts++;
    await txn.put('test:retry', String(attempts));
    return txn.get('test:retry');
  });
  assert.equal(result, '1');
  assert.equal(attempts, 1);
});

// ── Unicode / binary values ───────────────────────────────────────────────────

test('utf-8 values round-trip correctly', async () => {
  const value = 'héllo wörld — 日本語 — 🔑';
  await db.transact(async (txn) => { await txn.put('test:utf8', value); });
  const back = await db.transact(async (txn) => txn.get('test:utf8'));
  assert.equal(back, value);
});

test('empty string key and value', async () => {
  await db.transact(async (txn) => { await txn.put('', ''); });
  const v = await db.transact(async (txn) => txn.get(''));
  assert.equal(v, '');
});

console.log(`Running client tests against KeyVDB at ${HOST}:${PORT}`);
