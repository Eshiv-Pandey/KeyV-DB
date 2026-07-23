# interview.md — KeyVDB Interview Q&A

*This file grows alongside the project. Every time a new concept gets implemented, I add the questions that concept would invite. The goal is to have a running document I can review before a technical interview, covering both this project specifically and the broader topics it touches.*

*Answers are written the way I'd actually want to say them out loud, not as textbook definitions. Short where short works, detailed where the interviewer is clearly digging.*

---

## About this project

### Why did you build a database from scratch?

Honestly, I wanted something I could really talk about. A lot of projects people put on their resume are either tutorials dressed up, or things where you're mostly gluing libraries together. I wanted to build something where I had to understand every layer — where if something breaks, I can't just blame the library, because I wrote the library.

I also wanted to learn database internals specifically. The way databases handle concurrency, crash recovery, and durability are some of the most interesting problems in systems programming. The only way to really understand how a B+Tree split works, or what a write-ahead log actually does under the hood, is to implement it. Reading about it gets you 40% of the way there. Building it gets you the rest.

### Why C++?

Because the problems I wanted to tackle — memory management, POSIX I/O, concurrency with real locks — are the kinds of problems where C++ gives you enough control to do things right without fighting the language. I wanted to use RAII and smart pointers, manage a fixed-size buffer pool with precise control over when things get evicted and flushed, and call `fsync` and `pread` directly. C++ lets me do all of that while still being a reasonably modern, structured language.

I also wanted the practice. The kind of C++ you write when you're building a systems project — smart pointers, move semantics, concurrency primitives — is the kind that actually comes up in interviews for systems roles.

### Why not just use SQLite or RocksDB?

Because my goal was to learn how databases work, not to learn how to use a database. If I used SQLite, I'd understand the SQLite API. If I built from scratch, I'd understand B+Trees, write-ahead logging, two-phase locking, buffer pools, and crash recovery. Those are the concepts that transfer to understanding any database system, not just the one I'm using.

Also, there's a specific kind of confidence that comes from having built something like this. When someone asks "what happens when a crash occurs mid-commit?" I can tell them exactly what happens in my implementation, line by line, because I wrote every line.

### Walk me through what happens when you call `Put("key", "value")`.

This is a good one to answer in phases:

First, the transaction needs to hold an exclusive lock on this key. The lock manager checks if anyone else has a shared or exclusive lock on "key." If so, we wait (or time out if we're in a deadlock). If not, we grant the lock.

Once we have the lock, we write a WAL record. The record says: "Transaction X wants to write value Y to key K, and the old value was Z." We write this to the WAL file and — critically — do not yet change the actual data.

Then we go through the B+Tree to find where this key lives. We ask the buffer pool for the relevant pages, traversing from the root down to the leaf. If the tree doesn't have this key, we insert it. If it does, we update the value. Either way, we're modifying a page in the buffer pool — it's marked dirty but not yet flushed to disk.

At this point, the Put is logically done. The change is in memory and in the WAL. It's not on disk yet.

The data only hits disk when you call `Commit()`. That triggers an `fsync` on the WAL file (so the log record is durably on disk), and then the buffer pool flushes the dirty pages to the main database file.

### What happens when `Commit()` is called?

`Commit()` does three things in order:

1. Appends a COMMIT record to the WAL.
2. Calls `fsync` on the WAL file. This is the point of no return — after this, even if everything else crashes, the recovery manager will be able to replay this transaction.
3. Releases all locks the transaction was holding, which unblocks any transactions that were waiting on those keys.

The data pages might not be flushed to disk immediately after commit. That's okay, because as long as the WAL is flushed, recovery can replay the writes during the redo phase on startup. The WAL is the source of truth.

### How does crash recovery actually work?

On startup, before accepting any operations, the recovery manager reads through the WAL from beginning to end. It's doing three things in sequence — this is based on a simplified version of the ARIES recovery algorithm:

**Analysis phase:** Scan the WAL to figure out which transactions were active at the time of the crash. A transaction is "active" if we saw a BEGIN record but no COMMIT or ABORT record.

**Redo phase:** Replay all the WRITE records in order, regardless of whether the transaction committed or not. This might seem wrong at first — why replay uncommitted writes? — but the idea is to get the database to the exact state it was in at the crash point, so the undo phase can then cleanly remove the uncommitted changes.

**Undo phase:** For every transaction that was still active at the crash (no COMMIT), replay its WRITE records in reverse order, applying the "before images" — the values that existed before the transaction made its changes. This rolls back the uncommitted work.

After recovery, the database is in a consistent state: all committed transactions are fully applied, all uncommitted ones are fully reversed.

### What was the hardest part to get right?

*(This will have a real answer once the project is further along. Placeholder for now — will fill in during Milestone 3 or 4.)*

Probably the WAL recovery logic, specifically the redo/undo ordering. It's conceptually straightforward but the edge cases are subtle — what if the same key was modified by three different transactions that all partially committed? What if a page was written to disk during the crash but the WAL record for it wasn't? Getting the crash matrix right (testing all the different crash points and verifying the correct outcome for each) took careful thought.

---

## Storage manager

### Walk me through the KeyVDB storage stack — what are the layers and how do they interact?

Three layers, built in dependency order.

**DiskManager** is the only component that talks to the filesystem. Everything else talks in page IDs. DiskManager translates "give me page 7" into a `pread` at offset 7 × 4096 in the file. It owns the file descriptor, and it's the only place `fsync` is called.

**FreeList** lives on page 0 and tracks which page IDs have been freed and are available for reuse. When a B+Tree node is deleted after a merge, its page ID goes here. The next allocation checks FreeList first, before extending the file. FreeList talks directly to DiskManager — not to the BufferPool — to avoid a circular dependency and to ensure page 0 can never be accidentally evicted while it's being mutated.

**BufferPool** is the in-memory cache. It holds up to 64 page frames in RAM. Every B+Tree operation goes through the pool: `fetch_page` pins a page (loading from disk on a cache miss), the caller reads or modifies it, then `unpin_page(id, dirty)` releases the pin. When all frames are in use, the pool evicts the least recently used unpinned frame. If the evicted frame is dirty, it's written to disk before being overwritten.

Construction order is DiskManager, then FreeList (calls `load()` to read page 0), then BufferPool. Destruction is reversed.

### What is a pin count and why does it matter?

Pin count is the number of active users of a page frame. When you call `fetch_page`, the count goes up. When you call `unpin_page`, it goes down. A frame with pin count > 0 cannot be evicted.

Without pin counts, the buffer pool would have no way to know whether a caller is still using a frame before overwriting it. If the pool evicted a frame mid-use, the caller's `Page*` pointer would point at garbage. Pin counts make this safe: a page you've fetched will stay resident until you unpin it, no matter how many other pages are requested.

The discipline we enforce: every `fetch_page` is matched by exactly one `unpin_page`. Missing an unpin is a pin leak — that frame is never evicted, the pool gradually runs out of frames, and eventually `new_page` returns nullptr. Forgetting a pin is subtle and doesn't crash immediately, which is why every call site has an explicit comment explaining the dirty/clean choice.

### Why does eviction need to flush dirty pages? Isn't that what Commit() is for?

`Commit()` handles crash safety — it ensures that committed data survives a crash by fsyncing the WAL. But flushing dirty pages on eviction is about *normal-operation data loss prevention*, which is a separate concern.

Imagine you call `unpin_page(id, dirty=true)`. You've told the pool "this page has been modified." Later, under memory pressure, the pool decides to evict that frame. If it doesn't flush the dirty frame before overwriting it, your writes are silently discarded — no crash, no error, just gone data. `crash_test.sh` wouldn't catch this because it tests crash scenarios, not eviction scenarios.

So the rule is: dirty eviction must be identical to an explicit `flush_page()` call. If a frame is dirty when evicted, it goes to disk, full stop. This is what `evict_frame()` does in the code.

### How does reopening an existing database work? What would break if num_pages started at 0?

On open, DiskManager does `lseek(fd, 0, SEEK_END)` and divides by PAGE_SIZE to compute the actual number of pages in the file. This is the authoritative count.

If `num_pages_` were reset to 0 on reopen, the next `allocate_page()` call would return page ID 0 and start writing at offset 0 — directly overwriting the FreeList header on page 0, and then pages 1, 2, etc. All existing data would be silently clobbered. The test `DiskManagerTest::PersistAcrossReopen` specifically verifies that reopening returns the correct page count.

---

## B+Tree internals

### Explain the slotted page layout — why not just store cells sequentially?

Sequential storage is simple but brittle: if you want to delete a cell or insert one in the middle, you have to shift every byte after it. For variable-length keys, that's expensive and fragmentation-prone.

Slotted pages solve this with indirection. The page has two regions: a slot array that grows from the front, and a cell heap that grows from the back. The slot array contains small fixed-size offsets (2 bytes each) that point into the cell heap. When you want cell 5, you look up `slots[5]` to get the byte offset, then read the cell bytes at that offset.

Deleting a cell: just remove its slot entry and shift the remaining slots. The cell bytes become "dead space" in the heap — fragmented but harmless. When fragmentation blocks a new insertion, `compact()` rebuilds the heap from scratch, packing all live cells tightly.

Inserting: carve space from the top of the heap (subtract the cell size from `cell_end`), write the bytes, then add a slot entry at the right sorted position and shift the others right.

Fullness is checked by free space, not key count: `free_space = cell_end - free_end`. We need at least `sizeof(slot_offset_t) + payload_bytes` free bytes to insert a new cell. This handles variable-length keys correctly.

### What's the difference between a leaf split and an internal split?

**Leaf split (copy-up):** The separator key is *copied* to the parent — it stays in the right leaf. This is necessary because leaf nodes hold all the actual data. If we removed the separator from the leaf, we'd lose a key.

**Internal split (push-up):** The median key is *promoted* to the parent and removed from both halves. Internal nodes only hold routing keys — they don't need to hold every key. The promoted key acts as a boundary between the two new halves in the parent.

The name "B+Tree" (as opposed to "B-Tree") specifically refers to this distinction: all actual data lives in the leaves, and internal nodes are purely for routing.

### How does find_leaf work, and why does it unpin internal nodes immediately?

`find_leaf` starts at the root and descends by binary searching each internal node for the right child pointer. When it finds the child pointer, it saves the page ID in a `breadcrumbs` vector, unpins the internal page (dirty=false, it was read-only), and moves to the child.

The key constraint: only the final leaf is returned still pinned. Every intermediate internal node is unpinned before we descend.

Why? The buffer pool has 64 frames. If we held every node on the path to a leaf pinned simultaneously, a tree 64 levels deep would exhaust the pool before we could even read the leaf. Even at typical depths (3–4 levels for millions of keys), keeping all traversed pages pinned wastes frames that other concurrent operations (in Milestone 4) will need.

The breadcrumbs vector stores re-fetchable page IDs, not live `Page*` pointers. When `insert_into_parent` needs to go back up the tree, it re-fetches each parent by ID.

### What happens when a split propagates all the way to the root?

When the root splits, there's no parent to receive the separator. Instead:

1. Allocate a new page.
2. Initialise it as an internal node with `parent_id = INVALID_PAGE_ID`.
3. Set its `first_child` to the old left half.
4. Insert the separator key with the right half as its right child.
5. Update `root_page_id_` to the new root's page ID.
6. Persist the new root ID to page 0's reserved slot via `FreeList::set_root_page_id()`.
7. Update `parent_id` on both children to point to the new root.

The tree grows one level taller. This is the only way the tree's height increases — it always grows upward from a root split, never from the bottom.

### Walk me through how a merge works and when it's triggered.

After removing a key, if the leaf holds less than half the usable space (by byte count), it's "underflowing." We need to fix it before returning.

First, we check the left sibling. If it has spare capacity (more than half full), we steal its rightmost cell and move it to our leftmost position. Then we update the separator in the parent — the separator between left and us now reflects the new split point.

If the left sibling is also near-minimum (can't spare a key), we check the right sibling. Same idea: steal the leftmost cell from the right, update the separator.

If neither sibling can spare anything, we merge. We absorb the right sibling's cells into ourselves, pull the separator key down from the parent (for internal nodes), and remove the separator and right sibling's pointer from the parent. The right sibling's page is freed back to the FreeList.

Now the parent has one fewer key. If that puts the parent below half-full, we recurse. This can cascade all the way to the root. If the root ends up as an internal node with zero keys and a single child, we collapse: the child becomes the new root, the old root is freed, and the tree shrinks one level.

### Why maintain parent pointers on every page?

The immediate reason is upward propagation: after a split or merge in a child, we need to insert or remove a separator in the parent. We could recover the parent by re-traversing from the root (the breadcrumbs approach we use during traversal), but that requires re-traversing while modifications are in progress.

The deeper reason is Milestone 4. When we add concurrent access, we'll need "latch crabbing" — acquire a child's latch, then release the parent's latch once we know the child is safe. Knowing which node to latch-crab *to* requires knowing the parent ID without re-traversing from the root (which would require holding the root latch the whole time, defeating the purpose of crabbing). Parent pointers stored on each page solve this cleanly.

---

## Write-ahead log and recovery

### What is a write-ahead log and why does "write-ahead" matter?

The WAL is an append-only file where we record every intended change before applying it to the database. "Write-ahead" means the log record is durably written before we touch the B+Tree page. If the process crashes mid-write, the log tells us exactly what was in progress.

Without the WAL, a crash in the middle of a B+Tree split could leave pointers updated on one side but not the other. The tree would be structurally inconsistent and unreadable. With the WAL, we can always recover: replay committed writes, undo uncommitted ones.

### What is an LSN and what is it used for?

LSN stands for Log Sequence Number. It's a monotonically increasing integer assigned to every WAL record. Every record in the log has a unique LSN; later records have higher LSNs.

The LSN has two uses:

1. **Ordering**: LSNs give us a total order over all log records, which is what we need to replay them correctly during recovery.

2. **page_lsn for redo skipping**: each data page stores the LSN of the last WAL record that modified it (`page_lsn` in `PageHeader`). During redo, if a page's `page_lsn` is already >= the record's LSN, that write was already persisted before the crash. We can skip the record. This is the standard ARIES optimisation.

### Walk me through the commit protocol — what happens in order?

1. Log BEGIN (when the transaction starts).
2. For each write: log WRITE(key, before_val, after_val), then apply to the B+Tree in-memory.
3. Log COMMIT.
4. `fsync(WAL file)` — **this is the durability point**. After this call returns, the COMMIT is permanently on disk.
5. Flush dirty B+Tree pages to the data file.

Steps 5 happens after the durability point. If we crash between steps 4 and 5, recovery will redo the writes using the WAL. The key insight: the WAL is the source of truth. The data file is just a performance cache that saves us from replaying the entire WAL on every startup.

### What does recovery look like — the three phases?

**Analysis**: scan the WAL forward. Build the "loser set" — transactions with a BEGIN but no COMMIT and no ABORT. These crashed mid-transaction. Transactions with ABORT are not losers; they were already undone in memory at rollback time.

**Redo**: replay every WRITE record from committed (and loser) transactions in LSN order. Apply the after-values. This gets the B+Tree to the exact state it was in at crash time — all committed writes, all in-progress writes, aborted writes already absent.

**Undo**: for each loser transaction, walk its WRITE records in reverse LSN order and apply before-images. Keys that were inserted are deleted; keys that were updated are restored to their original values.

The order matters: redo first, then undo. You can't undo safely until everything is in the crash-time state, because the before-images in the log describe the state just before each write — not the state you'd find after a partial redo.

### Why does Rollback not need the WAL for correctness?

During a rollback, we have the write-set in memory — every key the transaction touched, along with its before-value. We just apply the before-images directly to the B+Tree. No log read required.

We still write an ABORT record to the WAL. This is important for recovery: if the process crashes between the rollback completing and the next clean shutdown, recovery needs to know not to undo this transaction's writes again. ABORT says "I already undid everything." A missing COMMIT and missing ABORT says "I crashed mid-transaction; please undo me."

### What's the difference between ABORT and a loser transaction?

ABORT means the transaction completed its rollback before the crash. All its writes were reversed in memory, and the reversed state was written to disk as part of normal operation.

A loser transaction had no COMMIT and no ABORT when the crash happened. It was still in progress. Some of its writes may have made it to disk (via dirty eviction from the buffer pool), some may not. Recovery has to redo to get to the crash-time state, then undo to remove the partial work.

### What is a bug you found while building the WAL, and how did you debug it?

When first running `WalIntegration.CommitPersistsKeys`, the test crashed with an assertion in `Node::Node()` — "page type must be LEAF or INTERNAL." The assertion was firing on the very first operation: `btree_.insert()` during redo was calling `find_leaf()`, which fetched the root page and found it had type `DB_HEADER` instead of `BTREE_LEAF`.

Adding a print showed the root page ID was 0, not 1. Page 0 is the FreeList header — a `DB_HEADER` page.

Tracing back: `DB::Open` calls `fl_->root_page_id()` to determine whether this is a fresh database or an existing one. For a fresh database, `root_page_id()` should return `INVALID_PAGE_ID (-1)`. But `FreeList::load()` was initialising page 0 with zero bytes, and `root_page_id()` reads a 4-byte `int32_t` from the tail slot — which was 0, not -1. `DB::Open` saw 0, concluded "existing database, root is at page 0," and tried to open a B+Tree rooted at the DB header page.

The fix was two lines: explicitly write `INVALID_PAGE_ID` to the root slot in `FreeList::load()` when creating a new database. The broader lesson: "zero" and "unset" are different things. When your sentinel value is not zero, you have to write it explicitly — you can't rely on zero-initialisation.

---

## Two-phase locking and concurrency

### What is two-phase locking and why does it give serializability?

2PL has two phases: growing (acquire locks, never release) and shrinking (release all at once at commit/rollback, never acquire after). The strict version we use releases all locks at commit time.

It gives serializability because: if two transactions conflict (one writes what the other reads), one of them must wait for the other to commit before it can proceed. The waiting transaction only sees the committed state — never the in-progress state. The result is equivalent to running them one after the other.

The intuition is a conflict graph argument: if you can't release a lock until you're done, then conflicting transactions must be totally ordered by their commit times. A total order = serial execution.

### What is the lock compatibility matrix?

```
         Requester:  S        X
Holder:
    S              ✓ OK     ✗ WAIT
    X              ✗ WAIT   ✗ WAIT
```

Two shared (read) locks on the same key are compatible — multiple readers can coexist. Any combination involving an exclusive (write) lock requires the requester to wait.

### What is a lock upgrade and when does it deadlock?

A lock upgrade is when a transaction already holding an S-lock on a key needs to promote it to X (e.g., it read a value and now wants to write it).

Upgrade is allowed if the upgrading transaction is the only S-lock holder — it's effectively going from S-alone to X. It deadlocks if another transaction also holds S on the same key: both would need to upgrade, each waiting for the other to release their S-lock.

In our implementation, the upgrade attempt times out after 200ms (DeadlockException) if blocked.

### What's the difference between deadlock detection and deadlock prevention?

**Detection** (wait-for graph): maintain a directed graph where A→B means "A is waiting for a lock held by B." A cycle means deadlock. Detect it and abort one transaction. Accurate but complex — you have to maintain the graph incrementally and run cycle detection efficiently.

**Prevention** (timeouts): if a lock wait exceeds a threshold, assume deadlock and abort. Simple and correct — every actual deadlock eventually resolves when one transaction times out. Downside: occasionally aborts transactions that were slow but not actually deadlocked.

We use timeouts (200ms). For the kind of short-lived transactions a key-value store handles, a 200ms timeout is effectively only triggered by real deadlocks.

### Why is a coarse DB-level mutex used alongside key-level locks?

The LockManager provides logical isolation: it serializes conflicting key-level operations between transactions. But the B+Tree and BufferPool are not internally thread-safe — they use data structures (hash maps, doubly-linked lists, raw pointers) that would corrupt under concurrent access.

Rather than add fine-grained page-level latching to the B+Tree (which is a substantial piece of work on its own), we use a single `db_mu_` held for the duration of each B+Tree operation. Two transactions won't physically run B+Tree code concurrently. But they're still logically isolated at the key level — a transaction waiting on `lm_.lock()` does not hold `db_mu_`, so the tree is free for other operations while it waits.

### Why does Commit release locks last, after fsyncing?

If we released key-level locks before fsyncing the WAL, a waiting transaction could acquire a lock, read the key, and proceed — but the data those reads are based on might not yet be durable. If the original committing transaction's WAL fsync then fails, the first transaction committed its reads based on data that never persisted. That's a durability violation.

The correct order: fsync WAL first (data is durable), flush pages, then release locks. Other transactions are only unblocked after the commit is safe.

### Describe how the concurrent counter stress test works.

Four threads each increment a "counter" key 10 times with a read-modify-write pattern: read the current value, add 1, write it back. Without locking, many increments would be lost updates — two threads reading 5, both writing 6, losing one increment.

With 2PL: `Get("counter")` acquires S-lock, `Put("counter",...)` upgrades to X. All four threads contend for the same X-lock, serializing them completely. Each increment is atomic from the perspective of the others.

The expected final value is 40 (4 threads × 10 increments). The test verifies this. It also exercises the deadlock retry loop: if a `DeadlockException` is thrown mid-transaction, the transaction rolls back and retries from scratch.

---

## Database internals

### What is ACID and why does each letter matter?

**Atomicity** means a transaction either fully completes or fully rolls back. No partial writes. The classic example is a bank transfer: you can't subtract from account A without adding to account B. If the process crashes between those two operations, atomicity guarantees the subtraction gets undone.

**Consistency** means the database only ever moves from one valid state to another. "Valid" is defined by the database's invariants — in our case, things like: every key appears exactly once, every page that's referenced in the tree actually exists. Consistency is enforced at transaction boundaries.

**Isolation** means concurrent transactions don't interfere with each other in ways that produce incorrect results. A transaction running in isolation should see the same result as if it ran alone, even if other transactions are running simultaneously.

**Durability** means committed data stays committed. If the database tells you "transaction committed," that data will still be there after a crash, power outage, or process kill. This is why we call `fsync` — the OS's `write()` buffering alone doesn't give you this guarantee.

### What is a database page?

A page is the unit of I/O. Instead of reading or writing arbitrary byte ranges, the database always reads or writes in fixed-size chunks — 4KB in our case. This aligns with how disk storage actually works (SSDs have 4KB blocks internally) and how the OS manages memory (also 4KB pages). Working in page-sized units avoids partial-block reads and makes the buffer pool straightforward to implement: each slot in the buffer pool holds exactly one page.

### What is a B+Tree and why do databases use it instead of something simpler?

A binary search tree would work in memory, but databases store data on disk. The critical insight is that each node in a binary tree holds one key — so finding a key in a million-record tree might require 20 node lookups, which means 20 disk reads. Each disk read is ~10ms on an HDD, so that's 200ms for one lookup. Not acceptable.

A B+Tree fixes this by having high fan-out — each node can hold hundreds of keys. A 4KB page can fit roughly 200+ (key, pointer) pairs in an internal node. So instead of 20 disk reads to find a key, you might need 3. That's a 7x improvement just from changing the tree structure.

The other thing B+Trees do: all the actual data lives in the leaf nodes, and the leaf nodes are linked in a chain. This makes range scans — "give me all keys between 'apple' and 'banana'" — very efficient. Binary trees don't have this property; a range scan on a BST requires a full traversal.

### What is a write-ahead log?

The WAL is a log file where we record every intended change before we apply it to the main database. "Write-ahead" means we write to the log first — before touching the actual data. If the process crashes mid-operation, we can look at the log on restart and figure out what was in progress and what needs to be undone.

The WAL solves the "half-write" problem. Without it, a crash in the middle of a B+Tree split could leave the tree in a corrupted state — some pointers updated, others not. With the WAL, we can always recover to a consistent state.

### What are isolation levels and what's the difference between them?

Isolation levels are basically a dial between performance and safety. The more isolated transactions are from each other, the safer the data, but the slower the system (because you have to block more).

**Read Uncommitted:** transactions can see each other's uncommitted changes. Basically no isolation. Never do this in production.

**Read Committed:** you only see committed data. No dirty reads, but you might get different results if you read the same row twice in one transaction (non-repeatable reads).

**Repeatable Read:** if you read a row, no one can change it until your transaction ends. But you might still see new rows appear (phantom reads).

**Serializable:** full isolation. Transactions behave as if they ran one after another, even though they're concurrent. This is what we implement with 2PL.

Our implementation gives serializable isolation. It's the strongest guarantee and the safest, but it has the most contention — transactions block each other more.

### What is two-phase locking?

2PL is a protocol for managing locks in a transaction such that transactions are serializable. The two phases are:

**Growing phase:** the transaction acquires locks as needed (reads get shared locks, writes get exclusive locks). It never releases a lock during this phase.

**Shrinking phase:** once the transaction starts releasing locks (at commit or rollback), it can never acquire new locks.

The rule "never acquire a lock after releasing one" is what makes 2PL correct. It prevents a class of anomalies where you release a lock, someone else modifies the data, and then you try to use a value based on the old assumption.

The downside: deadlocks are mathematically unavoidable with 2PL. Two transactions each waiting for a lock the other holds will wait forever unless someone steps in to break the deadlock. Our implementation handles this with timeouts: if you've been waiting for a lock for too long, we assume deadlock and abort your transaction.

### What is MVCC and how is it different from 2PL?

MVCC (Multi-Version Concurrency Control) is the alternative to 2PL used by PostgreSQL and many other production databases. Instead of locking, it keeps multiple versions of each record — one per transaction that modified it. Readers always see the version that was current at the start of their transaction, so they never block writers, and writers never block readers.

The advantage: much better read performance under high concurrency, because reads never have to wait.

The disadvantage: more complex to implement, requires garbage collection to clean up old versions, and still needs some form of locking for write-write conflicts.

We chose 2PL because it's conceptually simpler and maps more naturally to what we're building. The correctness guarantee is easier to reason about: hold all your locks until you're done, and the system is provably serializable. We explicitly traded read-concurrency performance for implementation simplicity.

### What is a deadlock?

A deadlock is when two or more transactions are each waiting for a lock that the other one holds, so neither can make progress. 

Example: Transaction A holds a lock on key "x" and is waiting for a lock on "y." Transaction B holds a lock on "y" and is waiting for a lock on "x." Neither can proceed. Without intervention, they'd wait forever.

Deadlocks are an inevitable consequence of 2PL — any system that uses blocking locks can deadlock if two transactions acquire locks in conflicting orders. There are two standard ways to handle it:

**Deadlock detection:** maintain a "wait-for graph" where each node is a transaction and each edge means "transaction A is waiting for a lock held by transaction B." If there's a cycle in this graph, there's a deadlock. Detect the cycle and abort one of the transactions.

**Deadlock prevention:** use timeouts. If a lock wait exceeds some threshold (say, 100ms), assume deadlock and abort the waiting transaction. Simpler to implement, slightly more aggressive (you'll occasionally abort a transaction that wasn't actually deadlocked, just slow), but correct.

We use timeouts because they're simpler to implement correctly. A proper wait-for graph can be added later.

---

## C++ specifics

### What is RAII and why does it matter for a database?

RAII stands for "Resource Acquisition Is Initialization." The idea is that every resource (memory, file handle, lock, database page pin) is tied to the lifetime of an object. When the object goes out of scope, its destructor automatically releases the resource.

For a database this is critical. If we forget to unpin a buffer pool page, that page can never be evicted, and eventually the pool runs out of frames. If we forget to release a lock, that transaction is stuck forever. If we forget to close a file descriptor, we leak OS resources.

With RAII, we wrap pages in `PinGuard` objects, locks in `LockGuard` objects, and transactions in `Transaction` objects. The destructor handles cleanup. If an exception is thrown or a function returns early, the destructor still runs. No resource leaks, even in error paths.

### What is the difference between `unique_ptr` and `shared_ptr`?

`unique_ptr<T>` means there is exactly one owner. Ownership can be moved (with `std::move`), but it can't be copied. When the `unique_ptr` goes out of scope, the resource is freed. Use this when there's one clear owner.

`shared_ptr<T>` uses reference counting. Multiple pointers can share ownership. The resource is freed when the last `shared_ptr` holding it goes out of scope. Use this when ownership is genuinely shared — multiple objects need the resource to stay alive.

In KeyVDB, buffer pool pages might be held by the pool itself and also pinned by multiple concurrent readers. `shared_ptr` makes sense there. The buffer pool itself is a singleton owned by the `DB` object, so that's a `unique_ptr`.

The cost of `shared_ptr`: the reference count is atomic, which adds overhead on every copy. For frequently-shared objects this matters. Don't use `shared_ptr` by default; prefer `unique_ptr` and only reach for `shared_ptr` when the ownership is genuinely shared.

---

## Systems programming

### Why does `fsync` matter and why is it slow?

When you write data with `write()` or `pwrite()`, the OS doesn't immediately write to the physical storage device. It stores the data in the "page cache" — a region of RAM managed by the OS as a write buffer. The OS flushes this to disk in the background at some point. The reason is performance: disk writes are slow, RAM is fast, and batching writes improves throughput.

But for durability, we need the data to actually be on disk before we tell the user "your transaction is committed." `fsync(fd)` forces the OS to flush all pending writes for that file descriptor to the storage device and blocks until the device confirms it. After `fsync` returns, the data is durably on the physical storage medium.

Why is it slow? Because you're talking to physical hardware. Even an NVMe SSD has microsecond-scale latency. An HDD has millisecond-scale latency (spinning the disk to the right position). Calling `fsync` on every commit directly limits your commit throughput — if each `fsync` takes 5ms, you can do at most 200 commits per second, regardless of how fast your CPU is.

This is a fundamental tradeoff in database design. Solutions include: group commit (batch multiple transactions' `fsync` calls together), async durability (let the user opt out of per-commit `fsync` for higher throughput), or using hardware with a capacitor-backed write cache that claims to persist data even through a power cut (enterprise SSDs often do this). We do simple per-commit `fsync` to start.

### What is a race condition?

A race condition is when the correctness of a program depends on the timing or ordering of operations across threads or processes, and that timing isn't controlled.

In a database context: two threads both read the value of key "counter" (let's say it's 5), both compute counter + 1 = 6, and both write 6 back. The expected result after two increments is 7, but because the reads and writes interleaved, both threads wrote 6. One increment was lost.

The name "race condition" comes from the idea that the two threads are "racing" — the outcome depends on who gets there first, and you can't control that without synchronization.

We prevent race conditions in KeyVDB using the lock manager. Before reading or writing any key, the transaction acquires a lock. Locks are exclusive for writes and shared for reads. Two write transactions can't hold a lock on the same key simultaneously, so the "both read 5, both write 6" scenario can't happen.

### What is the difference between disk I/O and memory access, and why does it matter for database design?

RAM access is roughly 100 nanoseconds. Disk access (even on an NVMe SSD) is roughly 100 microseconds — 1,000 times slower. An HDD is 10 milliseconds — 100,000 times slower than RAM.

This gap is one of the foundational constraints of database design. Everything about how databases structure data — pages, buffer pools, B+Trees, sequential access patterns — is an attempt to minimize disk reads.

The buffer pool is a direct response to this: instead of going to disk for every page access, we keep recently-used pages in memory. As long as the working set fits in the buffer pool, we're operating at RAM speeds. Only on a cache miss do we go to disk.

The B+Tree's high fan-out is also a response: by putting hundreds of keys in a single page, we minimize the number of pages — and therefore disk reads — needed to find a key.

This is why cache misses in production database systems are treated as serious performance events. In-memory operations are cheap; disk operations are expensive by multiple orders of magnitude.

---

---

## System design and performance

### Walk me through the full commit path from Put() to durable data.

1. `Put("k", "v")` acquires an X-lock on "k" from the LockManager. If another transaction holds any lock on "k", we block (or time out with DeadlockException).

2. Under `db_mu_` (the B+Tree mutex), we read the current value of "k" to capture the before-image, then append a WRITE record to the WAL file with (key, before_val, after_val). The WAL write is a `pwrite` — no fsync yet.

3. Still under `db_mu_`, we call `btree_.insert(k, v)` — the change is now in a dirty buffer pool page.

4. `Commit()` appends a COMMIT record to the WAL, then calls `fsync(WAL fd)`. This is the durability point — after this returns, the commit is permanent regardless of what happens next.

5. Under `db_mu_`, `bp_.flush_all()` writes all dirty B+Tree pages to the data file, then `dm_.flush()` fsyncs the data file.

6. `lock_mgr_.release_all(txn_id)` releases all held locks, waking any waiting transactions.

If the process crashes between steps 3 and 4, recovery sees no COMMIT and undoes the write. If it crashes between 4 and 5, recovery redoes the write from the WAL. Either way, the final state is consistent.

### What are the performance bottlenecks in this implementation?

Several deliberate simplifications create bottlenecks that a production system would address:

**Two fsyncs per commit.** We fsync the WAL after writing the COMMIT record, and fsync the data file after flushing dirty pages. That's two disk synchronisation barriers per transaction. Group commit — batching multiple transactions' data into a single fsync — would multiply throughput without sacrificing durability.

**Coarse B+Tree mutex.** A single `std::mutex db_mu_` serialises all B+Tree operations. Two transactions can never traverse or modify the tree concurrently, even if they're working on completely different parts of it. B+Tree latch crabbing (acquiring and releasing page-level latches as you traverse) would allow true parallel tree operations.

**WAL not compacted.** We truncate the WAL entirely on close/reopen. A production WAL would use checkpointing: periodically write a checkpoint record, then only keep WAL records newer than the last checkpoint. This bounds recovery time and WAL file size without truncating everything.

**No read-only transactions.** Every `Begin()` opens a read-write transaction with a WAL BEGIN record. Read-only transactions could skip the WAL entirely and use MVCC snapshot isolation instead of locking, allowing readers to never block writers.

**FreeList fsyncs on every mutation.** Each deleted page triggers a write+fsync to page 0. Batching free-list mutations with the WAL would eliminate these extra fsyncs.

### If you had to handle a million keys, what would break first?

The FreeList would become the first structural limit — it's bounded by page 0's data area (1016 entries). In practice, a million-key database would have very few free pages at any given time (you'd need to delete nearly all keys to overflow this), so it's not an immediate problem.

The buffer pool (64 pages = 256KB) is tiny for a million-key working set. A realistic deployment would increase `BUFFER_POOL_SIZE` significantly. The LRU eviction works correctly at any size; it would just start evicting hot pages, causing more disk reads.

The WAL file size is bounded by the current transaction's writes. A single transaction inserting 1000 keys generates a WAL of ~40KB — perfectly fine. The issue is if someone puts everything in one giant transaction, which is both slow and WAL-heavy.

The coarse B+Tree mutex would become a throughput bottleneck well before any of these structural limits — with 4+ concurrent writers, lock contention dominates.

### What would Phase 2 (TCP server) add and how would it affect the existing code?

Phase 2 wraps the Phase 1 embedded library with a network layer. The core changes:

A TCP server listens on a configurable port. Each client connection gets a thread (or async handler) that reads serialized operation requests and executes them against the DB. The protocol would be something like: 4-byte length prefix + opcode + payload (key/value bytes).

The transaction model maps cleanly: each client connection tracks an open transaction; `BEGIN`/`GET`/`PUT`/`DELETE`/`COMMIT`/`ROLLBACK` map directly to the existing API. The connection thread calls `db->Begin()`, issues operations, and calls `Commit()` or `Rollback()`.

The existing code changes minimally. The LockManager already handles concurrent transactions from multiple threads. The DB-level mutex already protects B+Tree access. The main addition is the network I/O layer on top.

The biggest design question is client failure handling: if a client disconnects mid-transaction, the server must detect it and call `Rollback()`. Without this, locks held by the dead client would block other clients indefinitely. TCP keepalive + connection timeout + explicit rollback on disconnect handle this.

### What's the difference between KeyVDB and something like Redis?

Both are key-value stores. The differences are architectural:

**Redis is in-memory; KeyVDB is disk-based.** Redis stores data in RAM with optional persistence (RDB snapshots or AOF log). KeyVDB stores data on disk with a page cache in RAM. Redis is faster; KeyVDB's working set can exceed available RAM.

**Redis is single-threaded (mostly); KeyVDB is concurrent.** Redis processes commands in a single event loop — no concurrent modifications possible. KeyVDB has actual multi-transaction concurrency with 2PL isolation.

**Redis doesn't provide full ACID.** Redis transactions (`MULTI`/`EXEC`) are atomic in the sense that no other command interleaves, but there's no isolation (you can't read your own uncommitted writes mid-transaction) and durability depends on the persistence mode. KeyVDB provides all four ACID properties.

**Different data model.** Redis supports rich data types (lists, sorted sets, hashes, streams). KeyVDB is a pure key-value store with string keys and values — simpler, but the foundation the richer types would be built on.

*Phase 1 is complete. The full source, test suite, and documentation are at the repository linked in the README.*