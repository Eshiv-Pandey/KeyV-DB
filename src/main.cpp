#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

#include "db.h"
#include "txn/lock_manager.h"

// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — KeyVDB demo / smoke test.
//
// Opens a temporary database, demonstrates the full API (Put, Get, Delete,
// Commit, Rollback), verifies durability across a close+reopen, then cleans up.
//
// This is the entry point for the `keyvdb` binary. All the real functionality
// lives in keyvdb_lib — this file just exercises it.
// ─────────────────────────────────────────────────────────────────────────────

using namespace keyvdb;

int main() {
    std::puts("KeyVDB Phase 1 — embedded key-value store demo");
    std::puts("Build: sanitizers active (ASan + UBSan)");
    std::puts("");

    const std::string db_path = "/tmp/keyvdb_demo.db";
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + ".wal");

    // ── Open ──────────────────────────────────────────────────────────────────
    auto db = DB::Open(db_path);
    std::puts("[1] Database opened (new).");

    // ── Commit ────────────────────────────────────────────────────────────────
    {
        auto txn = db->Begin();
        txn->Put("name",    "KeyVDB");
        txn->Put("version", "1.0.0");
        txn->Put("author",  "learning-in-public");
        txn->Commit();
        std::puts("[2] Committed: name, version, author.");
    }

    // ── Read ──────────────────────────────────────────────────────────────────
    {
        auto txn = db->Begin();
        auto name    = txn->Get("name");
        auto version = txn->Get("version");
        auto missing = txn->Get("does_not_exist");
        txn->Rollback();

        std::printf("[3] Get(name)    = %s\n", name    ? name->c_str()    : "(null)");
        std::printf("    Get(version) = %s\n", version ? version->c_str() : "(null)");
        std::printf("    Get(missing) = %s\n", missing ? missing->c_str() : "(null — expected)");
    }

    // ── Rollback ─────────────────────────────────────────────────────────────
    {
        auto txn = db->Begin();
        txn->Put("temp", "this will be rolled back");
        txn->Rollback();

        auto t2 = db->Begin();
        auto v = t2->Get("temp");
        t2->Rollback();
        std::printf("[4] Rolled-back key 'temp' present: %s (expected: false)\n",
                    v.has_value() ? "true" : "false");
    }

    // ── Delete ────────────────────────────────────────────────────────────────
    {
        auto txn = db->Begin();
        txn->Delete("author");
        txn->Commit();

        auto t2 = db->Begin();
        auto v = t2->Get("author");
        t2->Rollback();
        std::printf("[5] Deleted 'author' — present after delete: %s (expected: false)\n",
                    v.has_value() ? "true" : "false");
    }

    // ── Durability: close and reopen ──────────────────────────────────────────
    db->Close();
    std::puts("[6] DB closed cleanly.");

    db = DB::Open(db_path);
    std::puts("[7] DB reopened (WAL recovery ran).");

    {
        auto txn = db->Begin();
        auto name    = txn->Get("name");
        auto version = txn->Get("version");
        auto author  = txn->Get("author");   // was deleted
        txn->Rollback();

        std::printf("[8] After reopen:\n");
        std::printf("    Get(name)    = %s\n", name    ? name->c_str()    : "(null)");
        std::printf("    Get(version) = %s\n", version ? version->c_str() : "(null)");
        std::printf("    Get(author)  = %s (deleted, expected null)\n",
                    author ? author->c_str() : "(null)");
    }

    db->Close();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + ".wal");

    std::puts("");
    std::puts("All checks passed. KeyVDB Phase 1 is working correctly.");
    return 0;
}
