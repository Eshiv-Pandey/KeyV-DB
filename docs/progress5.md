# progress5.md — Integration and Phase 1 Complete

*Milestone 5. All four ACID properties are now implemented, tested, and verified. This entry covers what "done" looks like for Phase 1: how we proved the full system works correctly, what DB::Close() does, and what the test suite covers.*

*Milestones 1–4 built the pieces. Milestone 5 verifies they fit together correctly.*

---

## What "Phase 1 complete" means

By the end of Milestone 4, KeyVDB had all the right components:

- **Storage manager** (DiskManager + BufferPool + FreeList): pages read and written with fsync durability
- **B+Tree** (insert, get, delete, split, merge): ordered key-value storage with structural integrity
- **Write-ahead log** (LogManager + RecoveryManager): crash-safe commit protocol, redo/undo recovery
- **Transaction layer** (DB + Transaction + LockManager): 2PL isolation, deadlock handling, public API

Phase 1 is complete when all four ACID properties are demonstrably correct under realistic conditions: concurrent transactions, crash simulation, large datasets, and multiple close-reopen cycles. That's what Milestone 5 tests.

---

## DB::Close()

Before Milestone 5, the database had no explicit shutdown operation. The destructor relied on `~BufferPool()` → `flush_all()`, which works for clean shutdowns but doesn't fsync the data file or checkpoint the WAL.

`DB::Close()` does three things explicitly:

1. `bp_.flush_all()` — write all dirty B+Tree pages to the data file
2. `dm_.flush()` — fsync the data file (data is now durable)
3. `log_.truncate()` — zero the WAL file

After `Close()`, the database is in a clean state: data file fully up-to-date, WAL empty. On the next open, the recovery phase finds an empty WAL and skips recovery entirely. This is faster than a cold reopen with a long WAL to replay.

Without `Close()`, the destructor path is: `~Transaction()` auto-rollbacks any open transactions → `~BufferPool()` calls `flush_all()` → data pages are written but not fsynced, and the WAL is not truncated. On next open, recovery replays the WAL. The result is correct (recovery handles it) but slower and wastes disk space. `Close()` is the preferred path for any production use.

---

## The integration test suite

`integration_test.cpp` treats the entire system as a black box. It verifies each ACID property independently and then in combination.

### Atomicity tests

**MultiKeyCommitIsAllOrNothing**: 20 keys written in one transaction, all verified present after commit. This confirms that a single transaction can write multiple keys and all of them commit atomically.

**RollbackLeavesNothingBehind**: 20 keys written then rolled back. A previously committed key is verified present; all rolled-back keys verified absent. This exercises the write-set undo path.

**CrashMidTxnUndoneByRecovery**: the most honest atomicity test. We bypass the DB API and inject WAL records directly — a `BEGIN` and 10 `WRITE` records with no `COMMIT`. Then reopen. Recovery must undo all 10 partial writes. This simulates a process crash immediately after writing data but before committing.

### Consistency tests

These exercise the B+Tree's structural correctness under heavy load — not just "the data is there" but "the tree is internally consistent after thousands of mutations."

**HeavyInsertDeleteLeavesBTreeValid**: 5000 sequential inserts, delete the even half, verify every odd key present and every even key absent. At 5000 keys the tree is several levels deep with dozens of internal nodes; the delete phase triggers hundreds of leaf merges, borrows, and tree-height reductions.

**ShuffledInsertAndDelete**: 500 keys inserted in random order (not sequential), then a random 250 deleted. This exercises splits that occur in non-sequential key order, which stresses the node-splitting logic more than sequential inserts do.

### Isolation tests

**NoDirtyReads**: txn1 writes "x" (holds X-lock). txn2 tries to read "x". txn2 must be blocked by the lock, never seeing the dirty value. This verifies the most fundamental isolation guarantee.

**WriterReaderSerializable**: txn1 writes "k", txn2 is blocked, txn1 commits, txn3 reads the committed value. The full "writer → reader serialization" chain.

**NoLostUpdates**: 4 threads each increment a counter 5 times. With correct 2PL, all increments are serialized. Final value must be exactly 20. Without locking, some increments would be lost.

**MultiKeyTransferIsAtomic**: a concurrent observer reads keys "a" and "b" while a transfer txn moves value between them. The observer must always see `a + b == 10`. This proves that multi-key transactions are indivisible from a concurrent reader's perspective — the most important form of isolation for banking-style applications.

**CommitUnblocksWaiter**: a thread blocks on a lock, and a commit from another thread unblocks it. This tests the condition variable path in `LockManager::release_all`.

### Durability tests

**CommittedSurvivesClose**: 200 keys committed, DB closed, DB reopened, all 200 keys verified. This is the fundamental durability check.

**RolledBackDoesNotPersist**: a committed key and a rolled-back key. After close+reopen, committed is present, rolled-back is absent. Verifies that rollback is durable across restarts.

**MultipleCloseCycles**: three separate open→write→close cycles, each writing different keys. After the third reopen, all keys from all three sessions are present. Tests that WAL truncation and reopen work correctly across multiple sessions.

**LargeTransactionSurvivesReopen**: 1000 keys in a single transaction (forcing multiple B+Tree splits). Close and reopen. All 1000 keys verified. Tests that large WAL records and large dirty-page sets are fully persisted.

### The bank transfer (combined ACID)

The final test exercises all four properties simultaneously, the way real applications do.

Four accounts with 100 each. Two transfer workers concurrently move money between random account pairs. One checker thread reads all four balances on every iteration and verifies the total never changes.

- **Atomicity**: each transfer writes two accounts atomically. No transfer moves money from one account without adding it to another.
- **Consistency**: total balance must always be 400. Any partial write would show 400 − transferred_amount during the window between the debit and the credit.
- **Isolation**: the checker sees only committed states. It never observes a debit without the corresponding credit.
- **Durability**: final state is verified after `Close()` + reopen.

The checker found zero inconsistent reads in all test runs.

---

## How timeouts were tuned

The lock timeout problem was the main engineering challenge of Milestone 5. The original 200ms timeout meant that each expected-to-block test took 200ms, and the concurrent stress tests accumulated hundreds of timeouts. The full suite took over 100 seconds.

The solution: make `LockManager`'s timeout configurable in the constructor, and expose it through `DB::Open(path, timeout_ms)`. Tests that expect `DeadlockException` use `FAST = 15ms`. Tests where a thread is deliberately unblocked by another use `SLOW = 500ms` (enough headroom that the commit arrives before the timeout). Serial tests use the default 200ms (irrelevant since they don't contend).

This brought the full suite from ~110 seconds to ~15 seconds without changing any correctness behaviour.

---

## The main binary

`src/main.cpp` is now a real smoke test rather than "Build OK." It:

1. Opens a new database
2. Commits three keys
3. Reads them back (including a missing key)
4. Rolls back a write and verifies it's gone
5. Deletes a key and verifies it's gone
6. Closes and reopens
7. Reads all three keys and verifies the deleted one is still absent
8. Cleans up

Running `./keyvdb` after a build gives immediate confirmation that the full stack works end-to-end.

---

## What's left: Phase 2

Phase 1 (the embedded library) is complete. The API is:

```cpp
auto db = DB::Open("mydb.db");

auto txn = db->Begin();
txn->Put("hello", "world");
txn->Commit();

auto t2 = db->Begin();
auto v = t2->Get("hello");   // std::optional<std::string>
t2->Rollback();

db->Close();
```

Phase 2 adds a TCP server layer so the database can serve network clients:
- A TCP server that listens on a configurable port
- A simple binary protocol: serialize transaction operations over the wire
- A Docker image for easy deployment
- A Node.js client library

The storage, B+Tree, WAL, and transaction layers don't change. Phase 2 is a network adapter on top of what exists.

---

*The learning-in-public log ends here for Phase 1. All five milestones complete. The docs directory contains a detailed write-up of every design decision, and the interview.md covers the full range of questions a systems engineering interview would ask about this project.*
