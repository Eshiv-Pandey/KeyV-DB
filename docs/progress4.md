# progress4.md — Two-Phase Locking and the Transaction Manager

*Milestone 4. This is the final missing ACID property: isolation. The storage manager gave us the ability to persist data. The B+Tree gave us ordered lookups. The WAL gave us atomicity and durability. Now two-phase locking gives us isolation — the guarantee that concurrent transactions don't see each other's in-progress work.*

*This entry assumes you've read progress1.md through progress3.md.*

---

## What isolation means and why it's hard

Without isolation, two concurrent transactions can produce results that neither would produce alone. The classic example: two threads simultaneously incrementing a counter.

```
Thread 1: read counter (= 5)
Thread 2: read counter (= 5)
Thread 1: write counter = 6
Thread 2: write counter = 6   ← one increment was lost
```

Both reads happened before either write, so both threads incremented from 5. The expected result was 7; the actual result was 6. This is a **lost update** — one of the most common concurrency bugs.

The solution isn't to make operations atomic at the CPU level (that only works for individual word-sized reads and writes). The solution is to make transactions serializable: execute them in such a way that the result is identical to some serial order. Thread 1 fully before Thread 2, or Thread 2 fully before Thread 1 — either is fine, as long as they don't actually interleave.

Two-phase locking achieves this with a simple rule: **lock before you read or write, hold until you commit.**

---

## Two-phase locking: the rule and the phases

2PL has two phases per transaction:

**Growing phase**: the transaction acquires locks as needed. Every `Get` acquires a shared (S) lock. Every `Put` and `Delete` acquires an exclusive (X) lock. The transaction never releases a lock during this phase.

**Shrinking phase**: at commit or rollback, the transaction releases all locks at once. After releasing any lock, it can never acquire a new one.

This "all locks at commit time" variant is called **strict 2PL** or **S2PL**. It's the strongest form and the one we implement. It prevents not only lost updates but also dirty reads, non-repeatable reads, and phantom reads — all the anomalies that weaker isolation levels allow.

The mathematical intuition: if every transaction holds its locks until it finishes, then two transactions with conflicting operations (one writes what the other reads) can't interleave. The second one is forced to wait until the first commits or rolls back.

---

## Lock compatibility matrix

Not all locks conflict. Two transactions reading the same key is fine — they can both hold S-locks simultaneously. The problem only arises when at least one of them writes.

```
         Requester:  S        X
Holder:
    S              ✓ OK     ✗ WAIT
    X              ✗ WAIT   ✗ WAIT
```

S + S = compatible. Two readers can coexist.
S + X = conflict. A reader and a writer must be serialized.
X + X = conflict. Two writers must be serialized.

This table is enforced in `LockManager::can_grant()`. Before granting any lock, we check whether the current holder(s) are compatible with the request.

**Lock upgrade**: a transaction that already holds an S-lock on a key and then issues a write is upgrading S → X. This is allowed if no other transaction currently holds an S-lock on the same key. It's denied (wait) if another transaction shares the S-lock — upgrading over another reader would give you exclusive access while someone else is still reading, which violates isolation.

---

## The lock manager implementation

`LockManager` maintains two data structures protected by a single `std::mutex`:

1. **lock_table_**: maps key string → `LockState` (who currently holds S and/or X locks)
2. **txn_locks_**: maps txn_id → list of keys it holds (for efficient release_all)

A `std::condition_variable` is used for blocking: when a lock can't be granted, the requesting thread calls `cv_.wait_until(ul, deadline)`. When any lock is released, `release_all` calls `cv_.notify_all()` to wake all waiters.

One subtlety that ASan caught immediately: you cannot hold a `LockState&` reference across a condition variable wait. Here's why: `release_all` erases entries from `lock_table_` when a key's lock state becomes empty. If a waiter thread holds a reference to `LockState&` from before the wait, and the holder releases and erases that entry, the waiter wakes up with a dangling reference — a use-after-free.

The fix: re-look up the key from `lock_table_` on every iteration of the wait loop. The reference is only valid within a single hold of the mutex.

---

## Deadlock detection via timeouts

Deadlock is mathematically unavoidable with 2PL. The classic scenario:

```
Transaction A: holds X("account_a"), waiting for X("account_b")
Transaction B: holds X("account_b"), waiting for X("account_a")
```

Both are waiting for something the other holds. Without intervention, they'd wait forever.

There are two standard approaches:

**Wait-for graph**: maintain a directed graph where an edge A→B means "A is waiting for a lock held by B." A cycle in this graph means deadlock. Detect the cycle and abort one transaction.

**Timeout**: if a lock wait exceeds a threshold (we use 200ms), assume deadlock and abort the waiting transaction.

We use timeouts. The tradeoff: occasionally a slow-but-not-deadlocked transaction gets aborted. But it's far simpler to implement correctly. A wait-for graph adds significant complexity — you have to maintain it incrementally as transactions acquire and release locks, detect cycles efficiently, and choose the right victim to abort. Timeouts give the same correctness guarantee (deadlocks always resolve) with one conditional and no graph maintenance.

When `DeadlockException` is thrown, the caller is expected to roll back the transaction and retry. Our stress test (`TxnStress.ConcurrentIncrements`) demonstrates this pattern:

```cpp
for (;;) {
    try {
        auto txn = db->Begin();
        auto v = txn->Get("counter");
        txn->Put("counter", std::to_string(std::stoi(*v) + 1));
        txn->Commit();
        break;  // success
    } catch (const DeadlockException&) {
        // retry
    }
}
```

---

## The coarse database mutex

The `LockManager` provides logical isolation between transactions — it ensures that the operations of committed transactions are serializable. But the B+Tree and BufferPool are not internally thread-safe. They were designed and tested single-threaded.

Rather than adding fine-grained locking inside every B+Tree operation (which would be Milestone 4 in size by itself), we add a single `std::mutex db_mu_` at the `DB` level. Every call to `btree_.get()`, `btree_.insert()`, or `btree_.remove()` is made under this mutex.

The two-mutex architecture:
- `LockManager`'s internal mutex: protects the lock table, held only briefly
- `DB::db_mu_`: held for the duration of each B+Tree operation

This means two transactions won't physically execute B+Tree operations concurrently — only one is in the tree at a time. But logical isolation is still key-level, not table-level: a transaction blocked on `lm_.lock()` waiting for a key-level lock does not hold `db_mu_`. Once the key-level lock is granted, it acquires `db_mu_` briefly to do the operation, then releases it. Other transactions doing operations on different keys can proceed while this one is in the tree.

The result: the lock manager provides correct serializable isolation, and the coarse mutex prevents data corruption inside the B+Tree. In a production system you'd add B+Tree latch crabbing (acquiring and releasing page-level latches as you traverse) to allow true parallel tree traversals — but that's beyond Milestone 4.

---

## What happens at Commit and Rollback

**Commit sequence:**
1. `lm_.append_commit(txn_id_)` — WAL COMMIT record
2. `lm_.flush()` — fsync WAL ← durability point
3. Under `db_mu_`: `pool().flush_all()`, `dm().flush()` — flush pages
4. `lock_mgr_.release_all(txn_id_)` — release all key locks

The lock release happens last, after the data is safely on disk. This is correct: other transactions waiting on those keys are unblocked only after the commit is complete. If we released locks before fsyncing, a waiting transaction could read data that isn't yet durable — a durability violation.

**Rollback sequence:**
1. `undo_all()` — under `db_mu_`, re-apply before-images to B+Tree
2. `lm_.append_abort(txn_id_)` — WAL ABORT record (no fsync needed)
3. `lock_mgr_.release_all(txn_id_)` — release all key locks

Rollback is simpler: no fsync needed because we're discarding changes, and the undo is already complete in memory by the time locks are released.

---

## What the concurrent stress test proves

`TxnStress.ConcurrentIncrements` runs 4 threads, each doing 10 read-modify-write increments on a single counter key, with retry on DeadlockException.

Without the lock manager, this test would fail with a count less than 40 — some increments would be lost updates. With 2PL, every increment is serialized at the key level. The counter ends up at exactly 40.

The stress test doesn't just verify the final value — it proves that:
1. The lock manager correctly serializes conflicting operations
2. `DeadlockException` is catchable and the retry pattern works
3. The WAL + lock manager combination is correct under concurrent commit+rollback

---

## What's left: Milestone 5

The database now satisfies all four ACID properties:
- **Atomicity**: WAL ensures all-or-nothing commits
- **Consistency**: B+Tree invariants are maintained across all operations
- **Isolation**: 2PL prevents anomalies between concurrent transactions
- **Durability**: fsync on commit ensures committed data survives crashes

Milestone 5 is integration and hardening:
- A complete end-to-end integration test covering all four properties together
- Extended crash test suite covering concurrent crash scenarios
- A clean `DB::Close()` API that flushes and checkpoints cleanly
- Performance baseline numbers
- Documentation pass on the full public API

After Milestone 5, Phase 1 (the embedded library) is complete. Phase 2 adds a TCP server so the database can serve network clients.

---

*Next: [progress5.md](progress5.md) — integration, crash hardening, and Phase 1 complete (coming after Milestone 5)*
