#pragma once

// Shared helper for the manual/test drivers below: every one of them used to
// hardcode Unix-style "/tmp/..." paths for scratch files. That happens to
// work on Linux, but on Windows "/tmp/foo" resolves to "<current drive
// root>\tmp\foo" (e.g. "D:\tmp\foo" in CI) -- a directory that doesn't
// exist and is never created for you. std::ofstream/QFile silently fail to
// open a path whose parent directory is missing, which doesn't crash by
// itself, but the read-back that follows throws (file not found), and an
// uncaught exception in these single-main()-function test drivers reaches
// std::terminate() -- which on Windows/MSVC's CRT surfaces as an unhelpful
// "Exit code 0xc0000409" (STATUS_STACK_BUFFER_OVERRUN, the fail-fast code
// abort() routes through there) rather than anything mentioning a missing
// directory. First hit for real in CI -- see docs/DEVELOPMENT.md.
//
// std::filesystem::temp_directory_path() resolves to the right place on
// every platform (TMPDIR/TEMP/TMP/"/tmp" as appropriate) and is guaranteed
// to already exist, sidestepping the whole problem.

#include <filesystem>
#include <string>

namespace ls::test {

// Returns an absolute path for `filename` inside the platform's temp
// directory, as a std::string ready to hand to std::ofstream, QFile,
// cfitsio, etc.
inline std::string tempPath(const std::string& filename) {
    return (std::filesystem::temp_directory_path() / filename).string();
}

} // namespace ls::test
