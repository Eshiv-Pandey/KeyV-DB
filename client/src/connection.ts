import * as net from 'net';
import { HEADER_SIZE, STATUS_OK, STATUS_NOT_FOUND, STATUS_ERROR, STATUS_DEADLOCK } from './protocol';

export interface ConnectionOptions {
  host?: string;
  port?: number;
  /** Milliseconds to wait for a connection. Default: 5000 */
  connectTimeout?: number;
}

/**
 * A response parsed from the server.
 */
export interface Response {
  status: number;
  payload: Buffer;
}

/**
 * Thrown when the server reports a lock timeout / deadlock.
 * The caller should roll back the transaction and retry.
 */
export class DeadlockError extends Error {
  constructor() {
    super('KeyVDB: lock timeout — deadlock detected. Roll back and retry.');
    this.name = 'DeadlockError';
  }
}

/**
 * Thrown when the server returns an application-level error.
 */
export class KeyVDBError extends Error {
  constructor(message: string) {
    super('KeyVDB: ' + message);
    this.name = 'KeyVDBError';
  }
}

/**
 * Low-level TCP connection to a KeyVDB server.
 * Manages the socket and provides sendAndReceive().
 */
export class Connection {
  private socket: net.Socket | null = null;
  private host: string;
  private port: number;
  private connectTimeout: number;

  // Buffer for incoming data that hasn't been fully read yet.
  private recvBuf: Buffer = Buffer.alloc(0);
  // Queue of pending resolve/reject callbacks for in-flight requests.
  private pendingResolvers: Array<(r: Response) => void> = [];
  private pendingRejecters:  Array<(e: Error) => void>   = [];

  constructor(opts: ConnectionOptions = {}) {
    this.host           = opts.host           ?? 'localhost';
    this.port           = opts.port           ?? 6380;
    this.connectTimeout = opts.connectTimeout ?? 5000;
  }

  async connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      const sock = new net.Socket();

      const timer = setTimeout(() => {
        sock.destroy();
        reject(new Error(`KeyVDB: connection timeout to ${this.host}:${this.port}`));
      }, this.connectTimeout);

      sock.once('connect', () => {
        clearTimeout(timer);
        this.socket = sock;
        resolve();
      });

      sock.once('error', (err) => {
        clearTimeout(timer);
        reject(err);
      });

      sock.on('data', (chunk: Buffer) => {
        this.recvBuf = Buffer.concat([this.recvBuf, chunk]);
        this.drainResponses();
      });

      sock.on('close', () => {
        // Reject all pending requests.
        const err = new Error('KeyVDB: connection closed');
        while (this.pendingRejecters.length > 0) {
          this.pendingRejecters.shift()!(err);
          this.pendingResolvers.shift();
        }
      });

      sock.connect(this.port, this.host);
    });
  }

  /**
   * Send a raw message buffer and wait for the server's response.
   * Requests are pipelined — responses arrive in order.
   */
  send(msg: Buffer): Promise<Response> {
    return new Promise((resolve, reject) => {
      if (!this.socket || this.socket.destroyed) {
        reject(new Error('KeyVDB: not connected'));
        return;
      }
      this.pendingResolvers.push(resolve);
      this.pendingRejecters.push(reject);
      this.socket.write(msg);
    });
  }

  close(): void {
    this.socket?.destroy();
    this.socket = null;
  }

  get connected(): boolean {
    return this.socket !== null && !this.socket.destroyed;
  }

  // ── Internal: parse completed responses from the receive buffer ─────────────

  private drainResponses(): void {
    while (this.recvBuf.length >= HEADER_SIZE) {
      const status     = this.recvBuf.readUInt8(0);
      const payloadLen = this.recvBuf.readUInt32BE(1);
      const totalLen   = HEADER_SIZE + payloadLen;

      if (this.recvBuf.length < totalLen) break; // incomplete — wait for more data

      const payload = this.recvBuf.slice(HEADER_SIZE, totalLen);
      this.recvBuf  = this.recvBuf.slice(totalLen);

      const resolve = this.pendingResolvers.shift();
      this.pendingRejecters.shift();
      if (resolve) resolve({ status, payload });
    }
  }

  // ── Helper: check response and throw on error ──────────────────────────────

  static checkResponse(res: Response): void {
    if (res.status === STATUS_OK) return;
    if (res.status === STATUS_NOT_FOUND) return; // caller handles
    if (res.status === STATUS_DEADLOCK) throw new DeadlockError();
    if (res.status === STATUS_ERROR) {
      const msg = res.payload.toString('utf8').replace(/\0+$/, '');
      throw new KeyVDBError(msg);
    }
    throw new KeyVDBError(`unexpected status byte: 0x${res.status.toString(16)}`);
  }
}
