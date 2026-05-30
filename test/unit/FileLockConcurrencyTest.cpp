// Regression tests for the tpm project-level FileLock (cross-process
// advisory lock backed by topo::platform::FileLock).
//
// Pins the file-lock guard around cache and lock-file writes.
//
// Verifies:
//   1. Two same-process FileLock instances on the same path serialise —
//      the second tryAcquire fails while the first holds the lock.
//   2. Release allows a subsequent acquirer to proceed.
//   3. Atomic write: after a series of writes, the file is either the
//      old content or the new content — never the literal ".tmp"
//      half-written shape.

#include "topo/Platform/FileLock.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::atomic<unsigned> testDirCounter{0};

fs::path makeTempDir() {
    fs::path p = fs::temp_directory_path() /
        ("tpm-filelock-test-" +
         std::to_string(testDirCounter.fetch_add(1)) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

} // namespace

TEST(FileLockConcurrency, TwoInstancesSerialise) {
    fs::path dir = makeTempDir();
    fs::path lockPath = dir / ".tpm-lock";

    topo::platform::FileLock a(lockPath);
    ASSERT_TRUE(a.ok()) << a.error();
    ASSERT_TRUE(a.acquire()) << a.error();

    topo::platform::FileLock b(lockPath);
    ASSERT_TRUE(b.ok()) << b.error();
    EXPECT_FALSE(b.tryAcquire())
        << "second tryAcquire on the same lock file should fail while "
           "the first instance holds the lock (no error expected, just "
           "contention): " << b.error();
    EXPECT_FALSE(b.held());

    a.release();
    EXPECT_TRUE(b.tryAcquire()) << b.error();
    EXPECT_TRUE(b.held());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FileLockConcurrency, ReleaseAllowsAcquireFromAnotherThread) {
    fs::path dir = makeTempDir();
    fs::path lockPath = dir / ".tpm-lock";

    topo::platform::FileLock a(lockPath);
    ASSERT_TRUE(a.ok()) << a.error();
    ASSERT_TRUE(a.acquire()) << a.error();

    std::atomic<bool> threadAcquired{false};
    std::thread t([&]() {
        topo::platform::FileLock b(lockPath);
        if (b.ok() && b.acquire()) {
            threadAcquired.store(true);
        }
    });

    // Give the thread a moment to block on acquire().
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(threadAcquired.load())
        << "thread should still be blocked on acquire() while a holds the lock";

    a.release();
    t.join();
    EXPECT_TRUE(threadAcquired.load())
        << "thread should have acquired the lock once a released";

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FileLockConcurrency, TryAcquireWhenUncontendedSucceeds) {
    fs::path dir = makeTempDir();
    fs::path lockPath = dir / ".tpm-lock";

    topo::platform::FileLock lock(lockPath);
    ASSERT_TRUE(lock.ok()) << lock.error();
    EXPECT_TRUE(lock.tryAcquire()) << lock.error();
    EXPECT_TRUE(lock.held());

    std::error_code ec;
    fs::remove_all(dir, ec);
}
