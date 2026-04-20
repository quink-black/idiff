#ifndef IDIFF_PLATFORM_UTF8_H
#define IDIFF_PLATFORM_UTF8_H

// Cross-platform file I/O helpers that handle non-ASCII (e.g. Chinese)
// filenames correctly on every OS.  On Windows the CRT fopen() uses the
// local code page rather than UTF-8, so we go through the wide-char Win32
// API instead.  On non-Windows platforms UTF-8 paths work natively.

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace idiff {
namespace platform {

#ifdef _WIN32

inline std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
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
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(h, &file_size) ||
        file_size.QuadPart > static_cast<LONGLONG>(INT_MAX)) {
        CloseHandle(h);
        return {};
    }

    std::vector<uint8_t> buf(static_cast<size_t>(file_size.QuadPart));
    DWORD bytes_read = 0;
    if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()),
                  &bytes_read, nullptr)) {
        CloseHandle(h);
        return {};
    }
    CloseHandle(h);
    buf.resize(bytes_read);
    return buf;
#else
    // POSIX: fopen with UTF-8 paths works natively on Linux/macOS.
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return {}; }
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    size_t n = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    buf.resize(n);
    return buf;
#endif
}

// Write a byte buffer to a file, creating or overwriting it.  Returns
// true on success.
inline bool write_file_binary(const std::string& path,
                              const uint8_t* data, size_t size) {
#ifdef _WIN32
    auto wide = utf8_to_wide(path);
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD bytes_written = 0;
    bool ok = WriteFile(h, data, static_cast<DWORD>(size),
                        &bytes_written, nullptr) != 0 &&
              bytes_written == size;
    CloseHandle(h);
    return ok;
#else
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = fwrite(data, 1, size, f);
    fclose(f);
    return n == size;
#endif
}

} // namespace platform
} // namespace idiff

#endif // IDIFF_PLATFORM_UTF8_H
