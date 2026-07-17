# progress1.md: What Even Is a Database?

*This is the first entry in a series of notes I'm keeping as I build KeyVDB, an ACID-compliant embedded key-value store in C++. I'm writing these as if I'm explaining things to another student who also has no background in database internals, because that's who I was when I started. If something seems over-explained, that's intentional.*

---

## What made me want to build this

I wanted to build something that's actually hard. Not hard in a "the requirements were confusing" way, but hard in the sense that it involves real computer science: data structures, systems programming, concurrency, crash recovery. The kind of thing where if you get it wrong, data gets silently corrupted and you might not notice for weeks.

A key-value database is one of those things. It's also one of those things that sounds simple until you try to build it correctly. "Just store keys and values" — okay, but what happens if the process crashes halfway through writing? What if two threads try to write the same key at the same time? What if the disk is full? What if you write a value and the OS says it's saved, but the power goes out before it actually hits the platters?

These are the kinds of questions this project is about. The goal isn't a toy. The goal is something I can point to and say: this has real durability guarantees, real transaction semantics, and I can prove it by crashing it on purpose and watching it recover correctly.

---

## What is a database, actually?

This seems like a silly question but it's worth answering from scratch, because "a database stores data" is technically true but not useful.

Your filesystem stores data. A plain text file stores data. A JSON file stores data. So what does a database add on top of that?

Four things, and they happen to spell ACID, which we'll get to. But in plain terms:

**It makes writes atomic.** If you're doing an operation that involves multiple changes — say, transferring $100 from account A to account B, which means subtracting from A and adding to B — a database guarantees that both changes happen, or neither does. You can't end up with the money subtracted from A but never added to B because the process crashed between the two writes. If you were just writing to two text files, that's exactly the kind of thing that could happen.

**It makes reads consistent.** If you're reading data while someone else is writing it, a database guarantees you don't see half-written garbage. You either see the old state or the new state, not some incoherent mix of both.

**It survives crashes.** If you tell a database "save this," it means it. Not "I've put this in an OS buffer that might or might not hit disk before the power goes out" — it means the data is physically on the storage medium and will still be there when the machine reboots. This is the `fsync` problem, and we'll come back to it in detail.

**It lets multiple clients access it at the same time without corrupting each other's work.** This is the concurrency problem, and it's surprisingly hard to get right.

A plain file gives you none of these guarantees by default. You'd have to implement them yourself. That's exactly what we're doing.

---

## Embedded vs client-server

There are two main shapes a database can take.

**Client-server:** A separate process runs the database engine. Your application talks to it over a network socket — even if both are on the same machine, there's still a protocol, a connection, serialization. PostgreSQL, MySQL, and Redis all work this way. You start the server, your app connects to it, sends queries, gets results back.

**Embedded:** The database code runs inside your application process. There's no separate server, no socket, no protocol. You link the database as a library, call functions directly, and the database is part of your program. SQLite is the most famous example of this. LMDB and RocksDB are others.

KeyVDB is embedded. That means the API we're building is just C++ function calls: `Open()`, `Begin()`, `Get()`, `Put()`, `Commit()`. Your application calls them directly. The database lives in your process.

The tradeoff: embedded databases are simpler to deploy (just link the library, no server to run), but they can only serve one process at a time, and they don't have network access by default. We're going to add that in Phase 2, turning our embedded engine into a network server. But the core of the thing is embedded.

---

## ACID — what it actually means

ACID is an acronym for the four properties that define a "real" database transaction. Every letter has a specific meaning, and they're all necessary. Having three out of four isn't good enough.

### Atomicity: all or nothing

Imagine a bank transfer. You want to move $100 from Alice's account to Bob's. The operation has two steps:
1. Subtract $100 from Alice.
2. Add $100 to Bob.

What happens if the process crashes between step 1 and step 2? Without atomicity, you'd have $100 that's been subtracted from Alice but never added to Bob. It just... vanished. The database is in a state that should never exist.

Atomicity means this can't happen. A transaction either commits completely (both steps happen) or rolls back completely (neither step happens). There is no in-between.

We implement this with a **write-ahead log** (WAL). Before we apply any change to the actual data, we write a record to a log file that says "I'm about to do X." If we crash mid-transaction, we can look at the log on startup and either complete the operation or undo what we partially did.

### Consistency: no invalid states

This one is a bit different from the others. The database enforces rules about what states are valid. In our key-value store, "consistency" mostly means things like: a key either exists or it doesn't, no key appears twice, every page that's supposed to be part of the tree actually is.

The consistency guarantee is really a combination of the other three. If you have atomicity (no half-writes), isolation (no concurrent interference), and durability (no data loss), then invalid states can't sneak in. Consistency is the outcome; the others are the mechanism.

In more complex databases, consistency also means things like referential integrity (a foreign key must point to a row that exists). We don't have foreign keys, but we still enforce structural invariants on our data pages and tree structure.

### Isolation: transactions don't see each other's mess

Imagine two transactions running concurrently. Transaction 1 is in the middle of moving $100 from Alice to Bob — it's subtracted from Alice but hasn't added to Bob yet. Transaction 2 wants to read both balances to calculate the total money in the system. If Transaction 2 reads right in the middle of Transaction 1's operation, it might see Alice's account with $100 missing and Bob's account without the $100 yet. The total would be wrong.

Isolation means Transaction 2 either sees the state before Transaction 1 started, or the state after it finished. It never sees the intermediate half-committed state.

We implement this with **two-phase locking** (2PL). Before a transaction reads or writes a piece of data, it acquires a lock. It holds all locks until the transaction commits or rolls back. This prevents two transactions from reading/writing the same data at the same time in a way that would produce incorrect results.

There are other approaches — notably MVCC (Multi-Version Concurrency Control), which is what PostgreSQL uses. We're using 2PL because it's simpler to implement correctly. The tradeoff: 2PL blocks more (readers block writers), while MVCC lets readers and writers proceed concurrently by keeping multiple versions of each record. We'll cover this properly in the transaction manager milestone.

### Durability: committed means committed

When a transaction commits and you get a success response, that data is safe. Not "probably safe" or "safe unless the power goes out in the next 0.1 seconds" — actually safe. The data is on the storage medium and will survive a crash, power outage, or process kill.

This is harder to guarantee than it sounds. When you call `write()` on Linux, the OS typically buffers the data in memory and flushes it to disk later. Your `write()` call returns success before the data is physically on disk. If the power goes out between your `write()` call returning and the OS actually flushing to disk, the data is gone.

The only way to actually guarantee the data is on disk is to call `fsync()`. This tells the OS: "flush everything, and don't return until you've confirmed the storage device has it." It's slow — often 5-10 milliseconds per call, which doesn't sound like much until you're doing it on every commit. But it's the only real durability guarantee.

We call `fsync()` on the WAL after every commit. Not on the main data file — on the log. The WAL is the source of truth during recovery. As long as the log is safely on disk, we can reconstruct the data even if the main file is partially written.

---

## What a "page" is and why everything is in pages

The word "page" comes up constantly in database internals and it needs a clear definition because it means slightly different things in different contexts.

In the OS/CPU context, a "page" is the unit of virtual memory — typically 4096 bytes (4KB). The OS allocates memory in pages, hardware memory protection works at page granularity, and `mmap` works with pages.

In the database context, a "page" is the unit of disk I/O — the smallest chunk we read or write at once. We're using the same size: 4096 bytes.

Why 4KB? A few reasons that all point in the same direction:

1. **Disk sectors.** Traditional hard drives have 512-byte sectors. SSDs typically use 4KB blocks internally. Reading or writing less than one sector/block requires a read-modify-write cycle, which is expensive. Reading or writing exactly one block is a single operation.

2. **OS alignment.** The OS manages memory in 4KB pages. When we do a `pread` or `pwrite` on a 4KB-aligned offset, the OS can often avoid extra copies.

3. **B+Tree fan-out.** We're going to build a B+Tree on top of these pages. The fan-out (how many children each internal node has) determines how many disk reads you need to find a key. Larger pages = more keys per node = higher fan-out = fewer disk reads. 4KB is a good balance between "fits in memory" and "high enough fan-out."

4. **It's the standard.** PostgreSQL uses 8KB pages. SQLite uses 4KB by default. LMDB uses 4KB. We're not inventing something new here; we're matching the ecosystem.

Every higher layer in KeyVDB — the B+Tree, the WAL — talks to the storage manager, and the storage manager deals in pages. Nothing reads or writes arbitrary byte ranges to the file. It's always a full page, at a page-aligned offset.

---

## What we built in Milestone 0

Milestone 0 is about foundations, not functionality. Here's what exists now:

**CMakeLists.txt** is the build system. It does three important things beyond the basics:

1. `-fsanitize=address,undefined` is permanently on. AddressSanitizer (ASan) catches memory bugs at runtime: heap buffer overflows, use-after-free, stack overflows. UndefinedBehaviorSanitizer (UBSan) catches things like signed integer overflow, dereferencing null pointers, and calling functions with the wrong type. We turned these on from commit zero so that every bug we write is caught immediately, not weeks later as a mysterious segfault.

2. `-fno-omit-frame-pointer` and `-g` keep debug info in the binary so that when ASan catches something, the stack trace points at actual lines of code.

3. GTest is pulled in via CMake's FetchContent, which means the test framework is hermetic — no system-level installation required, no version conflicts.

**Directory structure:**

```
KeyVDB/
├── CMakeLists.txt
├── README.md
├── crash_test.sh
├── docs/
│   ├── progress1.md   ← this file
│   └── interview.md
├── src/
│   └── main.cpp
├── tests/
│   └── CMakeLists.txt
└── .gitignore
```

**crash_test.sh** exists and is runnable. Right now it verifies the build works and the binary runs. It has the crash scenario code stubbed out, ready to be uncommented as the storage manager and WAL are built. The reason it exists now rather than later is that building it early forces us to design the modules with testability in mind. If crash testing is an afterthought, you end up with code that's hard to inject crash points into.

---

## Why POSIX I/O instead of `fstream`

The spec says to use `open`, `pread`, `pwrite`, and `fsync` — raw POSIX calls — instead of C++ streams. This deserves an explanation.

C++ `fstream` is a buffered abstraction. When you write to a `fstream`, the bytes go into a userspace buffer. The library flushes them to the OS at some point, and the OS may buffer them further before writing to disk. You have limited control over when data actually hits the storage device.

`fstream` also doesn't have a direct equivalent of `fsync`. You can call `flush()`, which flushes the userspace buffer to the OS, but that doesn't mean it's on disk — it just means the OS has it. Getting data onto disk requires `fsync()`, which is a system call, not a C++ stream method.

By using `pread` and `pwrite` directly:
- We control exactly which bytes go to which offset in the file.
- We control exactly when `fsync` is called and on which file descriptor.
- There's no userspace buffering layer between us and the OS.
- We can use `O_DIRECT` in the future if we want to bypass the OS page cache entirely (relevant for durability tuning).

The code is a bit more verbose, but the behavior is explicit. In systems programming, explicit is better than implicit, especially when the thing you're being explicit about is "will my data survive a crash."

---

## What comes next

Milestone 1 is the storage manager. By the end of it, we'll have:
- A `DiskManager` that reads and writes pages to a file using `pread`/`pwrite`.
- A `BufferPool` that caches recently-used pages in memory and handles eviction.
- A `FreeList` that tracks which pages have been deleted and are available for reuse.
- Unit tests for all of the above.
- A real first version of `crash_test.sh` that kills the process mid-write and verifies no corruption.

The B+Tree, WAL, and transaction manager all sit on top of the storage manager. We can't build any of them until this layer is solid.

---

*Next: [progress2.md](progress2.md) — the B+Tree (coming after Milestone 1)*
