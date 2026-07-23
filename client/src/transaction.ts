import { Connection, DeadlockError } from './connection';
import {
  buildMessage, encodeString,
  OP_GET, OP_PUT, OP_DELETE, OP_COMMIT, OP_ROLLBACK,
  STATUS_NOT_FOUND,
} from './protocol';

/**
 * An open transaction on a KeyVDB connection.
 *
 * Obtain one via `db.begin()`. Every read/write must happen inside a
 * transaction. Call `commit()` or `rollback()` to end it.
 *
 * If the server returns DEADLOCK on any operation, a `DeadlockError` is
 * thrown. You must roll back (the transaction is already invalidated server-
 * side) and retry:
 *
 * ```ts
 * for (;;) {
 *   const txn = await db.begin();
 *   try {
 *     await txn.put('key', 'value');
 *     await txn.commit();
 *     break;
 *   } catch (e) {
 *     if (e instanceof DeadlockError) continue; // retry
 *     throw e;
 *   }
 * }
 * ```
 */
export class Transaction {
  private done = false;

  /** @internal */
  constructor(private readonly conn: Connection) {}

  /**
   * Read a key. Returns `null` if the key does not exist.
   * Acquires a shared (read) lock on the key.
   */
  async get(key: string): Promise<string | null> {
    this.assertOpen();
    const payload = encodeString(key);
    const msg = buildMessage(OP_GET, payload);
    const res = await this.conn.send(msg);

    if (res.status === STATUS_NOT_FOUND) return null;
    Connection.checkResponse(res);

    // Payload: [uint16_t val_len][val_bytes]
    const valLen = res.payload.readUInt16BE(0);
    return res.payload.slice(2, 2 + valLen).toString('utf8');
  }

  /**
   * Write a key-value pair. Creates or overwrites.
   * Acquires an exclusive (write) lock on the key.
   */
  async put(key: string, value: string): Promise<void> {
    this.assertOpen();
    const payload = Buffer.concat([encodeString(key), encodeString(value)]);
    const msg = buildMessage(OP_PUT, payload);
    const res = await this.conn.send(msg);
    Connection.checkResponse(res);
  }

  /**
   * Delete a key. No-op if the key does not exist.
   * Acquires an exclusive (write) lock on the key.
   */
  async delete(key: string): Promise<void> {
    this.assertOpen();
    const payload = encodeString(key);
    const msg = buildMessage(OP_DELETE, payload);
    const res = await this.conn.send(msg);
    Connection.checkResponse(res);
  }

  /**
   * Commit the transaction. All writes become durable.
   * Releases all held locks.
   */
  async commit(): Promise<void> {
    this.assertOpen();
    this.done = true;
    const msg = buildMessage(OP_COMMIT);
    const res = await this.conn.send(msg);
    Connection.checkResponse(res);
  }

  /**
   * Roll back the transaction. All writes are discarded.
   * Releases all held locks.
   */
  async rollback(): Promise<void> {
    if (this.done) return; // already ended — silently ok
    this.done = true;
    const msg = buildMessage(OP_ROLLBACK);
    const res = await this.conn.send(msg);
    Connection.checkResponse(res);
  }

  private assertOpen(): void {
    if (this.done) {
      throw new Error('KeyVDB: transaction already ended — call db.begin() for a new one');
    }
  }
}

export { DeadlockError };
