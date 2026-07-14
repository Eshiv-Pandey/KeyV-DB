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

*This file will keep growing. Next batch of questions will cover the storage manager internals, specific B+Tree edge cases, and the WAL recovery algorithm in more detail.*
