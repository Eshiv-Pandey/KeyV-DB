#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// protocol.h — Wire format for the KeyVDB TCP protocol.
//
// Every message (client → server and server → client) has the same envelope:
//
//   [uint8_t  opcode       ]   1 byte
//   [uint32_t payload_len  ]   4 bytes, big-endian
//   [char[]   payload      ]   payload_len bytes
//
// Client opcodes (requests):
//   BEGIN   0x01  —  no payload
//   GET     0x02  —  payload: [uint16_t key_len][key_bytes]
//   PUT     0x03  —  payload: [uint16_t key_len][key][uint16_t val_len][val]
//   DELETE  0x04  —  payload: [uint16_t key_len][key_bytes]
//   COMMIT  0x05  —  no payload
//   ROLLBACK 0x06 —  no payload
//
// Server status codes (first byte of every response payload):
//   OK        0x00  —  success; value follows for GET, empty for others
//   NOT_FOUND 0x01  —  key not present (GET only)
//   ERROR     0x02  —  general error; remaining payload is error message string
//   DEADLOCK  0x03  —  lock timeout; client should rollback and retry
//
// Response payload formats:
//   BEGIN    OK:         [0x00]
//   GET      OK:         [0x00][uint16_t val_len][val_bytes]
//   GET      NOT_FOUND:  [0x01]
//   PUT      OK:         [0x00]
//   DELETE   OK:         [0x00]
//   COMMIT   OK:         [0x00]
//   ROLLBACK OK:         [0x00]
//   Any      ERROR:      [0x02][error message, null-terminated]
//   Any      DEADLOCK:   [0x03]
//
// All multi-byte integers are big-endian on the wire.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {
namespace proto {

// ── Opcodes ───────────────────────────────────────────────────────────────────
static constexpr uint8_t OP_BEGIN    = 0x01;
static constexpr uint8_t OP_GET      = 0x02;
static constexpr uint8_t OP_PUT      = 0x03;
static constexpr uint8_t OP_DELETE   = 0x04;
static constexpr uint8_t OP_COMMIT   = 0x05;
static constexpr uint8_t OP_ROLLBACK = 0x06;

// ── Status bytes ──────────────────────────────────────────────────────────────
static constexpr uint8_t STATUS_OK        = 0x00;
static constexpr uint8_t STATUS_NOT_FOUND = 0x01;
static constexpr uint8_t STATUS_ERROR     = 0x02;
static constexpr uint8_t STATUS_DEADLOCK  = 0x03;

// ── Default port ──────────────────────────────────────────────────────────────
static constexpr int DEFAULT_PORT = 6380;

} // namespace proto
} // namespace keyvdb
