#include <cstdio>

// KeyVDB — an ACID-compliant embedded key-value store.
//
// This file is the entry point for the standalone keyvdb binary.
// Right now it just confirms the build works end-to-end with sanitizers on.
// Real functionality will be wired in once the core modules are built.

int main() {
    std::puts("KeyVDB starting up...");
    std::puts("Build OK. Sanitizers active.");
    return 0;
}
