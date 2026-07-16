# progress3.md — The Write-Ahead Log and Crash Recovery

*Milestone 3. We now have a real durability guarantee. When a transaction commits, its data is safe — not "probably safe," not "safe unless the OS is lying" — actually safe. This entry explains the Write-Ahead Log, why it exists, and how the recovery algorithm works.*

*This picks up right after progress2.md. The B+Tree from Milestone 2 is the data structure we're protecting.*

---

## The problem: what does "committed" mean?

Before Milestone 3, the database had no atomicity guarantee. If you wrote five keys in a row and the process crashed after the third, you'd have a tree with three of five writes applied. The B+Tree wouldn't be corrupted in a structural sense — no split was in progress — but the application data was partially written. There was no way to know which keys made it and which didn't.

The fix has two parts:

1. **Log before you modify.** Before touching the B+Tree, write a record to a log file that describes what you're about to do. If you crash mid-write, you can look at the log on restart and figure out what needs to be undone.

2. **Commit = log fsync, not data fsync.** The moment a transaction is considered durable is the moment its COMMIT record is `fsync`'d to the log. The actual B+Tree pages can be written lazily after that — if they weren't written before a crash, the log lets us redo them.

This is the Write-Ahead Log (WAL). "Write-ahead" literally means: write to the log ahead of writing to the data.

---

## What lives in the WAL file

The WAL is a flat binary file — just records appended in sequence, no modification. Records are never moved or overwritten. The only mutation is truncation after a successful recovery and checkpoint.

Each record has a 32-byte header:

```
lsn          (8 bytes) — Log Sequence Number, monotonically increasing
txn_id       (8 bytes) — which transaction wrote this record
type         (4 bytes) — BEGIN / WRITE / COMMIT / ABORT
key_len      (4 bytes) — 0 for non-WRITE records
before_len   (4 bytes) — 0 for INSERT (key didn't exist before)
after_len    (4 bytes) — 0 for DELETE
```

The payload (key, before_val, after_val) follows immediately after the header.

**WRITE records capture both directions**: the "after" value is used for redo; the "before" value is used for undo. Every write is fully reversible from the log.

**Four record types:**
- `BEGIN` — a new transaction started. No payload.
- `WRITE` — a key was inserted, updated, or deleted. Full before+after payload.
- `COMMIT` — all prior WRITEs are durable. After fsync, this is the point of no return.
- `ABORT` — transaction was rolled back. Writes were undone in-memory before this was written.

---

## The commit protocol

The sequence for a successful commit:

```
1. Log BEGIN                     (written to WAL file, not yet fsynced)
2. For each write:
   a. Log WRITE(key, before, after)   (written to WAL)
   b. Apply to B+Tree in memory       (dirty page in buffer pool)
3. Log COMMIT                    (written to WAL)
4. fsync(WAL file)               ← this is the durability point
5. flush dirty B+Tree pages      (written to data file)
6. fsync(data file)              (optional — the WAL ensures recovery)
```

The critical insight: after step 4, the transaction is permanent. If the process crashes after step 4 but before step 6, recovery will redo steps 5 and 6 automatically using the WAL records. If the process crashes before step 4, recovery will see no COMMIT record and undo any partial changes.

This is why we `fsync` the WAL file rather than the data file at commit time. The WAL is the source of truth. The data file is just a performance optimization (so recovery doesn't have to replay the entire WAL from the beginning on every startup).

---

## Rollback: undo in memory

When a transaction is rolled back, we don't need the WAL at all — we undo the changes in memory right now. The transaction's write-set stores the before-image of every key it touched:

```cpp
for (auto& [k, we] : write_set_) {
    if (!we.before_exists) {
        btree_.remove(k);           // key was inserted — delete it
    } else {
        btree_.insert(k, we.before_val);  // key existed — restore original
    }
}
```

Then we write an ABORT record to the WAL. This record is important for recovery: if the process crashes between an ABORT record being written and the next clean shutdown, recovery sees ABORT and knows not to undo anything for this transaction (it was already undone in memory before the pages were flushed).

This is the key distinction: **ABORT means "already undone." MISSING = "not yet undone."** Recovery must undo transactions with no COMMIT and no ABORT, not transactions marked ABORT.

---

## Crash recovery: Analysis → Redo → Undo

On startup, before accepting any operations, `RecoveryManager::recover()` runs. It's a simplified version of the ARIES algorithm (Algorithms for Recovery and Isolation Exploiting Semantics), which is what IBM DB2, PostgreSQL, SQL Server, and most serious databases use.

The three phases:

### Analysis

Scan the WAL forward. Build a "loser set" — transactions that have a BEGIN but no COMMIT and no ABORT. These are transactions that were in progress when the crash happened; their writes are partially applied to the B+Tree but were never committed.

A transaction with ABORT is NOT a loser. Its writes were undone in memory at rollback time, and the undone state was flushed to disk as part of normal operation.

### Redo

Replay every WRITE record in LSN order — but only for committed transactions. Skip writes from aborted transactions (those were already reversed before the crash).

The result: the B+Tree is in exactly the state it was in at crash time — all committed writes applied, all aborted writes not present.

"But what about writes from loser (crashed-mid-transaction) transactions?" We include those in redo too: the goal of redo is to get the data to the crash-time state. The undo phase will then clean them up.

Actually — in our implementation, we skip aborted writes in redo (they're clean on disk) but include loser writes. This is subtly different from textbook ARIES but correct: on redo we have either committed data (leave it), aborted data (already reversed on disk), or loser data (will be undone in the next phase).

### Undo

For each loser transaction, walk its WRITE records in reverse LSN order and apply the before-images. If a key was inserted (before_exists = false), delete it. If a key was updated (before_exists = true), restore the original value.

After undo, every loser transaction's effects are completely erased.

---

## The bug that wasn't obvious: page 0 root ID initialization

While building this milestone, we found a bug that had been hiding in the storage layer since Milestone 1: `FreeList::load()` was writing a zero-filled page 0 for new databases without explicitly initializing the root page ID slot to `INVALID_PAGE_ID (-1)`.

`INVALID_PAGE_ID` is -1 (encoded as `0xFFFFFFFF` in memory for `int32_t`). A freshly zeroed slot reads back as `0`, which is a valid page ID — page 0 (the FreeList header page itself). So `DB::Open` was reading `root_page_id() == 0` and concluding this was an existing database rather than a fresh one. It would try to open a B+Tree rooted at page 0, which is typed as `DB_HEADER`, causing an assertion failure in `Node::Node`.

The fix: explicitly write `INVALID_PAGE_ID` to the root slot when initializing page 0 for a new database. This is exactly the kind of bug that only appears when you start composing modules together — each module worked correctly in isolation, but the assumption "zero means unset" broke as soon as we added a layer that needed to distinguish "no root" from "root at page 0."

---

## Recovery after truncation

After a successful recovery, we do three things:

1. `bp_.flush_all()` — write all recovered (redo'd) pages to the data file.
2. `dm_.flush()` — fsync the data file.
3. `log_.truncate()` — zero out the WAL file.

The truncation is safe because everything in the WAL is now reflected in the data file. Future transactions will start with a clean WAL. If a crash happens during truncation (which is a single `ftruncate` call), the worst case is that recovery runs again on the next startup and re-applies already-applied changes — which is idempotent.

In a production system you'd use a "checkpoint" record rather than truncating, so the WAL can be used for point-in-time recovery and replication. We truncate because we're not building a production system — we're building one that demonstrates the concepts correctly.

---

## What the public API looks like now

With Milestone 3 complete, KeyVDB has a real public API in `db.h`:

```cpp
// Open (creates new or reopens existing, running WAL recovery)
auto db = DB::Open("my_database.db");

// Every operation is wrapped in a transaction
auto txn = db->Begin();
txn->Put("key", "value");
txn->Put("other", "data");
txn->Commit();  // WAL fsynced — these writes are durable

// Rollback undoes in-memory changes
auto txn2 = db->Begin();
txn2->Put("mistake", "oops");
txn2->Rollback();  // "mistake" key is gone from the tree

// Read
auto txn3 = db->Begin();
auto value = txn3->Get("key");  // returns std::optional<std::string>
txn3->Rollback();  // no changes to undo
```

The transaction destructor auto-rolls back if neither Commit nor Rollback was called, so forgetting to commit is safe (though inefficient).

---

## What the crash test covers

The `crash_test.sh` script now has a Milestone 3 section that tests five crash points:

- **after_begin**: process crashes immediately after opening. No writes in WAL. Tree is clean on reopen.
- **mid_transaction**: two of five writes logged, no COMMIT. Recovery undoes the partial writes.
- **after_writes_before_commit**: all writes logged but no COMMIT. Same result — full undo.
- **after_fsync_before_data**: COMMIT fsynced but data pages not yet written. Recovery redoes all writes.
- **after_data_written**: everything written correctly. Reopen finds committed data.

For each scenario, `verify_recovery` opens the database (triggering WAL recovery) and checks that the key-value pairs are in the expected state.

---

## What comes next

Milestone 4 is the transaction manager: two-phase locking (2PL) for isolation. What we have now gives atomicity (WAL) and durability (fsync), but two concurrent transactions could still interfere with each other. Milestone 4 adds:

- A lock manager with shared (read) and exclusive (write) locks per key.
- Lock acquisition in `Get` and `Put` — held until commit or rollback.
- Deadlock detection via timeouts (abort the waiting transaction after a configurable threshold).
- Thread-safe access to the BufferPool, LogManager, and BTree.

After Milestone 4, the database satisfies all four ACID properties.

---

*Next: [progress4.md](progress4.md) — two-phase locking and the transaction manager (coming after Milestone 4)*
