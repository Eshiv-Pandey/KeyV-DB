/**
 * protocol.ts — Wire-format constants matching server/protocol.h
 */

export const OP_BEGIN    = 0x01;
export const OP_GET      = 0x02;
export const OP_PUT      = 0x03;
export const OP_DELETE   = 0x04;
export const OP_COMMIT   = 0x05;
export const OP_ROLLBACK = 0x06;

export const STATUS_OK        = 0x00;
export const STATUS_NOT_FOUND = 0x01;
export const STATUS_ERROR     = 0x02;
export const STATUS_DEADLOCK  = 0x03;

export const DEFAULT_PORT = 6380;

export const HEADER_SIZE = 5; // 1 byte opcode + 4 bytes payload_len

/**
 * Build a request message buffer.
 * @param opcode  - one of OP_* constants
 * @param payload - optional payload bytes
 */
export function buildMessage(opcode: number, payload?: Buffer): Buffer {
  const payloadLen = payload ? payload.length : 0;
  const buf = Buffer.allocUnsafe(HEADER_SIZE + payloadLen);
  buf.writeUInt8(opcode, 0);
  buf.writeUInt32BE(payloadLen, 1);
  if (payload && payloadLen > 0) {
    payload.copy(buf, HEADER_SIZE);
  }
  return buf;
}

/**
 * Encode a length-prefixed string (uint16_t len + utf8 bytes).
 */
export function encodeString(s: string): Buffer {
  const strBuf = Buffer.from(s, 'utf8');
  const out = Buffer.allocUnsafe(2 + strBuf.length);
  out.writeUInt16BE(strBuf.length, 0);
  strBuf.copy(out, 2);
  return out;
}
