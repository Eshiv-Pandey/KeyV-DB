/**
 * keyvdb-client — Node.js client for KeyVDB
 *
 * @example
 * ```ts
 * import { KeyVDB } from 'keyvdb-client';
 *
 * const db = new KeyVDB({ host: 'localhost', port: 6380 });
 * await db.connect();
 *
 * // Simple helper: run a function inside a transaction with auto-retry on deadlock
 * const value = await db.transact(async (txn) => {
 *   await txn.put('hello', 'world');
 *   return txn.get('hello');
 * });
 *
 * console.log(value); // 'world'
 * await db.disconnect();
 * ```
 */

import { Connection, ConnectionOptions, DeadlockError, KeyVDBError } from './connection';
import { Transaction } from './transaction';
import { buildMessage, OP_BEGIN } from './protocol';

export { DeadlockError, KeyVDBError } from './connection';
export type { ConnectionOptions } from './connection';
export { Transaction } from './transaction';

/**
 * A KeyVDB client. Maintains a single TCP connection to the server.
 *
 * Create one instance per application (or per logical connection).
 * Not safe to share a single client across threads (Node.js is single-threaded
 * so this is not normally a concern).
 */
export class KeyVDB {
  private conn: Connection;

  constructor(opts: ConnectionOptions = {}) {
    this.conn = new Connection(opts);
  }

  /**
   * Establish the TCP connection to the server.
   * Must be called before `begin()` or `transact()`.
   */
  async connect(): Promise<void> {
    await this.conn.connect();
  }

  /**
   * Close the TCP connection.
   */
  async disconnect(): Promise<void> {
    this.conn.close();
  }

  /**
   * Begin a new transaction and return it.
   * You are responsible for calling `commit()` or `rollback()`.
   */
  async begin(): Promise<Transaction> {
    const msg = buildMessage(OP_BEGIN);
    const res = await this.conn.send(msg);
    Connection.checkResponse(res);
    return new Transaction(this.conn);
  }

  /**
   * Run `fn` inside a transaction with automatic retry on deadlock.
   *
   * The callback receives a `Transaction` object. If `fn` throws a
   * `DeadlockError`, the transaction is rolled back and `fn` is retried
   * up to `maxRetries` times (default: 10). Any other error propagates.
   *
   * The return value of `fn` is returned from `transact()`.
   *
   * @example
   * ```ts
   * await db.transact(async (txn) => {
   *   const v = await txn.get('counter') ?? '0';
   *   await txn.put('counter', String(Number(v) + 1));
   * });
   * ```
   */
  async transact<T>(
    fn: (txn: Transaction) => Promise<T>,
    maxRetries = 10,
  ): Promise<T> {
    for (let attempt = 0; attempt <= maxRetries; attempt++) {
      const txn = await this.begin();
      try {
        const result = await fn(txn);
        await txn.commit();
        return result;
      } catch (err) {
        await txn.rollback().catch(() => {}); // best-effort rollback
        if (err instanceof DeadlockError && attempt < maxRetries) {
          // Small backoff before retry.
          await new Promise(r => setTimeout(r, 5 * attempt));
          continue;
        }
        throw err;
      }
    }
    throw new DeadlockError(); // exhausted retries
  }

  /** True if the underlying socket is connected. */
  get connected(): boolean {
    return this.conn.connected;
  }
}
