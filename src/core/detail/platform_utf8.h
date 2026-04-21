#ifndef IDIFF_PLATFORM_UTF8_H
#define IDIFF_PLATFORM_UTF8_H

// Cross-platform file I/O helpers that handle non-ASCII (e.g. Chinese)
// filenames correctly on every OS.  On Windows the CRT fopen() uses the
// local code page rather than UTF-8, so we go through the wide-char Win32
// API instead.  On non-Windows platforms UTF-8 paths work natively.

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace idiff {
namespace platform {

#ifdef _WIN32

// Convert a UTF-8 string to a UTF-16 std::wstring suitable for Win32
// `*W` APIs.  Returns an empty wstring on failure or empty input so
// every caller can treat the result uniformly with .c_str().
inline std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};

    // Ask for the required buffer size *excluding* the null terminator
    // by passing an explicit length instead of -1.  That removes the
    // footgun of sizing the std::wstring off the terminator-inclusive
    // count and then writing past size() on the second call.
    const int utf8_len = static_cast<int>(utf8.size());
    int wide_len = MultiByteToWideChar(CP_UTF8, 0,
                                       utf8.c_str(), utf8_len,
                                       nullptr, 0);
    if (wide_len <= 0) return {};

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    int written = MultiByteToWideChar(CP_UTF8, 0,
                                      utf8.c_str(), utf8_len,
                                      wide.data(), wide_len);
    if (written <= 0) return {};
    wide.resize(static_cast<size_t>(written));
    return wide;
}

#endif // _WIN32

// Read the entire file into a byte buffer.  Returns an empty vector on
// failure (check !result.empty() rather than trying to distinguish
// "empty file" from "open failure" — zero-length files are not a
// realistic input for image loading).
inline std::vector<uint8_t> read_file_binary(const std::string& path) {
#ifdef _WIN32
    auto wide = utf8_to_wide(path);
    if (wide.empty()) return {};

    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(h, &file_size) || file_size.QuadPart < 0) {
        CloseHandle(h);
        return {};
    }

    // Refuse anything we can't address as a single std::vector on this
    // build, and also cap at a generous upper bound so a bogus file
    // size (from, say, a flaky network share) can't trigger a runaway
    // allocation.
    constexpr uint64_t kMaxReadBytes = 4ull * 1024ull * 1024ull * 1024ull; // 4 GiB
    const uint64_t size64 = static_cast<uint64_t>(file_size.QuadPart);
    if (size64 > kMaxReadBytes ||
        size64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        CloseHandle(h);
        return {};
    }

    std::vector<uint8_t> buf;
    try {
        buf.resize(static_cast<size_t>(size64));
    } catch (const std::bad_alloc&) {
        CloseHandle(h);
        return {};
    }

    // ReadFile takes a DWORD length per call, so loop for files that
    // exceed DWORD_MAX (4 GiB - 1).  Also note: passing a nullptr
    // buffer when size is 0 is explicitly allowed, but we skip the
    // call entirely to keep the contract simple.
    size_t total_read = 0;
    while (total_read < buf.size()) {
        const size_t remaining = buf.size() - total_read;
        const DWORD to_read =
            remaining > static_cast<size_t>(std::numeric_limits<DWORD>::max())
                ? std::numeric_limits<DWORD>::max()
                : static_cast<DWORD>(remaining);
        DWORD bytes_read = 0;
        if (!ReadFile(h, buf.data() + total_read, to_read,
                      &bytes_read, nullptr)) {
            CloseHandle(h);
            return {};
        }
        if (bytes_read == 0) break; // EOF — file was shorter than reported
        total_read += bytes_read;
    }
    CloseHandle(h);
    buf.resize(total_read);
    return buf;
#else
    // POSIX: fopen with UTF-8 paths works natively on Linux/macOS.
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};

    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return {}; }
    long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return {}; }
    if (std::fseek(f, 0, SEEK_SET) != 0) { std::fclose(f); return {}; }

    std::vector<uint8_t> buf;
    try {
        buf.resize(static_cast<size_t>(sz));
    } catch (const std::bad_alloc&) {
        std::fclose(f);
        return {};
    }

    size_t total_read = 0;
    while (total_read < buf.size()) {
        size_t n = std::fread(buf.data() + total_read, 1,
                              buf.size() - total_read, f);
        if (n == 0) break;
        total_read += n;
    }
    std::fclose(f);
    buf.resize(total_read);
    return buf;
#endif
}

// Write a byte buffer to a file, creating or overwriting it.  Returns
// true on success.  `data` may be null iff `size` is 0.
inline bool write_file_binary(const std::string& path,
                              const uint8_t* data, size_t size) {
#ifdef _WIN32
    auto wide = utf8_to_wide(path);
    if (wide.empty()) return false;

    HANDLE h = CreateFileW(wide.c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    // WriteFile takes a DWORD length per call, so loop for buffers that
    // exceed DWORD_MAX.  Also skip the call entirely for zero-byte
    // writes so a (null, 0) caller doesn't trip any defensive asserts.
    size_t total_written = 0;
    while (total_written < size) {
        const size_t remaining = size - total_written;
        const DWORD to_write =
            remaining > static_cast<size_t>(std::numeric_limits<DWORD>::max())
                ? std::numeric_limits<DWORD>::max()
                : static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (!WriteFile(h, data + total_written, to_write, &written, nullptr) ||
            written == 0) {
            CloseHandle(h);
            return false;
        }
        total_written += written;
    }
    CloseHandle(h);
    return total_written == size;
#else
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t total_written = 0;
    while (total_written < size) {
        size_t n = std::fwrite(data + total_written, 1,
                               size - total_written, f);
        if (n == 0) { std::fclose(f); return false; }
        total_written += n;
    }
    std::fclose(f);
    return total_written == size;
#endif
}

} // namespace platform
} // namespace idiff

#endif // IDIFF_PLATFORM_UTF8_H
