# Tests are added here one milestone at a time.
# Each test executable links keyvdb_lib and gtest_main.
#
# Pattern for adding a new test suite:
#
#   add_executable(storage_test storage_test.cpp)
#   target_link_libraries(storage_test PRIVATE keyvdb_lib GTest::gtest_main)
#   add_test(NAME storage_test COMMAND storage_test)

# Milestone 0: no tests yet — the module is empty.
# This file exists so CMake doesn't error on add_subdirectory(tests).

# Milestone 1 (Storage Manager) — uncomment when ready:
# add_executable(storage_test storage_test.cpp)
# target_link_libraries(storage_test PRIVATE keyvdb_lib GTest::gtest_main)
# add_test(NAME storage_test COMMAND storage_test)

# Milestone 2 (B+Tree) — uncomment when ready:
# add_executable(btree_test btree_test.cpp)
# target_link_libraries(btree_test PRIVATE keyvdb_lib GTest::gtest_main)
# add_test(NAME btree_test COMMAND btree_test)

# Milestone 3 (WAL) — uncomment when ready:
# add_executable(wal_test wal_test.cpp)
# target_link_libraries(wal_test PRIVATE keyvdb_lib GTest::gtest_main)
# add_test(NAME wal_test COMMAND wal_test)

# Milestone 4 (Transactions) — uncomment when ready:
# add_executable(txn_test txn_test.cpp)
# target_link_libraries(txn_test PRIVATE keyvdb_lib GTest::gtest_main)
# add_test(NAME txn_test COMMAND txn_test)
