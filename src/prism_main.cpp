#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <exception>
#include <omp.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "prism_buf.hpp"
#include "prism_precond.hpp"
#include "prism_codecs.hpp"

namespace fs = std::filesystem;

namespace prism {

using codecs::g_fast_mode;

enum class OverwriteMode { AskInteractive, ForceYes, NoClobber };
inline OverwriteMode g_overwrite_mode = OverwriteMode::AskInteractive;
inline bool g_embed_meta = false;
inline bool g_verify_sha256 = false;

inline bool is_stdin_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

inline fs::path win_long_path(const fs::path& p, bool force_prefix = false) {
#ifdef _WIN32
    if (p.empty()) return p;
    std::wstring ws = p.wstring();
    if (ws.size() >= 4 && ws[0] == L'\\' && ws[1] == L'\\' && ws[2] == L'?' && ws[3] == L'\\') {
        for (wchar_t& ch : ws) {
            if (ch == L'/') ch = L'\\';
        }
        while (ws.size() > 7 && ws.back() == L'\\') {
            ws.pop_back();
        }
        return fs::path(ws);
    }
    std::error_code ec;
    fs::path abs_p = fs::absolute(p, ec);
    if (ec) abs_p = p;
    abs_p = abs_p.lexically_normal();
    ws = abs_p.wstring();
    for (wchar_t& ch : ws) {
        if (ch == L'/') ch = L'\\';
    }
    while (ws.size() > 3 && ws.back() == L'\\') {
        ws.pop_back();
    }
    if (ws.size() >= 4 && ws[0] == L'\\' && ws[1] == L'\\' && ws[2] == L'?' && ws[3] == L'\\') {
        return fs::path(ws);
    }
    if (force_prefix || ws.size() >= 240) {
        if (ws.size() >= 2 && ws[0] == L'\\' && ws[1] == L'\\') {
            return fs::path(L"\\\\?\\UNC\\" + ws.substr(2));
        }
        if (ws.size() >= 2 && ws[1] == L':') {
            return fs::path(L"\\\\?\\" + ws);
        }
    }
    return fs::path(ws);
#else
    (void)force_prefix;
    return p;
#endif
}

inline FILE* prism_fopen(const fs::path& p, const char* mode) {
#ifdef _WIN32
    fs::path lp = win_long_path(p);
    std::wstring wmode;
    for (const char* m = mode; m && *m; ++m) {
        wmode.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*m)));
    }
    return _wfopen(lp.wstring().c_str(), wmode.c_str());
#else
    return std::fopen(p.string().c_str(), mode);
#endif
}

inline std::string sanitize_rel_path(std::string rel) {
    size_t pos = 0;
    while (pos < rel.size() && (rel[pos] == '/' || rel[pos] == '\\')) {
        ++pos;
    }
    if (pos > 0) {
        rel.erase(0, pos);
    }
    return rel;
}

inline bool prism_exists(const fs::path& p) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(win_long_path(p).wstring().c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    std::error_code ec;
    return fs::exists(p, ec);
#endif
}

inline bool prism_is_directory(const fs::path& p) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(win_long_path(p).wstring().c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    std::error_code ec;
    return fs::is_directory(p, ec);
#endif
}

inline uint64_t prism_file_size(const fs::path& p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(win_long_path(p).wstring().c_str(), GetFileExInfoStandard, &fad)) {
        throw std::runtime_error("Failed to stat file size: " + p.string());
    }
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | static_cast<uint64_t>(fad.nFileSizeLow);
#else
    return static_cast<uint64_t>(fs::file_size(p));
#endif
}

inline void prism_create_directories(const fs::path& p) {
#ifdef _WIN32
    if (p.empty()) return;
    std::wstring ws = win_long_path(p, true).wstring();
    if (ws.empty()) return;
    DWORD attr = GetFileAttributesW(ws.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (attr & FILE_ATTRIBUTE_DIRECTORY) return;
        throw std::runtime_error("Path exists and is not a directory: " + p.string());
    }
    size_t start = 0;
    if (ws.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        size_t s1 = ws.find(L'\\', 8);
        if (s1 != std::wstring::npos) {
            size_t s2 = ws.find(L'\\', s1 + 1);
            start = (s2 != std::wstring::npos) ? (s2 + 1) : ws.size();
        } else {
            start = ws.size();
        }
    } else if (ws.rfind(L"\\\\?\\", 0) == 0 && ws.size() >= 7 && ws[5] == L':' && ws[6] == L'\\') {
        start = 7;
    } else if (ws.size() >= 3 && ws[1] == L':' && ws[2] == L'\\') {
        start = 3;
    }
    for (size_t pos = start; pos <= ws.size(); ++pos) {
        if (pos == ws.size() || ws[pos] == L'\\') {
            if (pos > start) {
                std::wstring sub = ws.substr(0, pos);
                DWORD sub_attr = GetFileAttributesW(sub.c_str());
                if (sub_attr == INVALID_FILE_ATTRIBUTES) {
                    if (!CreateDirectoryW(sub.c_str(), nullptr)) {
                        DWORD err = GetLastError();
                        if (err != ERROR_ALREADY_EXISTS) {
                            throw std::runtime_error("Failed to create directory: " + p.string());
                        }
                    }
                } else if (!(sub_attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    throw std::runtime_error("Path component is not a directory: " + p.string());
                }
            }
        }
    }
#else
    fs::create_directories(p);
#endif
}

inline bool check_overwrite_path(const fs::path& p) {
    return prism_exists(p);
}

inline void ensure_can_overwrite_list(const std::vector<fs::path>& targets) {
    if (g_overwrite_mode == OverwriteMode::ForceYes) return;

    std::vector<fs::path> existing;
    for (const auto& p : targets) {
        if (check_overwrite_path(p)) {
            existing.push_back(p);
        }
    }
    if (existing.empty()) return;

    if (g_overwrite_mode == OverwriteMode::NoClobber) {
        throw std::runtime_error("Refusing to overwrite existing file(s): " + existing.front().string());
    }

    if (is_stdin_tty()) {
        std::cerr << "[PRISM] Warning: " << existing.size()
                  << " existing file(s) will be overwritten (e.g. " << existing.front().string()
                  << "). Overwrite? [y/N]: " << std::flush;
        std::string resp;
        if (!std::getline(std::cin, resp) || resp.empty() || (resp[0] != 'y' && resp[0] != 'Y')) {
            throw std::runtime_error("Operation cancelled by user (would overwrite existing files).");
        }
    }
}

const char* PRISM_MAGIC = "PRISM";
const uint32_t PRISM_VERSION = 1;

struct ArchiveHeader {
    char magic[4];
    uint32_t version;
    uint32_t flags;
    uint64_t orig_size;
    uint64_t comp_size;
};

struct RawBuf {
    std::unique_ptr<uint8_t[]> ptr;
    size_t size = 0;
    uint8_t* data() { return ptr.get(); }
    const uint8_t* data() const { return ptr.get(); }
};

inline RawBuf read_file_raw(const fs::path& path) {
#ifdef _WIN32
    std::wstring ws = win_long_path(path, true).wstring();
    HANDLE h = CreateFileW(ws.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Failed to open file for reading: " + path.string());
    }
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0) {
        CloseHandle(h);
        throw std::runtime_error("Failed to stat file: " + path.string());
    }
    RawBuf buf;
    buf.size = static_cast<size_t>(li.QuadPart);
    if (buf.size > 0) {
        buf.ptr.reset(new uint8_t[buf.size]);
        size_t got_total = 0;
        while (got_total < buf.size) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(buf.size - got_total, 64ULL * 1024 * 1024));
            DWORD got = 0;
            if (!ReadFile(h, buf.ptr.get() + got_total, chunk, &got, nullptr) || got == 0) {
                CloseHandle(h);
                throw std::runtime_error("Failed to read file: " + path.string());
            }
            got_total += static_cast<size_t>(got);
        }
    }
    CloseHandle(h);
    return buf;
#else
    std::string pstr = path.string();
    FILE* fp = prism_fopen(path, "rb");
    if (!fp) throw std::runtime_error("Failed to open file for reading: " + pstr);
    _fseeki64(fp, 0, SEEK_END);
    long long sz = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    if (sz < 0) { std::fclose(fp); throw std::runtime_error("Failed to stat file: " + pstr); }
    RawBuf buf;
    buf.size = (size_t)sz;
    if (sz > 0) {
        buf.ptr.reset(new uint8_t[buf.size]);
        if (std::fread(buf.ptr.get(), 1, buf.size, fp) != buf.size) {
            std::fclose(fp);
            throw std::runtime_error("Failed to read file: " + pstr);
        }
    }
    std::fclose(fp);
    return buf;
#endif
}

inline ByteVec read_file_bytes(const fs::path& path) {
    std::string pstr = path.string();
    FILE* fp = prism_fopen(path, "rb");
    if (!fp) throw std::runtime_error("Failed to open file for reading: " + pstr);
    std::fseek(fp, 0, SEEK_END);
    long long sz = _ftelli64(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz < 0) { std::fclose(fp); throw std::runtime_error("Failed to stat file: " + pstr); }
    ByteVec buffer((size_t)sz);
    if (sz > 0 && std::fread(buffer.data(), 1, (size_t)sz, fp) != (size_t)sz) {
        std::fclose(fp);
        throw std::runtime_error("Failed to read file: " + pstr);
    }
    std::fclose(fp);
    return buffer;
}

#ifdef _WIN32
inline void win_write_file_direct(const fs::path& path, const uint8_t* data, size_t size) {
    std::wstring ws = win_long_path(path, true).wstring();
    HANDLE h = CreateFileW(
        ws.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Failed to open file for writing: " + path.string());
    }
    struct HandleGuard {
        HANDLE h;
        explicit HandleGuard(HANDLE handle) : h(handle) {}
        ~HandleGuard() {
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    } guard(h);

    constexpr size_t MAX_CHUNK = 64ULL * 1024 * 1024;
    size_t offset = 0;
    while (offset < size) {
        size_t remain = size - offset;
        DWORD to_write = static_cast<DWORD>(std::min(remain, MAX_CHUNK));
        DWORD written = 0;
        if (!WriteFile(h, data + offset, to_write, &written, nullptr) || written == 0) {
            throw std::runtime_error("Failed to write file: " + path.string());
        }
        offset += static_cast<size_t>(written);
    }

    guard.h = INVALID_HANDLE_VALUE;
    if (!CloseHandle(h)) {
        throw std::runtime_error("Failed to close file after writing: " + path.string());
    }
}
#endif

inline void write_file_bytes(const fs::path& path, const uint8_t* data, size_t size) {
    if (path.has_parent_path()) {
        fs::path parent = path.parent_path();
        if (!parent.empty() && !prism_exists(parent)) {
            prism_create_directories(parent);
        }
    }
#ifdef _WIN32
    win_write_file_direct(path, data, size);
#else
    std::string pstr = path.string();
    FILE* fp = prism_fopen(path, "wb");
    if (!fp) throw std::runtime_error("Failed to open file for writing: " + pstr);
    if (size > 0 && std::fwrite(data, 1, size, fp) != size) {
        std::fclose(fp);
        throw std::runtime_error("Failed to write file: " + pstr);
    }
    std::fclose(fp);
#endif
}

#ifdef _WIN32
inline std::string win_wide_to_utf8(const wchar_t* ws) {
    if (!ws || !*ws) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), needed, nullptr, nullptr);
    return out;
}
#endif

inline ByteVec pack_directory(const fs::path& root_dir) {
    struct FileKey {
        fs::path path;
        std::string rel_str;
        uint64_t fsize;
    };
    std::vector<FileKey> files;
#ifdef _WIN32
    std::wstring root_w = win_long_path(root_dir, true).wstring();
    struct DirFrame {
        std::wstring dir_w;
        std::string rel_prefix;
    };
    std::vector<DirFrame> stack;
    stack.push_back(DirFrame{root_w, ""});
    while (!stack.empty()) {
        DirFrame cur = std::move(stack.back());
        stack.pop_back();
        std::wstring pattern = cur.dir_w;
        if (!pattern.empty() && pattern.back() != L'\\') {
            pattern.push_back(L'\\');
        }
        pattern.push_back(L'*');
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES) continue;
            throw std::runtime_error("Failed to iterate directory: " + root_dir.string());
        }
        do {
            const wchar_t* name = fd.cFileName;
            if ((name[0] == L'.' && name[1] == L'\0') ||
                (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0')) {
                continue;
            }
            std::wstring full_w = cur.dir_w;
            if (!full_w.empty() && full_w.back() != L'\\') {
                full_w.push_back(L'\\');
            }
            full_w.append(name);
            std::string name_utf8 = win_wide_to_utf8(name);
            std::string rel = cur.rel_prefix.empty() ? name_utf8 : (cur.rel_prefix + "/" + name_utf8);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    stack.push_back(DirFrame{std::move(full_w), std::move(rel)});
                }
            } else {
                uint64_t fsize = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                                 static_cast<uint64_t>(fd.nFileSizeLow);
                files.push_back(FileKey{fs::path(std::move(full_w)), std::move(rel), fsize});
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
#else
    for (auto& p : fs::recursive_directory_iterator(root_dir)) {
        if (p.is_regular_file()) {
            fs::path fpath = p.path();
            std::string rel_str = fs::relative(fpath, root_dir).generic_string();
            uint64_t fsize = prism_file_size(fpath);
            files.push_back(FileKey{std::move(fpath), std::move(rel_str), fsize});
        }
    }
#endif
    std::sort(files.begin(), files.end(), [](const FileKey& a, const FileKey& b) {
        return a.rel_str < b.rel_str;
    });

    struct PackEntry {
        fs::path fpath;
        std::string rel_str;
        uint64_t fsize;
        size_t hdr_off;
        size_t data_off;
    };

    std::vector<PackEntry> entries;
    entries.reserve(files.size());
    size_t total_size = 8;
    uint64_t total_data_bytes = 0;
    for (const auto& fk : files) {
        if (fk.rel_str.size() > 65535) throw std::runtime_error("File path too long");
        total_data_bytes += fk.fsize;
        size_t hdr_off = total_size;
        size_t data_off = hdr_off + 2 + fk.rel_str.size() + 8;
        total_size = data_off + (size_t)fk.fsize;
        entries.push_back(PackEntry{fk.path, fk.rel_str, fk.fsize, hdr_off, data_off});
    }

    ByteVec packed(total_size);
    uint8_t* base = packed.data();
    std::memcpy(base, "PDIR", 4);
    uint32_t file_count = (uint32_t)entries.size();
    std::memcpy(base + 4, &file_count, 4);

    for (const auto& entry : entries) {
        uint8_t* dst = base + entry.hdr_off;
        uint16_t path_len = (uint16_t)entry.rel_str.size();
        std::memcpy(dst, &path_len, 2);
        dst += 2;
        if (path_len > 0) {
            std::memcpy(dst, entry.rel_str.data(), path_len);
            dst += path_len;
        }
        std::memcpy(dst, &entry.fsize, 8);
    }

    std::atomic<bool> has_err{false};
    std::string err_msg;
    bool emit_pack_progress = codecs::g_emit_progress.load(std::memory_order_relaxed)
                              && files.size() >= 8
                              && total_data_bytes >= 1024ULL * 1024ULL;
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<int> last_pack_pct{5};

    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < (int)entries.size(); ++i) {
        if (has_err.load(std::memory_order_relaxed)) continue;
        const auto& entry = entries[i];
        if (entry.fsize == 0) continue;
#ifdef _WIN32
        std::wstring ws = win_long_path(entry.fpath, true).wstring();
        HANDLE h = CreateFileW(ws.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::string pstr = entry.fpath.string();
            #pragma omp critical
            {
                if (!has_err.exchange(true)) {
                    err_msg = "Failed to open file for reading: " + pstr;
                }
            }
            continue;
        }
        uint8_t* dst_ptr = base + entry.data_off;
        size_t want = (size_t)entry.fsize;
        size_t got_total = 0;
        bool read_ok = true;
        while (got_total < want) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(want - got_total, 64ULL * 1024 * 1024));
            DWORD got = 0;
            if (!ReadFile(h, dst_ptr + got_total, chunk, &got, nullptr) || got == 0) {
                read_ok = false;
                break;
            }
            got_total += static_cast<size_t>(got);
        }
        CloseHandle(h);
        if (!read_ok) {
            std::string pstr = entry.fpath.string();
            #pragma omp critical
            {
                if (!has_err.exchange(true)) {
                    err_msg = "Failed to read file: " + pstr;
                }
            }
            continue;
        }
#else
        std::string pstr = entry.fpath.string();
        FILE* fp = prism_fopen(entry.fpath, "rb");
        if (!fp) {
            #pragma omp critical
            {
                if (!has_err.exchange(true)) {
                    err_msg = "Failed to open file for reading: " + pstr;
                }
            }
            continue;
        }
        size_t want = (size_t)entry.fsize;
        if (std::fread(base + entry.data_off, 1, want, fp) != want) {
            std::fclose(fp);
            #pragma omp critical
            {
                if (!has_err.exchange(true)) {
                    err_msg = "Failed to read file: " + pstr;
                }
            }
            continue;
        }
        std::fclose(fp);
#endif
        if (emit_pack_progress) {
            uint64_t cur_read = bytes_read.fetch_add(entry.fsize, std::memory_order_relaxed) + entry.fsize;
            int pack_pct = 5 + (int)((cur_read * 10ULL) / total_data_bytes);
            if (pack_pct > 15) pack_pct = 15;
            if (pack_pct > last_pack_pct.load(std::memory_order_relaxed)) {
                #pragma omp critical(prism_progress)
                {
                    int cur_last = last_pack_pct.load(std::memory_order_relaxed);
                    if (pack_pct > cur_last) {
                        last_pack_pct.store(pack_pct, std::memory_order_relaxed);
                        std::cout << "[PROGRESS] " << pack_pct << " Packing\n" << std::flush;
                    }
                }
            }
        }
    }

    if (has_err.load()) {
        throw std::runtime_error(err_msg);
    }

    return packed;
}

struct WriteEntry {
    fs::path target_path;
    const uint8_t* data;
    size_t size;
};

inline void write_entries_parallel(const fs::path& out_dir, const std::vector<WriteEntry>& items) {
    if (!items.empty()) {
        std::vector<fs::path> targets;
        targets.reserve(items.size());
        for (const auto& item : items) {
            targets.push_back(item.target_path);
        }
        ensure_can_overwrite_list(targets);
    }

    prism_create_directories(out_dir);

    std::unordered_set<std::string> seen_dirs;
    std::vector<fs::path> unique_dirs;
    unique_dirs.reserve(items.size());
    for (const auto& item : items) {
        if (item.target_path.has_parent_path()) {
            fs::path parent = item.target_path.parent_path();
            if (!parent.empty()) {
                std::string key = parent.generic_string();
                if (seen_dirs.insert(key).second) {
                    unique_dirs.push_back(parent);
                }
            }
        }
    }
    for (const auto& dir : unique_dirs) {
        prism_create_directories(dir);
    }

    std::atomic<bool> has_err{false};
    std::string err_msg;

#ifdef _WIN32
    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < (int)items.size(); ++i) {
        if (has_err.load(std::memory_order_relaxed)) continue;
        const auto& item = items[i];
        try {
            win_write_file_direct(item.target_path, item.data, item.size);
        } catch (const std::exception& e) {
            #pragma omp critical
            {
                if (!has_err.exchange(true)) {
                    err_msg = e.what();
                }
            }
        } catch (...) {
            #pragma omp critical
            {
                if (!has_err.exchange(true)) {
                    err_msg = "Failed to write file: " + item.target_path.string();
                }
            }
        }
    }
#else
    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < (int)items.size(); ++i) {
        if (has_err.load(std::memory_order_relaxed)) continue;
        const auto& item = items[i];
        std::string pstr = item.target_path.string();
        FILE* fp = prism_fopen(item.target_path, "wb");
        if (!fp) {
            #pragma omp critical
            {
                if (!has_err.exchange(true)) {
                    err_msg = "Failed to open file for writing: " + pstr;
                }
            }
            continue;
        }
        if (item.size > 0 && std::fwrite(item.data, 1, item.size, fp) != item.size) {
            std::fclose(fp);
            #pragma omp critical
            {
                if (!has_err.exchange(true)) {
                    err_msg = "Failed to write file: " + pstr;
                }
            }
            continue;
        }
        std::fclose(fp);
    }
#endif

    if (has_err.load()) {
        throw std::runtime_error(err_msg);
    }
}

inline void unpack_directory(const uint8_t* data, size_t size, const fs::path& out_dir) {
    if (size < 8) throw std::runtime_error("Invalid PDIR container: too short");
    if (std::memcmp(data, "PDIR", 4) != 0) throw std::runtime_error("Invalid PDIR container magic");

    uint32_t file_count = 0;
    std::memcpy(&file_count, data + 4, 4);
    size_t offset = 8;

    std::vector<WriteEntry> items;
    items.reserve(file_count);

    for (uint32_t i = 0; i < file_count; ++i) {
        if (offset + 2 > size) throw std::runtime_error("PDIR truncated at path length");
        uint16_t path_len = 0;
        std::memcpy(&path_len, data + offset, 2);
        offset += 2;

        if (offset + path_len + 8 > size) throw std::runtime_error("PDIR truncated at path/size");
        std::string rel_path((const char*)(data + offset), path_len);
        offset += path_len;

        uint64_t fsize = 0;
        std::memcpy(&fsize, data + offset, 8);
        offset += 8;

        if (offset + fsize > size) throw std::runtime_error("PDIR truncated at file payload");
        items.push_back({out_dir / fs::path(sanitize_rel_path(rel_path)), data + offset, (size_t)fsize});
        offset += (size_t)fsize;
    }

    write_entries_parallel(out_dir, items);
}

// Forward declarations
inline ByteVec decode_sub_codec_payload(uint8_t sub_codec_id, const uint8_t* pdata, size_t plen, uint64_t expected_orig);
inline ByteVec decompress_bucket_payload(const uint8_t* data, size_t len, uint64_t orig_size);
inline ByteVec decompress_stream(const uint8_t* data, size_t len, uint64_t orig_size, uint32_t flags);
inline ByteVec try_compress_tar_zip_demux(const uint8_t* transformed, size_t tr_len, const uint8_t* patch_meta, size_t pm_len);
inline ByteVec decompress_tar_zip_demux(const uint8_t* src, size_t len);

inline bool is_predominantly_text(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    if (len <= 24576) {
        return static_cast<double>(precond::count_printable_ascii(data, len)) / static_cast<double>(len) >= 0.78;
    }
    const size_t slice = 8192;
    const size_t offs[3] = { 0, len / 2 - slice / 2, len - slice };
    size_t printable = 0;
    for (int s = 0; s < 3; ++s) {
        printable += precond::count_printable_ascii(data + offs[s], slice);
    }
    return static_cast<double>(printable) / static_cast<double>(slice * 3) >= 0.78;
}

// Unified Bucket Payload Compression with optional per-bucket CDC
inline ByteVec compress_bucket_payload(const uint8_t* ptr, size_t len, uint8_t bucket_id, bool enable_cdc) {
    if (len == 0) return ByteVec();

    size_t cdc_min_len = g_fast_mode ? (16ULL * 1024 * 1024) : (4ULL * 1024 * 1024);
    bool use_cdc = enable_cdc && (len >= cdc_min_len);
    precond::CdcResult cdc_res;
    if (use_cdc) {
        cdc_res = precond::cdc_encode(ptr, len);
    }

    const uint8_t* target_ptr = (use_cdc && cdc_res.applied) ? cdc_res.unique_payload.data() : ptr;
    size_t target_len = (use_cdc && cdc_res.applied) ? cdc_res.unique_payload.size() : len;

    ByteVec payload_comp;
    uint8_t sub_codec_id = 0; // 0: BSC, 1: Stride+BSC, 2: X86, 3: SPARC, 4: ZipPrecomp+LZMA

    switch (bucket_id) {
        case precond::BUCKET_TEXT: {
            if (!g_fast_mode && target_len >= 4ULL * 1024 * 1024) {
                payload_comp = codecs::lrm_bsc_compress(target_ptr, target_len, 3, 15, 72, 0);
                sub_codec_id = (payload_comp.size() >= 4 && std::memcmp(payload_comp.data(), "PLRM", 4) == 0) ? 5 : 0;
            } else {
                sub_codec_id = 0;
                payload_comp = codecs::bsc_parallel_compress(target_ptr, target_len, 3, 15, 72, 0);
            }
            break;
        }
        case precond::BUCKET_BINARY: {
            if (!g_fast_mode && codecs::should_use_lzma2_for_binary(target_ptr, target_len)) {
                sub_codec_id = 5;
                payload_comp = codecs::lrm_lzma2_compress(target_ptr, target_len, 16, 64, 24, 3, 0, 2, 6);
            } else {
                sub_codec_id = 0;
                payload_comp = codecs::bsc_parallel_compress(target_ptr, target_len, 3, 15, 72, 0);
            }
            break;
        }
        case precond::BUCKET_STRIDE: {
            auto stride_res = precond::stride_encode(target_ptr, target_len);
            if (stride_res.applied) {
                sub_codec_id = 1;
                ByteVec s_comp = g_fast_mode
                    ? codecs::lzma_parallel_compress(stride_res.data.data(), stride_res.data.size(), 8, 64, 16, 6, 3, 0, 0, 5)
                    : codecs::lzma2_parallel_compress(stride_res.data.data(), stride_res.data.size(), 16, 64, 24, 0, 3, 0, 0, 6);
                ByteVec s_container;
                s_container.push_back(stride_res.stride);
                const uint8_t* p_mask = (const uint8_t*)&stride_res.delta_mask;
                s_container.insert(s_container.end(), p_mask, p_mask + 8);
                s_container.insert(s_container.end(), s_comp.begin(), s_comp.end());
                payload_comp = std::move(s_container);
            } else {
                sub_codec_id = 0;
                payload_comp = codecs::bsc_parallel_compress(target_ptr, target_len, 3, 15, 72, 0);
            }
            break;
        }
        case precond::BUCKET_EXEC_X86: {
            sub_codec_id = 2;
            payload_comp = codecs::exec_x86_compress(target_ptr, target_len);
            break;
        }
        case precond::BUCKET_EXEC_SPARC: {
            sub_codec_id = 3;
            if (precond::detect_executable(target_ptr, target_len) == precond::EXEC_ARM64) {
                payload_comp = codecs::exec_arm64_compress(target_ptr, target_len);
            } else {
                payload_comp = codecs::exec_sparc_compress(target_ptr, target_len);
            }
            break;
        }
        case precond::BUCKET_TAR_ARCHIVE: {
            precond::ZipPrecompResult zip_res;
            if (!g_fast_mode) {
                zip_res = precond::zip_precomp_encode(target_ptr, target_len, codecs::g_comp_level >= 9);
            }
            if (zip_res.applied) {
                sub_codec_id = 4;
                double zscale = zip_res.transformed.empty()
                    ? 1.0
                    : (static_cast<double>(target_len) / static_cast<double>(zip_res.transformed.size()));
                ByteVec t_comp = is_predominantly_text(zip_res.transformed.data(), zip_res.transformed.size())
                    ? codecs::lrm_bsc_compress(zip_res.transformed.data(), zip_res.transformed.size(), 3, 15, 72, 0, zscale)
                    : codecs::lrm_lzma2_compress(zip_res.transformed.data(), zip_res.transformed.size(), 16, 64, 24, 3, 0, 2, 6, zscale);
                ByteVec p_comp;
                {
                    codecs::SuppressProgress sp;
                    p_comp = codecs::bsc_parallel_compress(zip_res.patch_meta.data(), zip_res.patch_meta.size(), 3, 15, 72, 0);
                }
                ByteVec z_container;
                uint64_t b_orig = (uint64_t)target_len;
                const uint8_t* pb_orig = (const uint8_t*)&b_orig;
                z_container.insert(z_container.end(), pb_orig, pb_orig + 8);
                uint32_t t_sz = (uint32_t)t_comp.size();
                const uint8_t* pt = (const uint8_t*)&t_sz;
                z_container.insert(z_container.end(), pt, pt + 4);
                z_container.insert(z_container.end(), t_comp.begin(), t_comp.end());
                z_container.insert(z_container.end(), p_comp.begin(), p_comp.end());
                payload_comp = std::move(z_container);
            } else {
                sub_codec_id = 0;
                payload_comp = codecs::bsc_parallel_compress(target_ptr, target_len, 3, 15, 72, 0);
            }
            break;
        }
        default: {
            sub_codec_id = 0;
            payload_comp = codecs::bsc_parallel_compress(target_ptr, target_len, 3, 15, 72, 0);
            break;
        }
    }

    ByteVec container;
    container.push_back((use_cdc && cdc_res.applied) ? 1 : 0);
    if (use_cdc && cdc_res.applied) {
        ByteVec table_comp;
        {
            codecs::SuppressProgress sp;
            table_comp = codecs::bsc_parallel_compress(cdc_res.chunk_table.data(), cdc_res.chunk_table.size(), 3, 15, 72, 0);
        }
        uint32_t t_sz = (uint32_t)table_comp.size();
        const uint8_t* pt = (const uint8_t*)&t_sz;
        container.insert(container.end(), pt, pt + 4);
        container.insert(container.end(), table_comp.begin(), table_comp.end());
        uint64_t up_orig = (uint64_t)cdc_res.unique_payload.size();
        const uint8_t* pup = (const uint8_t*)&up_orig;
        container.insert(container.end(), pup, pup + 8);
    }
    container.push_back(sub_codec_id);
    container.insert(container.end(), payload_comp.begin(), payload_comp.end());
    return container;
}

inline ByteVec decode_sub_codec_payload(uint8_t sub_codec_id, const uint8_t* pdata, size_t plen, uint64_t expected_orig) {
    if (sub_codec_id == 0) {
        return codecs::bsc_parallel_decompress(pdata, plen);
    } else if (sub_codec_id == 5) {
        return codecs::lzma_auto_decompress(pdata, plen);
    } else if (sub_codec_id == 1) {
        if (plen < 9) throw std::runtime_error("Invalid stride bucket");
        uint8_t stride_val = pdata[0];
        uint64_t stride_mask = 0;
        std::memcpy(&stride_mask, pdata + 1, 8);
        ByteVec s_raw = codecs::lzma_auto_decompress(pdata + 9, plen - 9);
        ByteVec out;
        if (!precond::stride_decode(s_raw.data(), s_raw.size(), expected_orig, stride_val, stride_mask, out)) {
            throw std::runtime_error("stride_decode failed");
        }
        return out;
    } else if (sub_codec_id == 2) {
        return codecs::exec_x86_decompress(pdata, plen);
    } else if (sub_codec_id == 3) {
        return codecs::exec_sparc_decompress(pdata, plen);
    } else if (sub_codec_id == 4) {
        if (plen < 12) throw std::runtime_error("Invalid zip bucket");
        uint64_t b_orig = 0;
        std::memcpy(&b_orig, pdata, 8);
        uint32_t t_sz2 = 0;
        std::memcpy(&t_sz2, pdata + 8, 4);
        if (12 + (size_t)t_sz2 > plen) throw std::runtime_error("Invalid zip bucket truncated");
        ByteVec t_trans, p_comp;
        if (plen - 12 - t_sz2 >= 65536) {
            std::exception_ptr p_err;
            std::thread p_thr([&]() {
                codecs::g_tl_decode_threads = 1;
                try { p_comp = codecs::bsc_parallel_decompress(pdata + 12 + t_sz2, plen - 12 - t_sz2); }
                catch (...) { p_err = std::current_exception(); }
                codecs::g_tl_decode_threads = 0;
            });
            t_trans = (t_sz2 >= 12 && std::memcmp(pdata + 12, "PTZD", 4) == 0)
                ? decompress_tar_zip_demux(pdata + 12, t_sz2)
                : codecs::lzma_auto_decompress(pdata + 12, t_sz2);
            p_thr.join();
            if (p_err) std::rethrow_exception(p_err);
        } else {
            t_trans = (t_sz2 >= 12 && std::memcmp(pdata + 12, "PTZD", 4) == 0)
                ? decompress_tar_zip_demux(pdata + 12, t_sz2)
                : codecs::lzma_auto_decompress(pdata + 12, t_sz2);
            p_comp = codecs::bsc_parallel_decompress(pdata + 12 + t_sz2, plen - 12 - t_sz2);
        }
        ByteVec out;
        if (!precond::zip_precomp_decode(t_trans.data(), t_trans.size(), p_comp.data(), p_comp.size(), b_orig, out)) {
            throw std::runtime_error("zip_precomp_decode failed");
        }
        return out;
    }
    return codecs::bsc_parallel_decompress(pdata, plen);
}

inline ByteVec decompress_bucket_payload(const uint8_t* data, size_t len, uint64_t orig_size) {
    if (len == 0 || orig_size == 0) return ByteVec();
    if (len < 2) throw std::runtime_error("Invalid bucket payload container");
    size_t offset = 0;
    bool cdc_applied = (data[offset++] != 0);

    ByteVec raw_payload;
    if (cdc_applied) {
        if (offset + 4 > len) throw std::runtime_error("Bucket CDC truncated");
        uint32_t t_sz = 0;
        std::memcpy(&t_sz, data + offset, 4);
        offset += 4;
        if (offset + t_sz + 8 > len) throw std::runtime_error("Bucket CDC truncated");
        ByteVec chunk_table = codecs::bsc_parallel_decompress(data + offset, t_sz);
        offset += t_sz;
        uint64_t up_orig = 0;
        std::memcpy(&up_orig, data + offset, 8);
        offset += 8;

        if (offset >= len) throw std::runtime_error("Bucket payload missing sub_codec_id");
        uint8_t sub_codec_id = data[offset++];
        ByteVec unique_payload = decode_sub_codec_payload(sub_codec_id, data + offset, len - offset, up_orig);

        if (!precond::cdc_decode(unique_payload.data(), unique_payload.size(), chunk_table.data(), chunk_table.size(), orig_size, raw_payload)) {
            throw std::runtime_error("Bucket cdc_decode failed");
        }
        return raw_payload;
    } else {
        if (offset >= len) throw std::runtime_error("Bucket payload missing sub_codec_id");
        uint8_t sub_codec_id = data[offset++];
        return decode_sub_codec_payload(sub_codec_id, data + offset, len - offset, orig_size);
    }
}

// Compress TarDemuxResult helper with unified bucket compression
inline ByteVec compress_tar_demux(const precond::TarDemuxResult& tar_res, uint32_t& flags, bool enable_cdc) {
    flags |= (1 << 1); // tar_demux applied
    std::vector<ByteVec> comp_buckets(precond::BUCKET_COUNT);
    ByteVec meta_comp;

    constexpr size_t LARGE_THRESH = 2 * 1024 * 1024;
    std::vector<int> large_items;
    std::vector<int> small_items;

    if (tar_res.meta_stream.size() >= LARGE_THRESH) {
        large_items.push_back(-1);
    } else {
        small_items.push_back(-1);
    }

    for (int b = 0; b < precond::BUCKET_COUNT; ++b) {
        size_t bsz = tar_res.buckets[b].size();
        if (bsz == 0) continue;
        if (bsz >= LARGE_THRESH) {
            large_items.push_back(b);
        } else {
            small_items.push_back(b);
        }
    }

    auto run_item = [&](int tag) {
        if (tag == -1) {
            codecs::SuppressProgress sp;
            meta_comp = codecs::bsc_parallel_compress(tar_res.meta_stream.data(), tar_res.meta_stream.size(), 3, 15, 72, 0);
        } else {
            comp_buckets[tag] = compress_bucket_payload(tar_res.buckets[tag].data(), tar_res.buckets[tag].size(), (uint8_t)tag, enable_cdc);
        }
    };

    if (large_items.size() >= 2) {
        std::sort(large_items.begin(), large_items.end(), [&](int a, int b) {
            uint64_t sa = (a == -1) ? tar_res.meta_stream.size() : tar_res.buckets[a].size();
            uint64_t sb = (b == -1) ? tar_res.meta_stream.size() : tar_res.buckets[b].size();
            return sa > sb;
        });
        std::mutex err_mtx;
        std::exception_ptr err_ptr;
        std::vector<std::thread> workers;
        workers.reserve(large_items.size());
        const int per_worker_t = std::max(2, (omp_get_max_threads() + static_cast<int>(large_items.size()) - 1) / static_cast<int>(large_items.size()));
        for (int tag : large_items) {
            workers.emplace_back([&, tag, per_worker_t]() {
                codecs::g_tl_decode_threads = per_worker_t;
                try {
                    run_item(tag);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(err_mtx);
                    if (!err_ptr) err_ptr = std::current_exception();
                }
                codecs::g_tl_decode_threads = 0;
            });
        }
        for (int tag : small_items) {
            try {
                run_item(tag);
            } catch (...) {
                std::lock_guard<std::mutex> lock(err_mtx);
                if (!err_ptr) err_ptr = std::current_exception();
            }
        }
        for (auto& w : workers) w.join();
        if (err_ptr) std::rethrow_exception(err_ptr);
    } else {
        for (int tag : large_items) {
            run_item(tag);
        }
        for (int tag : small_items) {
            run_item(tag);
        }
    }

    ByteVec container;
    auto append_u32 = [&](uint32_t val) {
        const uint8_t* p = (const uint8_t*)&val;
        container.insert(container.end(), p, p + 4);
    };

    append_u32((uint32_t)meta_comp.size());
    container.insert(container.end(), meta_comp.begin(), meta_comp.end());

    for (int b = 0; b < precond::BUCKET_COUNT; ++b) {
        uint64_t b_orig = (uint64_t)tar_res.buckets[b].size();
        const uint8_t* pbo = (const uint8_t*)&b_orig;
        container.insert(container.end(), pbo, pbo + 8);
        append_u32((uint32_t)comp_buckets[b].size());
        container.insert(container.end(), comp_buckets[b].begin(), comp_buckets[b].end());
    }

    return container;
}

inline void decode_tar_demux_parts(const uint8_t* data, size_t len, ByteVec& meta_stream, ByteVec buckets[precond::BUCKET_COUNT]) {
    size_t offset = 0;
    auto read_u32 = [&](uint32_t& val) {
        if (offset + 4 > len) throw std::runtime_error("TarDemux decode truncated");
        std::memcpy(&val, data + offset, 4);
        offset += 4;
    };

    uint32_t meta_sz = 0;
    read_u32(meta_sz);
    if (offset + meta_sz > len) throw std::runtime_error("TarDemux meta truncated");
    const uint8_t* meta_ptr = data + offset;
    offset += meta_sz;

    struct BucketDesc {
        uint64_t b_orig = 0;
        uint32_t b_sz = 0;
        const uint8_t* b_ptr = nullptr;
    };
    BucketDesc bdesc[precond::BUCKET_COUNT];
    for (int b = 0; b < precond::BUCKET_COUNT; ++b) {
        if (offset + 8 > len) throw std::runtime_error("Bucket orig_size truncated");
        uint64_t b_orig = 0;
        std::memcpy(&b_orig, data + offset, 8);
        offset += 8;
        uint32_t b_sz = 0;
        read_u32(b_sz);
        if (offset + b_sz > len) throw std::runtime_error("Bucket payload truncated");
        bdesc[b] = BucketDesc{b_orig, b_sz, data + offset};
        offset += b_sz;
        buckets[b].clear();
    }

    constexpr uint64_t LARGE_THRESH = 2ULL * 1024 * 1024;
    std::vector<int> large_items;
    std::vector<int> small_items;

    uint64_t meta_orig_est = static_cast<uint64_t>(meta_sz) * 4ULL;
    if (meta_orig_est >= LARGE_THRESH) {
        large_items.push_back(-1);
    } else {
        small_items.push_back(-1);
    }

    for (int b = 0; b < precond::BUCKET_COUNT; ++b) {
        if (bdesc[b].b_sz == 0 || bdesc[b].b_orig == 0) continue;
        if (bdesc[b].b_orig >= LARGE_THRESH) {
            large_items.push_back(b);
        } else {
            small_items.push_back(b);
        }
    }

    auto run_item = [&](int tag) {
        if (tag == -1) {
            meta_stream = codecs::bsc_parallel_decompress(meta_ptr, meta_sz);
        } else {
            buckets[tag] = decompress_bucket_payload(bdesc[tag].b_ptr, bdesc[tag].b_sz, bdesc[tag].b_orig);
        }
    };

    if (large_items.size() >= 2) {
        std::sort(large_items.begin(), large_items.end(), [&](int a, int b) {
            uint64_t sa = (a == -1) ? meta_orig_est : bdesc[a].b_orig;
            uint64_t sb = (b == -1) ? meta_orig_est : bdesc[b].b_orig;
            return sa > sb;
        });
        std::mutex err_mtx;
        std::exception_ptr err_ptr;
        std::vector<std::thread> workers;
        workers.reserve(large_items.size());
        const int per_worker_t = std::max(2, (omp_get_max_threads() + static_cast<int>(large_items.size()) - 1) / static_cast<int>(large_items.size()));
        for (int tag : large_items) {
            workers.emplace_back([&, tag, per_worker_t]() {
                codecs::g_tl_decode_threads = per_worker_t;
                try {
                    run_item(tag);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(err_mtx);
                    if (!err_ptr) err_ptr = std::current_exception();
                }
                codecs::g_tl_decode_threads = 0;
            });
        }
        for (int tag : small_items) {
            try {
                run_item(tag);
            } catch (...) {
                std::lock_guard<std::mutex> lock(err_mtx);
                if (!err_ptr) err_ptr = std::current_exception();
            }
        }
        for (auto& w : workers) w.join();
        if (err_ptr) std::rethrow_exception(err_ptr);
    } else {
        for (int tag : large_items) {
            run_item(tag);
        }
        for (int tag : small_items) {
            run_item(tag);
        }
    }
}

inline ByteVec try_compress_tar_zip_demux(const uint8_t* transformed, size_t tr_len, const uint8_t* patch_meta, size_t pm_len) {
    if (!transformed || tr_len == 0 || !patch_meta || pm_len < 4) return ByteVec();
    uint32_t num_patches = 0;
    std::memcpy(&num_patches, patch_meta, 4);
    if (pm_len < 4 + static_cast<size_t>(num_patches) * sizeof(precond::ZipPatch)) return ByteVec();

    std::vector<precond::ZipPatch> patches(num_patches);
    if (num_patches > 0) {
        std::memcpy(patches.data(), patch_meta + 4, static_cast<size_t>(num_patches) * sizeof(precond::ZipPatch));
    }

    precond::TarDemuxResult sub_res;
    sub_res.applied = true;
    sub_res.orig_size = tr_len;
    sub_res.meta_stream.reserve(tr_len / 64 + 1024);

    auto emit_raw = [&](const uint8_t* ptr, size_t len) {
        if (len == 0) return;
        sub_res.meta_stream.push_back(precond::OP_RAW_CHUNK);
        uint32_t clen = static_cast<uint32_t>(len);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&clen);
        sub_res.meta_stream.insert(sub_res.meta_stream.end(), p, p + 4);
        sub_res.meta_stream.insert(sub_res.meta_stream.end(), ptr, ptr + len);
    };

    size_t tr_cursor = 0;
    uint64_t prev_orig_off = 0;
    for (uint32_t i = 0; i < num_patches; ++i) {
        const auto& p = patches[i];
        if (p.orig_comp_off < prev_orig_off) return ByteVec();
        size_t gap_len = static_cast<size_t>(p.orig_comp_off - prev_orig_off);
        if (tr_cursor + gap_len > tr_len) return ByteVec();
        emit_raw(transformed + tr_cursor, gap_len);
        tr_cursor += gap_len;

        if (tr_cursor + p.uncomp_len > tr_len) return ByteVec();
        if (p.uncomp_len >= 65536 && precond::tar_check_header(transformed + tr_cursor)) {
            size_t saved_meta_sz = sub_res.meta_stream.size();
            size_t saved_bsz[precond::BUCKET_COUNT];
            for (int b = 0; b < precond::BUCKET_COUNT; ++b) saved_bsz[b] = sub_res.buckets[b].size();

            if (!precond::demux_tar_span(transformed + tr_cursor, p.uncomp_len, sub_res.meta_stream, sub_res.buckets)) {
                sub_res.meta_stream.resize(saved_meta_sz);
                for (int b = 0; b < precond::BUCKET_COUNT; ++b) sub_res.buckets[b].resize(saved_bsz[b]);
                emit_raw(transformed + tr_cursor, p.uncomp_len);
            }
        } else {
            emit_raw(transformed + tr_cursor, p.uncomp_len);
        }
        tr_cursor += p.uncomp_len;
        prev_orig_off = p.orig_comp_off + p.orig_comp_len;
    }
    if (tr_cursor < tr_len) {
        emit_raw(transformed + tr_cursor, tr_len - tr_cursor);
    }

    if (sub_res.buckets[precond::BUCKET_TEXT].size() < 2ULL * 1024 * 1024) {
        return ByteVec();
    }

    uint32_t dummy_flags = 0;
    ByteVec inner = compress_tar_demux(sub_res, dummy_flags, false);
    ByteVec out;
    out.reserve(12 + inner.size());
    out.insert(out.end(), {'P', 'T', 'Z', 'D'});
    uint64_t orig_tr = static_cast<uint64_t>(tr_len);
    const uint8_t* po = reinterpret_cast<const uint8_t*>(&orig_tr);
    out.insert(out.end(), po, po + 8);
    out.insert(out.end(), inner.begin(), inner.end());
    return out;
}

inline ByteVec decompress_tar_zip_demux(const uint8_t* src, size_t len) {
    if (len < 12 || std::memcmp(src, "PTZD", 4) != 0) {
        throw std::runtime_error("Invalid PTZD container");
    }
    uint64_t orig_tr = 0;
    std::memcpy(&orig_tr, src + 4, 8);
    ByteVec meta_stream;
    ByteVec buckets[precond::BUCKET_COUNT];
    decode_tar_demux_parts(src + 12, len - 12, meta_stream, buckets);
    ByteVec out;
    if (!precond::tar_demux_decode(meta_stream.data(), meta_stream.size(), buckets, orig_tr, out)) {
        throw std::runtime_error("PTZD tar_demux_decode failed");
    }
    return out;
}

inline void emit_raw_op(ByteVec& meta, const uint8_t* ptr, size_t len) {
    if (len == 0) return;
    meta.push_back(precond::OP_RAW_CHUNK);
    uint32_t clen = (uint32_t)len;
    const uint8_t* p = (const uint8_t*)&clen;
    meta.insert(meta.end(), p, p + 4);
    meta.insert(meta.end(), ptr, ptr + len);
}

// Pack directory into classified TarDemuxResult
inline precond::TarDemuxResult pack_dir_to_tar_demux(const uint8_t* packed_dir, size_t len) {
    if (len < 8) throw std::runtime_error("Invalid PDIR for demux");
    uint32_t file_count = 0;
    std::memcpy(&file_count, packed_dir + 4, 4);
    size_t offset = 8;

    precond::TarDemuxResult res;
    res.applied = true;
    res.orig_size = len;

    struct DirEntrySpan {
        size_t hdr_off;
        size_t hdr_len;
        const uint8_t* fdata;
        uint64_t fsize;
        precond::TarBucketId b_id;
    };

    // Pass 1: scan PDIR entries into spans
    std::vector<DirEntrySpan> entries;
    entries.reserve(std::min<size_t>(file_count, len / 10));

    for (uint32_t i = 0; i < file_count; ++i) {
        size_t hdr_start = offset;
        if (offset + 2 > len) throw std::runtime_error("PDIR truncated");
        uint16_t path_len = 0;
        std::memcpy(&path_len, packed_dir + offset, 2);
        offset += 2;

        if (offset + path_len + 8 > len) throw std::runtime_error("PDIR truncated");
        offset += path_len;

        uint64_t fsize = 0;
        std::memcpy(&fsize, packed_dir + offset, 8);
        offset += 8;

        if (offset + fsize > len) throw std::runtime_error("PDIR truncated");
        size_t hdr_len = offset - hdr_start;
        const uint8_t* fdata = packed_dir + offset;
        offset += (size_t)fsize;

        entries.push_back(DirEntrySpan{hdr_start, hdr_len, fdata, fsize, precond::BUCKET_BINARY});
    }

    // Pass 2: classify all entries in parallel
    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        auto& e = entries[i];
        const uint8_t* fdata = e.fdata;
        uint64_t fsize = e.fsize;

        precond::TarBucketId b_id = precond::BUCKET_BINARY;
        auto exec_t = precond::detect_executable(fdata, (size_t)fsize);
        if (exec_t == precond::EXEC_X86) {
            b_id = precond::BUCKET_EXEC_X86;
        } else if (exec_t == precond::EXEC_SPARC || exec_t == precond::EXEC_ARM64) {
            b_id = precond::BUCKET_EXEC_SPARC;
        } else if ((fsize >= 1024 && (fsize % 512) == 0 && precond::tar_check_header(fdata) && precond::has_embedded_zip(fdata, (size_t)fsize)) ||
                   (fsize >= 4 && fdata[0] == 'P' && fdata[1] == 'K' && fdata[2] == 3 && fdata[3] == 4) ||
                   (fsize >= 18 && fdata[0] == 0x1F && fdata[1] == 0x8B && fdata[2] == 0x08)) {
            b_id = precond::BUCKET_TAR_ARCHIVE;
        } else if (precond::has_beneficial_stride(fdata, (size_t)fsize)) {
            b_id = precond::BUCKET_STRIDE;
        } else {
            size_t check_len = std::min<size_t>((size_t)fsize, 8192);
            size_t printable = precond::count_printable_ascii(fdata, check_len);
            if (check_len > 0 && (double)printable / check_len >= 0.78) {
                b_id = precond::BUCKET_TEXT;
            } else {
                b_id = precond::BUCKET_BINARY;
            }
        }
        e.b_id = b_id;
    }

    // Pass 3: pre-size meta_stream and buckets, then emit in exact original order
    size_t meta_total = 13; // initial emit_raw_op(packed_dir, 8) = 1 + 4 + 8
    size_t bucket_totals[precond::BUCKET_COUNT] = {0};
    for (const auto& e : entries) {
        if (e.hdr_len > 0) meta_total += 5 + e.hdr_len;
        meta_total += 6; // OP_BUCKET_ENTRY (1) + b_id (1) + fs32 (4)
        bucket_totals[e.b_id] += static_cast<size_t>(e.fsize);
    }

    res.meta_stream.reserve(meta_total);
    for (int b = 0; b < precond::BUCKET_COUNT; ++b) {
        if (bucket_totals[b] > 0) {
            res.buckets[b].resize(bucket_totals[b]);
        }
    }

    std::vector<size_t> entry_bucket_off(entries.size(), 0);
    size_t bucket_cursors[precond::BUCKET_COUNT] = {0};

    emit_raw_op(res.meta_stream, packed_dir, 8);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        emit_raw_op(res.meta_stream, packed_dir + e.hdr_off, e.hdr_len);

        res.meta_stream.push_back(precond::OP_BUCKET_ENTRY);
        res.meta_stream.push_back((uint8_t)e.b_id);
        uint32_t fs32 = (uint32_t)e.fsize;
        const uint8_t* p_fs = (const uint8_t*)&fs32;
        res.meta_stream.insert(res.meta_stream.end(), p_fs, p_fs + 4);

        entry_bucket_off[i] = bucket_cursors[e.b_id];
        bucket_cursors[e.b_id] += static_cast<size_t>(e.fsize);
    }

    #pragma omp parallel for schedule(dynamic, 64)
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const auto& e = entries[i];
        if (e.fsize > 0) {
            std::memcpy(res.buckets[e.b_id].data() + entry_bucket_off[i], e.fdata, static_cast<size_t>(e.fsize));
        }
    }

    return res;
}

// Core compress dispatcher
inline ByteVec compress_stream(const uint8_t* data, size_t len, uint32_t& flags) {
    flags = 0;

    if (len >= 16 * 1024 * 1024) {
        auto tar_res = precond::tar_demux_encode(data, len);
        if (tar_res.applied) {
            bool accept_tar = (tar_res.buckets[precond::BUCKET_TEXT].size() * 100 >= len * 15) ||
                              !tar_res.buckets[precond::BUCKET_STRIDE].empty() ||
                              !tar_res.buckets[precond::BUCKET_EXEC_X86].empty();
            if (accept_tar) {
                uint64_t total_bucket_bytes = 0;
                for (int b = 0; b < precond::BUCKET_COUNT; ++b) {
                    total_bucket_bytes += static_cast<uint64_t>(tar_res.buckets[b].size());
                }
                codecs::reset_payload_progress(total_bucket_bytes);
                return compress_tar_demux(tar_res, flags, false);
            }
        }
    }

    auto exec_type = precond::detect_executable(data, len);
    if (exec_type == precond::EXEC_X86) {
        flags |= (1 << 3);
        codecs::reset_payload_progress(static_cast<uint64_t>(len));
        return codecs::exec_x86_compress(data, len);
    } else if (exec_type == precond::EXEC_SPARC) {
        flags |= (1 << 4);
        codecs::reset_payload_progress(static_cast<uint64_t>(len));
        return codecs::exec_sparc_compress(data, len);
    } else if (exec_type == precond::EXEC_ARM64) {
        flags |= (1 << 4);
        codecs::reset_payload_progress(static_cast<uint64_t>(len));
        return codecs::exec_arm64_compress(data, len);
    }

    auto stride_res = precond::stride_encode(data, len);
    if (stride_res.applied) {
        flags |= (1 << 2);
        codecs::reset_payload_progress(static_cast<uint64_t>(stride_res.data.size()));
        ByteVec s_comp = g_fast_mode
                    ? codecs::lzma_parallel_compress(stride_res.data.data(), stride_res.data.size(), 8, 64, 16, 6, 3, 0, 0, 5)
                    : codecs::lzma2_parallel_compress(stride_res.data.data(), stride_res.data.size(), 16, 64, 24, 0, 3, 0, 0, 6);
        ByteVec container;
        container.push_back(stride_res.stride);
        const uint8_t* p_mask = (const uint8_t*)&stride_res.delta_mask;
        container.insert(container.end(), p_mask, p_mask + 8);
        container.insert(container.end(), s_comp.begin(), s_comp.end());
        return container;
    }

    if (!g_fast_mode) {
        auto zip_res = precond::zip_precomp_encode(data, len, codecs::g_comp_level >= 9);
        if (zip_res.applied) {
            flags |= (1 << 5);
            codecs::reset_payload_progress(static_cast<uint64_t>(zip_res.transformed.size()));
            ByteVec t_comp = is_predominantly_text(zip_res.transformed.data(), zip_res.transformed.size())
                ? codecs::lrm_bsc_compress(zip_res.transformed.data(), zip_res.transformed.size(), 3, 15, 72, 0)
                : codecs::lrm_lzma2_compress(zip_res.transformed.data(), zip_res.transformed.size(), 16, 64, 24, 3, 0, 2, 6);
            ByteVec p_comp;
            {
                codecs::SuppressProgress sp;
                p_comp = codecs::bsc_parallel_compress(zip_res.patch_meta.data(), zip_res.patch_meta.size(), 3, 15, 72, 0);
            }
            ByteVec container;
            uint32_t t_sz = (uint32_t)t_comp.size();
            const uint8_t* pt = (const uint8_t*)&t_sz;
            container.insert(container.end(), pt, pt + 4);
            container.insert(container.end(), t_comp.begin(), t_comp.end());
            container.insert(container.end(), p_comp.begin(), p_comp.end());
            return container;
        }
    }

    if (!g_fast_mode && codecs::should_use_lzma2_for_binary(data, len)) {
        flags |= (1 << 8);
        codecs::reset_payload_progress(static_cast<uint64_t>(len));
        return codecs::lrm_lzma2_compress(data, len, 16, 64, 24, 3, 0, 2, 6);
    }

    codecs::reset_payload_progress(static_cast<uint64_t>(len));
    if (g_fast_mode && len >= 4ULL * 1024 * 1024) {
        ByteVec out = codecs::lrm_bsc_compress(data, len, 3, 15, 72, 0);
        if (out.size() >= 4 && std::memcmp(out.data(), "PLRM", 4) == 0) {
            flags |= (1 << 8);
        }
        return out;
    }
    return codecs::bsc_parallel_compress(data, len, 3, 15, 72, 0);
}

inline ByteVec decompress_stream(const uint8_t* data, size_t len, uint64_t orig_size, uint32_t flags) {
    if (flags & (1 << 1)) {
        ByteVec meta_stream;
        ByteVec buckets[precond::BUCKET_COUNT];
        decode_tar_demux_parts(data, len, meta_stream, buckets);

        ByteVec out;
        bool ok = precond::tar_demux_decode(meta_stream.data(), meta_stream.size(), buckets, orig_size, out);
        if (!ok) throw std::runtime_error("tar_demux_decode failed");
        return out;
    }

    if (flags & (1 << 8)) {
        return codecs::lzma_auto_decompress(data, len);
    }

    if (flags & (1 << 7)) {
        return codecs::lzma_parallel_decompress(data, len);
    }

    if (flags & (1 << 3)) {
        return codecs::exec_x86_decompress(data, len);
    }
    if (flags & (1 << 4)) {
        return codecs::exec_sparc_decompress(data, len);
    }
    if (flags & (1 << 2)) {
        if (len < 9) throw std::runtime_error("Invalid stride container");
        uint8_t stride_val = data[0];
        uint64_t stride_mask = 0;
        std::memcpy(&stride_mask, data + 1, 8);
        ByteVec s_raw = codecs::lzma_auto_decompress(data + 9, len - 9);
        ByteVec out;
        bool ok = precond::stride_decode(s_raw.data(), s_raw.size(), orig_size, stride_val, stride_mask, out);
        if (!ok) throw std::runtime_error("stride_decode failed");
        return out;
    }
    if (flags & (1 << 5)) {
        if (len < 4) throw std::runtime_error("Invalid zip container");
        uint32_t t_sz = 0;
        std::memcpy(&t_sz, data, 4);
        if (4 + t_sz > len) throw std::runtime_error("Invalid zip container truncated");
        ByteVec t_trans = (t_sz >= 12 && std::memcmp(data + 4, "PTZD", 4) == 0)
            ? decompress_tar_zip_demux(data + 4, t_sz)
            : codecs::lzma_auto_decompress(data + 4, t_sz);
        ByteVec p_meta = codecs::bsc_parallel_decompress(data + 4 + t_sz, len - 4 - t_sz);
        ByteVec out;
        bool ok = precond::zip_precomp_decode(t_trans.data(), t_trans.size(), p_meta.data(), p_meta.size(), orig_size, out);
        if (!ok) throw std::runtime_error("zip_precomp_decode failed");
        return out;
    }

    return codecs::bsc_parallel_decompress(data, len);
}

inline void decompress_tar_demux_to_file(const uint8_t* data, size_t len, uint64_t orig_size, const fs::path& output_path) {
    ByteVec meta_stream;
    ByteVec buckets[precond::BUCKET_COUNT];
    decode_tar_demux_parts(data, len, meta_stream, buckets);

    if (output_path.has_parent_path()) {
        fs::path parent = output_path.parent_path();
        if (!parent.empty() && !prism_exists(parent)) {
            prism_create_directories(parent);
        }
    }
    std::string pstr = output_path.string();
    FILE* fp = prism_fopen(output_path, "wb");
    if (!fp) throw std::runtime_error("Failed to open file for writing: " + pstr);
    std::setvbuf(fp, nullptr, _IOFBF, 4 * 1024 * 1024);

    size_t bucket_cursors[precond::BUCKET_COUNT] = {0};
    size_t meta_cursor = 0;
    uint64_t out_cursor = 0;
    const uint8_t* meta = meta_stream.data();
    size_t meta_len = meta_stream.size();

    while (meta_cursor < meta_len) {
        uint8_t op = meta[meta_cursor++];
        if (op == precond::OP_RAW_CHUNK) {
            if (meta_cursor + 4 > meta_len) { std::fclose(fp); throw std::runtime_error("tar_demux_decode failed"); }
            uint32_t clen = 0;
            std::memcpy(&clen, meta + meta_cursor, 4);
            meta_cursor += 4;
            if (meta_cursor + clen > meta_len || out_cursor + clen > orig_size) { std::fclose(fp); throw std::runtime_error("tar_demux_decode failed"); }
            if (clen > 0 && std::fwrite(meta + meta_cursor, 1, clen, fp) != clen) { std::fclose(fp); throw std::runtime_error("fwrite failed"); }
            meta_cursor += clen;
            out_cursor += clen;
        } else if (op == precond::OP_BUCKET_ENTRY) {
            if (meta_cursor + 5 > meta_len) { std::fclose(fp); throw std::runtime_error("tar_demux_decode failed"); }
            uint8_t b_id = meta[meta_cursor++];
            uint32_t entry_len = 0;
            std::memcpy(&entry_len, meta + meta_cursor, 4);
            meta_cursor += 4;
            if (b_id >= precond::BUCKET_COUNT || bucket_cursors[b_id] + entry_len > buckets[b_id].size() || out_cursor + entry_len > orig_size) {
                std::fclose(fp);
                throw std::runtime_error("tar_demux_decode failed");
            }
            if (entry_len > 0 && std::fwrite(buckets[b_id].data() + bucket_cursors[b_id], 1, entry_len, fp) != entry_len) {
                std::fclose(fp);
                throw std::runtime_error("fwrite failed");
            }
            bucket_cursors[b_id] += entry_len;
            out_cursor += entry_len;
        } else {
            std::fclose(fp);
            throw std::runtime_error("tar_demux_decode unknown opcode");
        }
    }
    std::fclose(fp);
    if (out_cursor != orig_size) throw std::runtime_error("tar_demux_decode size mismatch");
}

inline void decompress_tar_demux_to_directory(const uint8_t* data, size_t len, uint64_t orig_size, const fs::path& out_dir) {
    ByteVec meta_stream;
    ByteVec buckets[precond::BUCKET_COUNT];
    decode_tar_demux_parts(data, len, meta_stream, buckets);

    const uint8_t* meta = meta_stream.data();
    size_t meta_len = meta_stream.size();
    size_t meta_cursor = 0;
    uint64_t out_cursor = 0;
    size_t bucket_cursors[precond::BUCKET_COUNT] = {0};

    bool fast_ok = false;
    std::vector<WriteEntry> items;

    // Fast path: verify PDIR header OP_RAW_CHUNK of 8 bytes ("PDIR" + uint32_t file_count)
    if (meta_len >= 13 && meta[0] == precond::OP_RAW_CHUNK) {
        uint32_t hdr_len = 0;
        std::memcpy(&hdr_len, meta + 1, 4);
        if (hdr_len == 8 && std::memcmp(meta + 5, "PDIR", 4) == 0 && orig_size >= 8) {
            uint32_t file_count = 0;
            std::memcpy(&file_count, meta + 9, 4);
            meta_cursor = 13;
            out_cursor = 8;

            // Each entry in fast-path meta_stream takes at least 1+4+2+8+6 = 21 bytes
            if (static_cast<uint64_t>(file_count) * 21 <= meta_len - meta_cursor) {
                items.reserve(file_count);
                fast_ok = true;

                for (uint32_t i = 0; i < file_count; ++i) {
                    if (meta_cursor + 5 > meta_len || meta[meta_cursor] != precond::OP_RAW_CHUNK) {
                        fast_ok = false;
                        break;
                    }
                    uint32_t clen = 0;
                    std::memcpy(&clen, meta + meta_cursor + 1, 4);
                    meta_cursor += 5;

                    if (clen < 10 || meta_cursor + clen > meta_len || out_cursor + clen > orig_size) {
                        fast_ok = false;
                        break;
                    }
                    const uint8_t* chunk = meta + meta_cursor;
                    uint16_t path_len = 0;
                    std::memcpy(&path_len, chunk, 2);
                    if (clen != static_cast<uint32_t>(2 + path_len + 8)) {
                        fast_ok = false;
                        break;
                    }
                    std::string rel_path(reinterpret_cast<const char*>(chunk + 2), path_len);
                    uint64_t fsize = 0;
                    std::memcpy(&fsize, chunk + 2 + path_len, 8);
                    meta_cursor += clen;
                    out_cursor += clen;

                    if (meta_cursor + 6 > meta_len || meta[meta_cursor] != precond::OP_BUCKET_ENTRY) {
                        fast_ok = false;
                        break;
                    }
                    uint8_t b_id = meta[meta_cursor + 1];
                    uint32_t entry_len = 0;
                    std::memcpy(&entry_len, meta + meta_cursor + 2, 4);
                    meta_cursor += 6;

                    if (b_id >= precond::BUCKET_COUNT ||
                        static_cast<uint64_t>(entry_len) != fsize ||
                        bucket_cursors[b_id] + entry_len > buckets[b_id].size() ||
                        out_cursor + entry_len > orig_size) {
                        fast_ok = false;
                        break;
                    }

                    const uint8_t* fptr = (fsize > 0) ? (buckets[b_id].data() + bucket_cursors[b_id]) : nullptr;
                    items.push_back(WriteEntry{out_dir / fs::path(sanitize_rel_path(rel_path)), fptr, static_cast<size_t>(fsize)});
                    bucket_cursors[b_id] += entry_len;
                    out_cursor += entry_len;
                }

                if (fast_ok && (meta_cursor != meta_len || out_cursor != orig_size)) {
                    fast_ok = false;
                }
            }
        }
    }

    if (fast_ok) {
        write_entries_parallel(out_dir, items);
        return;
    }

    // Clean fallback for legacy or split-op meta_streams
    ByteVec out;
    if (!precond::tar_demux_decode(meta_stream.data(), meta_stream.size(), buckets, orig_size, out)) {
        throw std::runtime_error("tar_demux_decode failed");
    }
    unpack_directory(out.data(), out.size(), out_dir);
}

inline ByteVec decompress_archive_buffer(const uint8_t* data, size_t size, uint32_t& out_flags, uint64_t& out_orig_size) {
    if (size < 28) throw std::runtime_error("Invalid PRISM archive: too short");
    if (std::memcmp(data, PRISM_MAGIC, 4) != 0) throw std::runtime_error("Invalid PRISM archive magic");

    uint32_t flags = 0; std::memcpy(&flags, data + 8, 4);
    uint64_t orig_size = 0; std::memcpy(&orig_size, data + 12, 8);
    uint64_t comp_size = 0; std::memcpy(&comp_size, data + 20, 8);

    if (comp_size > static_cast<uint64_t>(size - 28)) throw std::runtime_error("Invalid PRISM archive truncated");
    const uint8_t* payload = data + 28;

    ByteVec decompressed;
    bool is_cdc = (flags & (1 << 0));
    if (is_cdc) {
        size_t offset = 0;
        if (offset + 4 > comp_size) throw std::runtime_error("CDC header truncated");
        uint32_t t_sz = 0;
        std::memcpy(&t_sz, payload + offset, 4);
        offset += 4;
        if (static_cast<uint64_t>(t_sz) + 8 > comp_size - offset) throw std::runtime_error("CDC chunk_table truncated");
        ByteVec chunk_table = codecs::bsc_parallel_decompress(payload + offset, t_sz);
        offset += t_sz;
        uint64_t up_orig = 0;
        std::memcpy(&up_orig, payload + offset, 8);
        offset += 8;
        uint32_t sub_flags = flags & ~((uint32_t)1 | (1U << 6));
        ByteVec unique_payload = decompress_stream(payload + offset, (size_t)comp_size - offset, up_orig, sub_flags);
        if (!precond::cdc_decode(unique_payload.data(), unique_payload.size(), chunk_table.data(), chunk_table.size(), orig_size, decompressed)) {
            throw std::runtime_error("cdc_decode failed");
        }
    } else {
        decompressed = decompress_stream(payload, (size_t)comp_size, orig_size, flags);
    }
    out_flags = flags;
    out_orig_size = orig_size;
    return decompressed;
}

inline ByteVec decompress_archive_to_memory(const fs::path& input_prism, uint32_t& out_flags, uint64_t& out_orig_size) {
    auto archive = read_file_raw(input_prism);
    return decompress_archive_buffer(archive.data(), archive.size, out_flags, out_orig_size);
}

struct PidxEntry {
    std::string path;
    uint64_t fsize;
    uint8_t kind; // 0 = FILE, 1 = TAR_ENTRY
};

inline std::vector<PidxEntry> extract_pidx_from_pdir(const uint8_t* data, size_t size) {
    std::vector<PidxEntry> entries;
    if (size < 8 || std::memcmp(data, "PDIR", 4) != 0) return entries;
    uint32_t file_count = 0;
    std::memcpy(&file_count, data + 4, 4);
    entries.reserve(file_count);
    size_t offset = 8;
    for (uint32_t i = 0; i < file_count; ++i) {
        if (offset + 2 > size) break;
        uint16_t path_len = 0;
        std::memcpy(&path_len, data + offset, 2);
        offset += 2;
        if (offset + path_len + 8 > size) break;
        std::string rpath((const char*)(data + offset), path_len);
        offset += path_len;
        uint64_t fsize = 0;
        std::memcpy(&fsize, data + offset, 8);
        offset += 8;
        if (offset + fsize > size) break;
        offset += (size_t)fsize;
        entries.push_back({std::move(rpath), fsize, 0});
    }
    return entries;
}

inline std::vector<PidxEntry> extract_pidx_from_tar(const uint8_t* data, size_t size) {
    std::vector<PidxEntry> entries;
    if (size < 512 || !precond::tar_check_header(data)) return entries;
    size_t t_offset = 0;
    while (t_offset + 512 <= size) {
        const char* header = (const char*)(data + t_offset);
        if (header[0] == '\0') break;
        std::string name(header, 100);
        size_t null_pos = name.find('\0');
        if (null_pos != std::string::npos) name = name.substr(0, null_pos);
        char size_buf[12];
        std::memcpy(size_buf, header + 124, 11);
        size_buf[11] = '\0';
        uint64_t t_fsize = std::strtoull(size_buf, nullptr, 8);
        char typeflag = header[156];
        t_offset += 512;
        if (typeflag == '0' || typeflag == '\0' || typeflag == '5') {
            if (!name.empty()) {
                entries.push_back({std::move(name), t_fsize, 1});
            }
        }
        size_t blocks = (t_fsize + 511) / 512;
        t_offset += blocks * 512;
    }
    return entries;
}

inline std::string describe_algorithm(uint32_t flags) {
    std::vector<std::string> parts;
    if (flags & (1u << 6)) parts.emplace_back("Directory");
    if (flags & (1u << 0)) parts.emplace_back("CDC");
    if (flags & (1u << 1)) parts.emplace_back("TarDemux");
    if (flags & (1u << 2)) parts.emplace_back("Stride");
    if (flags & (1u << 3)) parts.emplace_back("BCJ2+LZMA (x86)");
    if (flags & (1u << 4)) parts.emplace_back("SPARC+LZMA");
    if (flags & (1u << 5)) parts.emplace_back("ZipPrecomp+LZMA");
    if (flags & (1u << 7)) parts.emplace_back("Parallel LZMA");
    if (flags & (1u << 8)) parts.emplace_back("Parallel LZMA2");
    const uint32_t codec_mask = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8);
    if ((flags & codec_mask) == 0) {
        parts.emplace_back("Parallel BSC (BWT+QLFC)");
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += " + ";
        out += parts[i];
    }
    return out;
}

inline std::string json_escape(const std::string& s) {
    static const char hex_digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex_digits[(c >> 4) & 0x0F]);
                    out.push_back(hex_digits[c & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

inline size_t write_pmet_footer(FILE* fp, uint64_t meta_start_offset, const std::string& json_str) {
    if (!json_str.empty() && std::fwrite(json_str.data(), 1, json_str.size(), fp) != json_str.size()) {
        throw std::runtime_error("Failed to write PMET payload");
    }
    uint32_t meta_len = static_cast<uint32_t>(json_str.size());
    uint8_t footer[16];
    std::memcpy(footer, &meta_start_offset, 8);
    std::memcpy(footer + 8, &meta_len, 4);
    std::memcpy(footer + 12, "PMET", 4);
    if (std::fwrite(footer, 1, 16, fp) != 16) {
        throw std::runtime_error("Failed to write PMET footer");
    }
    return json_str.size() + 16;
}

inline bool find_trailer_block(FILE* fp, uint64_t file_sz, uint64_t comp_size, const char target_magic[4], uint64_t& out_off, uint32_t& out_len) {
    if (file_sz < 28 || comp_size > file_sz || 28ULL + comp_size > file_sz) {
        return false;
    }
    const uint64_t payload_end = 28ULL + comp_size;
    uint64_t cursor = file_sz;
    while (cursor >= 16 && cursor - 16 >= payload_end) {
        uint8_t tr[16];
        if (_fseeki64(fp, static_cast<long long>(cursor - 16), SEEK_SET) != 0) {
            return false;
        }
        if (std::fread(tr, 1, 16, fp) != 16) {
            return false;
        }
        const bool is_known = (std::memcmp(tr + 12, "PIDX", 4) == 0 ||
                               std::memcmp(tr + 12, "PMET", 4) == 0);
        if (!is_known) {
            return false;
        }
        uint64_t start_off = 0;
        uint32_t blk_len = 0;
        std::memcpy(&start_off, tr, 8);
        std::memcpy(&blk_len, tr + 8, 4);
        if (start_off < payload_end ||
            start_off > cursor - 16 ||
            static_cast<uint64_t>(blk_len) != (cursor - 16 - start_off)) {
            return false;
        }
        if (std::memcmp(tr + 12, target_magic, 4) == 0) {
            out_off = start_off;
            out_len = blk_len;
            return true;
        }
        cursor = start_off;
    }
    return false;
}

inline bool try_read_pmet_json(FILE* fp, uint64_t file_sz, uint64_t comp_size, std::string& out_json) {
    uint64_t meta_off = 0;
    uint32_t meta_len = 0;
    if (!find_trailer_block(fp, file_sz, comp_size, "PMET", meta_off, meta_len)) {
        return false;
    }
    if (meta_len == 0 || meta_len > 1024U * 1024U) {
        return false;
    }
    if (_fseeki64(fp, static_cast<long long>(meta_off), SEEK_SET) != 0) {
        return false;
    }
    std::string buf(meta_len, '\0');
    if (std::fread(buf.data(), 1, meta_len, fp) != meta_len) {
        return false;
    }
    out_json = std::move(buf);
    return true;
}

inline size_t write_pidx_footer(FILE* fp, uint64_t idx_start_offset, const std::vector<PidxEntry>& entries) {
    ByteVec raw;
    raw.reserve(4 + entries.size() * 48);
    uint32_t count = (uint32_t)entries.size();
    raw.insert(raw.end(), (const uint8_t*)&count, (const uint8_t*)&count + 4);
    for (const auto& e : entries) {
        uint16_t path_len = (uint16_t)std::min<size_t>(e.path.size(), 65535);
        raw.insert(raw.end(), (const uint8_t*)&path_len, (const uint8_t*)&path_len + 2);
        raw.insert(raw.end(), e.path.data(), e.path.data() + path_len);
        raw.insert(raw.end(), (const uint8_t*)&e.fsize, (const uint8_t*)&e.fsize + 8);
        raw.push_back(e.kind);
    }

    ByteVec comp_idx;
    {
        codecs::SuppressProgress sp;
        comp_idx = codecs::bsc_parallel_compress(raw.data(), raw.size(), 3, 15, 72, 0);
    }
    uint32_t idx_comp_len = (uint32_t)comp_idx.size();

    if (!comp_idx.empty() && std::fwrite(comp_idx.data(), 1, comp_idx.size(), fp) != comp_idx.size()) {
        throw std::runtime_error("Failed to write PIDX payload");
    }
    uint8_t footer[16];
    std::memcpy(footer, &idx_start_offset, 8);
    std::memcpy(footer + 8, &idx_comp_len, 4);
    std::memcpy(footer + 12, "PIDX", 4);
    if (std::fwrite(footer, 1, 16, fp) != 16) {
        throw std::runtime_error("Failed to write PIDX footer");
    }
    return comp_idx.size() + 16;
}

void cmd_compress(const fs::path& input_path, const fs::path& output_path) {
    ensure_can_overwrite_list({output_path});

    struct ProgressGuard {
        ProgressGuard() { codecs::g_emit_progress.store(true, std::memory_order_relaxed); }
        ~ProgressGuard() { codecs::g_emit_progress.store(false, std::memory_order_relaxed); }
    } progress_guard;
    codecs::reset_payload_progress(0);

    std::cout << "[PRISM] Packing & compressing " << input_path << " -> " << output_path
              << (g_fast_mode ? " [--fast]" : (codecs::g_ultra_mode ? " [--ultra]" : "")) << " ...\n";
    std::cout << "[PROGRESS] 5 Scanning/Packing\n" << std::flush;
    auto start_time = std::chrono::high_resolution_clock::now();

    ByteVec raw_data;
    bool is_dir = prism_is_directory(input_path);
    uint32_t flags = 0;
    uint64_t real_orig_size = 0;
    std::vector<PidxEntry> pidx_entries;
    bool build_pidx = false;

    if (is_dir) {
        flags |= (1 << 6);
        auto packed_dir = pack_directory(input_path);
        real_orig_size = (uint64_t)packed_dir.size();
        pidx_entries = extract_pidx_from_pdir(packed_dir.data(), packed_dir.size());
        build_pidx = true;

        std::cout << "[PROGRESS] 20 Preconditioning\n" << std::flush;
        precond::CdcResult cdc_res;
        if (g_fast_mode) {
            cdc_res = precond::cdc_encode(packed_dir.data(), packed_dir.size());
        }
        if (cdc_res.applied) {
            flags |= (1 << 0);
            ByteVec table_comp;
            {
                codecs::SuppressProgress sp;
                table_comp = codecs::bsc_parallel_compress(cdc_res.chunk_table.data(), cdc_res.chunk_table.size(), 3, 15, 72, 0);
            }
            uint32_t sub_flags = 0;
            ByteVec payload_comp;
            precond::ZipPrecompResult zip_res;
            if (!g_fast_mode) {
                zip_res = precond::zip_precomp_encode(cdc_res.unique_payload.data(), cdc_res.unique_payload.size());
            }
            std::cout << "[PROGRESS] 20 Compressing\n" << std::flush;
            codecs::reset_payload_progress(static_cast<uint64_t>(zip_res.applied ? zip_res.transformed.size() : cdc_res.unique_payload.size()));
            if (zip_res.applied) {
                sub_flags |= (1 << 5);
                ByteVec t_comp = is_predominantly_text(zip_res.transformed.data(), zip_res.transformed.size())
                ? codecs::lrm_bsc_compress(zip_res.transformed.data(), zip_res.transformed.size(), 3, 15, 72, 0)
                : codecs::lrm_lzma2_compress(zip_res.transformed.data(), zip_res.transformed.size(), 16, 64, 24, 3, 0, 2, 6);
                ByteVec p_comp;
                {
                    codecs::SuppressProgress sp;
                    p_comp = codecs::bsc_parallel_compress(zip_res.patch_meta.data(), zip_res.patch_meta.size(), 3, 15, 72, 0);
                }
                uint32_t t_sz = (uint32_t)t_comp.size();
                const uint8_t* pt = (const uint8_t*)&t_sz;
                payload_comp.insert(payload_comp.end(), pt, pt + 4);
                payload_comp.insert(payload_comp.end(), t_comp.begin(), t_comp.end());
                payload_comp.insert(payload_comp.end(), p_comp.begin(), p_comp.end());
            } else {
                if (g_fast_mode) {
                    sub_flags |= (1 << 8);
                    payload_comp = codecs::lrm_bsc_compress(cdc_res.unique_payload.data(), cdc_res.unique_payload.size(), 3, 12, 96, 0);
                } else {
                    sub_flags |= (1 << 8);
                    payload_comp = codecs::lrm_lzma2_compress(cdc_res.unique_payload.data(), cdc_res.unique_payload.size(), 16, 64, 24, 3, 0, 2, 6);
                }
            }
            flags |= sub_flags;

            uint64_t up_orig_size = (uint64_t)cdc_res.unique_payload.size();
            ByteVec container;
            uint32_t tbl_sz = (uint32_t)table_comp.size();
            const uint8_t* ptbl = (const uint8_t*)&tbl_sz;
            container.insert(container.end(), ptbl, ptbl + 4);
            container.insert(container.end(), table_comp.begin(), table_comp.end());
            const uint8_t* pup = (const uint8_t*)&up_orig_size;
            container.insert(container.end(), pup, pup + 8);
            container.insert(container.end(), payload_comp.begin(), payload_comp.end());
            raw_data = std::move(container);
        } else {
            auto demux_res = pack_dir_to_tar_demux(packed_dir.data(), packed_dir.size());
            std::cout << "[PROGRESS] 20 Compressing\n" << std::flush;
            uint64_t total_bucket_bytes = 0;
            for (int b = 0; b < precond::BUCKET_COUNT; ++b) {
                total_bucket_bytes += static_cast<uint64_t>(demux_res.buckets[b].size());
            }
            codecs::reset_payload_progress(total_bucket_bytes);
            uint32_t sub_flags = 0;
            raw_data = compress_tar_demux(demux_res, sub_flags, false);
            flags |= sub_flags;
            flags |= (1 << 6);
        }
    } else {
        auto file_bytes = read_file_raw(input_path);
        real_orig_size = (uint64_t)file_bytes.size;
        std::cout << "[PROGRESS] 20 Preconditioning\n" << std::flush;
        std::cout << "[PROGRESS] 20 Compressing\n" << std::flush;
        uint32_t sub_flags = 0;
        raw_data = compress_stream(file_bytes.data(), file_bytes.size, sub_flags);
        flags |= sub_flags;
        if (flags & (1 << 1)) {
            pidx_entries = extract_pidx_from_tar(file_bytes.data(), file_bytes.size);
            build_pidx = !pidx_entries.empty();
        }
    }

    codecs::reset_payload_progress(0);
    std::cout << "[PROGRESS] 95 Writing archive\n" << std::flush;
    if (output_path.has_parent_path()) {
        fs::path parent = output_path.parent_path();
        if (!parent.empty() && !prism_exists(parent)) {
            prism_create_directories(parent);
        }
    }

    uint64_t comp_size = (uint64_t)raw_data.size();
    uint8_t hdr[28];
    uint32_t ver = PRISM_VERSION;
    std::memcpy(hdr, PRISM_MAGIC, 4);
    std::memcpy(hdr + 4, &ver, 4);
    std::memcpy(hdr + 8, &flags, 4);
    std::memcpy(hdr + 12, &real_orig_size, 8);
    std::memcpy(hdr + 20, &comp_size, 8);

    std::string out_pstr = output_path.string();
    FILE* fp = prism_fopen(output_path, "wb");
    if (!fp) throw std::runtime_error("Failed to open file for writing: " + out_pstr);
    std::setvbuf(fp, nullptr, _IOFBF, 4 * 1024 * 1024);

    uint64_t total_archive_size = 28 + comp_size;
    std::chrono::duration<double, std::milli> elapsed{0.0};
    try {
        if (std::fwrite(hdr, 1, 28, fp) != 28) {
            throw std::runtime_error("Failed to write archive header: " + out_pstr);
        }
        if (!raw_data.empty() && std::fwrite(raw_data.data(), 1, raw_data.size(), fp) != raw_data.size()) {
            throw std::runtime_error("Failed to write archive payload: " + out_pstr);
        }
        if (build_pidx) {
            total_archive_size += (uint64_t)write_pidx_footer(fp, 28 + comp_size, pidx_entries);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        elapsed = end_time - start_time;

        if (g_embed_meta) {
            std::string raw_src =
#ifdef _WIN32
                win_wide_to_utf8(input_path.wstring().c_str());
#else
                input_path.string();
#endif
            while (raw_src.size() > 1 && (raw_src.back() == '/' || raw_src.back() == '\\')) {
                if (raw_src.size() == 3 && raw_src[1] == ':') break;
                raw_src.pop_back();
            }
            size_t last_sep = raw_src.find_last_of("/\\");
            std::string source_name = (last_sep == std::string::npos) ? raw_src : raw_src.substr(last_sep + 1);
            if (source_name.empty()) source_name = raw_src;

            std::time_t now_t = std::time(nullptr);
            std::tm utc_tm = {};
#ifdef _WIN32
            gmtime_s(&utc_tm, &now_t);
#else
            gmtime_r(&now_t, &utc_tm);
#endif
            char utc_buf[32] = {0};
            std::strftime(utc_buf, sizeof(utc_buf), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);

            const std::string alg_esc = json_escape(describe_algorithm(flags));
            const std::string src_esc = json_escape(source_name);
            const double enc_ms = elapsed.count();
            const int threads = omp_get_max_threads();
            const uint64_t base_archive_size = total_archive_size;

            auto build_pmet_json = [&](uint64_t cand_archive_bytes) -> std::string {
                double cand_ratio = real_orig_size > 0
                    ? (static_cast<double>(cand_archive_bytes) / static_cast<double>(real_orig_size)) * 100.0
                    : 0.0;
                double cand_savings = real_orig_size > 0 ? (100.0 - cand_ratio) : 0.0;
                std::ostringstream oss;
                oss << "{\"prism_version\":" << PRISM_VERSION
                    << ",\"preset\":\"" << (g_fast_mode ? "Fast" : (codecs::g_ultra_mode ? "Ultra" : (codecs::g_comp_level == 5 ? "Normal" : ("Level " + std::to_string(codecs::g_comp_level))))) << "\""
                    << ",\"algorithm\":\"" << alg_esc << "\""
                    << ",\"flags\":" << flags
                    << ",\"orig_bytes\":" << real_orig_size
                    << ",\"payload_bytes\":" << comp_size
                    << ",\"archive_bytes\":" << cand_archive_bytes
                    << ",\"ratio_pct\":" << std::fixed << std::setprecision(4) << cand_ratio
                    << ",\"savings_pct\":" << std::fixed << std::setprecision(4) << cand_savings
                    << ",\"enc_ms\":" << std::fixed << std::setprecision(2) << enc_ms
                    << ",\"threads\":" << threads
                    << ",\"is_dir\":" << (is_dir ? "true" : "false")
                    << ",\"source_name\":\"" << src_esc << "\""
                    << ",\"created_utc\":\"" << utc_buf << "\"}";
                return oss.str();
            };

            uint64_t est_archive_bytes = base_archive_size;
            uint64_t next_archive_bytes = base_archive_size;
            std::string pmet_json;
            for (int iter = 0; iter < 5; ++iter) {
                pmet_json = build_pmet_json(est_archive_bytes);
                next_archive_bytes = base_archive_size + static_cast<uint64_t>(pmet_json.size()) + 16ULL;
                if (next_archive_bytes == est_archive_bytes) {
                    break;
                }
                est_archive_bytes = next_archive_bytes;
            }
            if (next_archive_bytes != est_archive_bytes) {
                uint64_t target_bytes = std::max(est_archive_bytes, next_archive_bytes);
                pmet_json = build_pmet_json(target_bytes);
                uint64_t actual = base_archive_size + static_cast<uint64_t>(pmet_json.size()) + 16ULL;
                if (actual < target_bytes && !pmet_json.empty() && pmet_json.back() == '}') {
                    pmet_json.insert(pmet_json.size() - 1, static_cast<size_t>(target_bytes - actual), ' ');
                }
            }
            total_archive_size += static_cast<uint64_t>(write_pmet_footer(fp, total_archive_size, pmet_json));
        }
    } catch (...) {
        std::fclose(fp);
        throw;
    }
    if (std::fclose(fp) != 0) {
        throw std::runtime_error("Failed to close output archive: " + out_pstr);
    }
    std::cout << "[PROGRESS] 100 Done\n" << std::flush;

    double ratio = real_orig_size > 0 ? (double)total_archive_size / real_orig_size * 100.0 : 0.0;
    std::cout << "[PRISM] Compression successful!\n"
              << "  Original Size : " << real_orig_size << " bytes\n"
              << "  Archive Size  : " << total_archive_size << " bytes (" << ratio << "% of original)\n"
              << "  Time Elapsed  : " << elapsed.count() << " ms\n";
}

void cmd_decompress(const fs::path& input_prism, const fs::path& output_path) {
    std::cout << "[PRISM] Decompressing " << input_prism << " -> " << output_path << " ...\n";
    auto start_time = std::chrono::high_resolution_clock::now();

    auto archive = read_file_raw(input_prism);
    if (archive.size < 28) throw std::runtime_error("Invalid PRISM archive: too short");

    if (std::memcmp(archive.data(), PRISM_MAGIC, 4) != 0) throw std::runtime_error("Invalid PRISM archive magic");

    uint32_t flags = 0; std::memcpy(&flags, archive.data() + 8, 4);
    uint64_t orig_size = 0; std::memcpy(&orig_size, archive.data() + 12, 8);
    uint64_t comp_size = 0; std::memcpy(&comp_size, archive.data() + 20, 8);

    if (28 + comp_size > archive.size) throw std::runtime_error("Invalid PRISM archive truncated");

    const uint8_t* payload = archive.data() + 28;

    bool is_dir = (flags & (1 << 6));
    bool is_cdc = (flags & (1 << 0));
    bool is_tar = (flags & (1 << 1));

    if (!is_dir) {
        ensure_can_overwrite_list({output_path});
    }

    if (!is_cdc && !is_dir && is_tar) {
        decompress_tar_demux_to_file(payload, (size_t)comp_size, orig_size, output_path);
    } else if (is_dir && !is_cdc && is_tar) {
        decompress_tar_demux_to_directory(payload, (size_t)comp_size, orig_size, output_path);
    } else {
        uint32_t dummy_flags = 0;
        uint64_t dummy_orig = 0;
        ByteVec decompressed = decompress_archive_buffer(archive.data(), archive.size, dummy_flags, dummy_orig);

        if (is_dir) {
            unpack_directory(decompressed.data(), decompressed.size(), output_path);
        } else {
            write_file_bytes(output_path, decompressed.data(), decompressed.size());
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end_time - start_time;

    std::cout << "[PRISM] Decompression successful! Time Elapsed: " << elapsed.count() << " ms\n";
}

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        state_[0] = 0x6a09e667u;
        state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u;
        state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu;
        state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu;
        state_[7] = 0x5be0cd19u;
        buf_len_ = 0;
        total_bytes_ = 0;
    }

    void update(const uint8_t* data, size_t len) {
        if (len == 0 || data == nullptr) return;
        total_bytes_ += static_cast<uint64_t>(len);
        size_t offset = 0;
        if (buf_len_ > 0) {
            size_t take = std::min(len, static_cast<size_t>(64) - buf_len_);
            std::memcpy(buf_ + buf_len_, data, take);
            buf_len_ += take;
            offset += take;
            if (buf_len_ == 64) {
                compress_block(state_, buf_);
                buf_len_ = 0;
            }
        }
        while (offset + 64 <= len) {
            compress_block(state_, data + offset);
            offset += 64;
        }
        if (offset < len) {
            buf_len_ = len - offset;
            std::memcpy(buf_, data + offset, buf_len_);
        }
    }

    std::string final_hex() const {
        uint32_t st[8];
        std::memcpy(st, state_, sizeof(st));
        uint8_t block[64];
        std::memcpy(block, buf_, buf_len_);
        size_t blen = buf_len_;

        block[blen++] = 0x80u;
        if (blen > 56) {
            if (blen < 64) {
                std::memset(block + blen, 0, 64 - blen);
            }
            compress_block(st, block);
            blen = 0;
        }
        if (blen < 56) {
            std::memset(block + blen, 0, 56 - blen);
        }

        uint64_t bit_len = total_bytes_ << 3;
        for (int i = 0; i < 8; ++i) {
            block[56 + i] = static_cast<uint8_t>((bit_len >> (56 - i * 8)) & 0xffu);
        }
        compress_block(st, block);

        static constexpr char HEX_DIGITS[] = "0123456789abcdef";
        std::string out(64, '0');
        for (int i = 0; i < 8; ++i) {
            uint32_t v = st[i];
            for (int j = 0; j < 8; ++j) {
                out[static_cast<size_t>(i * 8 + j)] = HEX_DIGITS[(v >> (28 - j * 4)) & 0xfu];
            }
        }
        return out;
    }

private:
    uint32_t state_[8]{};
    uint8_t buf_[64]{};
    size_t buf_len_ = 0;
    uint64_t total_bytes_ = 0;

    static inline uint32_t rotr(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32u - n));
    }

    static void compress_block(uint32_t st[8], const uint8_t block[64]) {
#if defined(__SHA__) && defined(__SSSE3__) && defined(__SSE4_1__)
        const __m128i shuf_mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
        __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(st + 0)), 0xB1);
        __m128i state1 = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(st + 4)), 0xB1);
        __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);
        state1 = _mm_blend_epi16(state1, tmp, 0xF0);
        const __m128i abef_save = state0;
        const __m128i cdgh_save = state1;

        __m128i msg0 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 0)), shuf_mask);
        __m128i msg1 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16)), shuf_mask);
        __m128i msg2 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 32)), shuf_mask);
        __m128i msg3 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 48)), shuf_mask);

        static constexpr uint32_t K4[16][4] = {
            {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u},
            {0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u},
            {0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u},
            {0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u},
            {0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu},
            {0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau},
            {0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u},
            {0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u},
            {0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u},
            {0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u},
            {0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u},
            {0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u},
            {0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u},
            {0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u},
            {0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u},
            {0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u}
        };

        auto round4 = [&](int idx, __m128i m) {
            __m128i msg = _mm_add_epi32(m, _mm_loadu_si128(reinterpret_cast<const __m128i*>(K4[idx])));
            state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        };

        round4(0, msg0);
        round4(1, msg1); msg0 = _mm_sha256msg1_epu32(msg0, msg1);
        round4(2, msg2); msg1 = _mm_sha256msg1_epu32(msg1, msg2);
        round4(3, msg3); msg2 = _mm_sha256msg1_epu32(msg2, msg3);

        for (int r = 4; r < 16; r += 4) {
            msg0 = _mm_sha256msg2_epu32(_mm_add_epi32(msg0, _mm_alignr_epi8(msg3, msg2, 4)), msg3);
            round4(r + 0, msg0);
            if (r + 1 < 13) msg3 = _mm_sha256msg1_epu32(msg3, msg0);

            msg1 = _mm_sha256msg2_epu32(_mm_add_epi32(msg1, _mm_alignr_epi8(msg0, msg3, 4)), msg0);
            round4(r + 1, msg1);
            if (r + 2 < 13) msg0 = _mm_sha256msg1_epu32(msg0, msg1);

            msg2 = _mm_sha256msg2_epu32(_mm_add_epi32(msg2, _mm_alignr_epi8(msg1, msg0, 4)), msg1);
            round4(r + 2, msg2);
            if (r + 3 < 13) msg1 = _mm_sha256msg1_epu32(msg1, msg2);

            msg3 = _mm_sha256msg2_epu32(_mm_add_epi32(msg3, _mm_alignr_epi8(msg2, msg1, 4)), msg2);
            round4(r + 3, msg3);
            if (r + 4 < 13) msg2 = _mm_sha256msg1_epu32(msg2, msg3);
        }

        state0 = _mm_add_epi32(state0, abef_save);
        state1 = _mm_add_epi32(state1, cdgh_save);
        tmp = _mm_shuffle_epi32(state0, 0x1B);
        state1 = _mm_shuffle_epi32(state1, 0xB1);
        state0 = _mm_blend_epi16(tmp, state1, 0xF0);
        state1 = _mm_alignr_epi8(state1, tmp, 8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(st + 0), state0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(st + 4), state1);
        return;
#else
        static constexpr uint32_t K[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            size_t j = static_cast<size_t>(i * 4);
            w[i] = (static_cast<uint32_t>(block[j]) << 24) |
                   (static_cast<uint32_t>(block[j + 1]) << 16) |
                   (static_cast<uint32_t>(block[j + 2]) << 8) |
                   static_cast<uint32_t>(block[j + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
        uint32_t e = st[4], f = st[5], g = st[6], h = st[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        st[0] += a; st[1] += b; st[2] += c; st[3] += d;
        st[4] += e; st[5] += f; st[6] += g; st[7] += h;
#endif
    }
};

void cmd_verify(const fs::path& input_prism) {
    auto start = std::chrono::high_resolution_clock::now();
    uint32_t flags = 0;
    uint64_t orig_size = 0;
    auto decomp = decompress_archive_to_memory(input_prism, flags, orig_size);
    if (decomp.size() != orig_size) throw std::runtime_error("Size verification mismatch");
    bool is_dir = (flags & (1 << 6)) != 0;
    bool has_pdir = is_dir && decomp.size() >= 8 && std::memcmp(decomp.data(), "PDIR", 4) == 0;
    if (has_pdir) {
        uint32_t file_count = 0;
        std::memcpy(&file_count, decomp.data() + 4, 4);
        size_t offset = 8;
        for (uint32_t i = 0; i < file_count; ++i) {
            if (offset + 2 > decomp.size()) throw std::runtime_error("PDIR verify truncated");
            uint16_t path_len = 0;
            std::memcpy(&path_len, decomp.data() + offset, 2);
            offset += 2;
            if (static_cast<size_t>(path_len) + 8 > decomp.size() - offset) throw std::runtime_error("PDIR verify truncated");
            offset += path_len;
            uint64_t fsize = 0;
            std::memcpy(&fsize, decomp.data() + offset, 8);
            offset += 8;
            if (fsize > static_cast<uint64_t>(decomp.size() - offset)) throw std::runtime_error("PDIR verify truncated");
            offset += static_cast<size_t>(fsize);
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> ms = end - start;
    std::cout << "[PRISM] Verify OK! (" << orig_size << " bytes, " << ms.count() << " ms)\n";

    if (g_verify_sha256) {
        Sha256 sha;
        uint64_t content_bytes = 0;
        if (has_pdir) {
            struct VerifyEntry {
                const char* p;
                uint16_t len;
                uint64_t fsize;
                const uint8_t* data;
            };
            uint32_t file_count = 0;
            std::memcpy(&file_count, decomp.data() + 4, 4);
            std::vector<VerifyEntry> entries;
            entries.reserve(file_count);
            size_t offset = 8;
            bool already_sorted = true;
            for (uint32_t i = 0; i < file_count; ++i) {
                uint16_t path_len = 0;
                std::memcpy(&path_len, decomp.data() + offset, 2);
                offset += 2;
                const char* rel_ptr = reinterpret_cast<const char*>(decomp.data() + offset);
                offset += path_len;
                uint64_t fsize = 0;
                std::memcpy(&fsize, decomp.data() + offset, 8);
                offset += 8;
                const uint8_t* fdata = decomp.data() + offset;
                offset += static_cast<size_t>(fsize);
                if (already_sorted && !entries.empty()) {
                    const auto& prev = entries.back();
                    const size_t n = std::min<size_t>(prev.len, path_len);
                    int cmp = (n > 0) ? std::memcmp(prev.p, rel_ptr, n) : 0;
                    if (cmp > 0 || (cmp == 0 && prev.len > path_len)) {
                        already_sorted = false;
                    }
                }
                entries.push_back(VerifyEntry{rel_ptr, path_len, fsize, fdata});
            }
            if (!already_sorted) {
                std::sort(entries.begin(), entries.end(), [](const VerifyEntry& a, const VerifyEntry& b) {
                    const size_t n = std::min<size_t>(a.len, b.len);
                    if (n > 0) {
                        int cmp = std::memcmp(a.p, b.p, n);
                        if (cmp != 0) return cmp < 0;
                    }
                    return a.len < b.len;
                });
            }
            char sz_buf[32];
            for (const auto& e : entries) {
                content_bytes += e.fsize;
                sha.update(reinterpret_cast<const uint8_t*>(e.p), e.len);
                uint64_t v = e.fsize;
                int si = 32;
                do {
                    sz_buf[--si] = static_cast<char>('0' + (v % 10));
                    v /= 10;
                } while (v > 0);
                sha.update(reinterpret_cast<const uint8_t*>(sz_buf + si), static_cast<size_t>(32 - si));
                if (e.fsize > 0) {
                    sha.update(e.data, static_cast<size_t>(e.fsize));
                }
            }
        } else {
            content_bytes = static_cast<uint64_t>(decomp.size());
            sha.update(decomp.data(), decomp.size());
        }
        std::ostringstream oss;
        oss << "VERIFY_JSON\t{\"orig_bytes\":" << orig_size
            << ",\"content_bytes\":" << content_bytes
            << ",\"dec_ms\":" << std::fixed << std::setprecision(3) << ms.count()
            << ",\"sha256\":\"" << sha.final_hex()
            << "\",\"is_dir\":" << (is_dir ? "true" : "false") << "}\n";
        std::cout << oss.str();
    }
}

inline bool try_print_pidx_entries(FILE* fp, uint64_t file_sz, uint64_t comp_size) {
    uint64_t idx_start_offset = 0;
    uint32_t idx_comp_len = 0;
    if (!find_trailer_block(fp, file_sz, comp_size, "PIDX", idx_start_offset, idx_comp_len)) {
        return false;
    }

    ByteVec comp_idx(idx_comp_len);
    if (_fseeki64(fp, (long long)idx_start_offset, SEEK_SET) != 0) return false;
    if (idx_comp_len > 0 && std::fread(comp_idx.data(), 1, idx_comp_len, fp) != idx_comp_len) return false;

    ByteVec idx_raw;
    try {
        idx_raw = codecs::bsc_parallel_decompress(comp_idx.data(), comp_idx.size());
    } catch (...) {
        return false;
    }

    if (idx_raw.size() < 4) return false;
    uint32_t count = 0;
    std::memcpy(&count, idx_raw.data(), 4);
    size_t off = 4;

    std::vector<PidxEntry> parsed;
    parsed.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 2 > idx_raw.size()) return false;
        uint16_t path_len = 0;
        std::memcpy(&path_len, idx_raw.data() + off, 2);
        off += 2;
        if (off + path_len + 9 > idx_raw.size()) return false;
        std::string rpath((const char*)(idx_raw.data() + off), path_len);
        off += path_len;
        uint64_t fsize = 0;
        std::memcpy(&fsize, idx_raw.data() + off, 8);
        off += 8;
        uint8_t kind = idx_raw[off++];
        parsed.push_back({std::move(rpath), fsize, kind});
    }

    for (const auto& e : parsed) {
        std::cout << "ENTRY\t" << e.path << "\t" << e.fsize << "\t" << (e.kind == 1 ? "TAR_ENTRY" : "FILE") << "\n";
    }
    return true;
}

void cmd_list(const fs::path& input_prism) {
    std::string pstr = input_prism.string();
    FILE* fp = prism_fopen(input_prism, "rb");
    if (!fp) throw std::runtime_error("Failed to open archive for listing: " + pstr);

    _fseeki64(fp, 0, SEEK_END);
    long long file_sz_ll = _ftelli64(fp);
    if (file_sz_ll < 28) {
        std::fclose(fp);
        throw std::runtime_error("Invalid PRISM archive: too short");
    }
    uint64_t file_sz = (uint64_t)file_sz_ll;

    uint8_t hdr[28];
    _fseeki64(fp, 0, SEEK_SET);
    if (std::fread(hdr, 1, 28, fp) != 28) {
        std::fclose(fp);
        throw std::runtime_error("Failed to read PRISM archive header");
    }
    if (std::memcmp(hdr, PRISM_MAGIC, 4) != 0) {
        std::fclose(fp);
        throw std::runtime_error("Invalid PRISM archive magic");
    }

    uint32_t flags = 0;
    uint64_t orig_size = 0;
    uint64_t comp_size = 0;
    std::memcpy(&flags, hdr + 8, 4);
    std::memcpy(&orig_size, hdr + 12, 8);
    std::memcpy(&comp_size, hdr + 20, 8);

    bool is_dir = (flags & (1 << 6));
    std::cout << "META\torig_size=" << orig_size << "\tcomp_size=" << comp_size << "\tis_dir=" << (is_dir ? 1 : 0) << "\tflags=" << flags << "\n";

    bool pidx_ok = try_print_pidx_entries(fp, file_sz, comp_size);
    std::fclose(fp);
    if (pidx_ok) return;

    uint32_t decomp_flags = 0;
    uint64_t decomp_orig = 0;
    auto decomp = decompress_archive_to_memory(input_prism, decomp_flags, decomp_orig);
    if (is_dir) {
        if (decomp.size() >= 8 && std::memcmp(decomp.data(), "PDIR", 4) == 0) {
            uint32_t file_count = 0;
            std::memcpy(&file_count, decomp.data() + 4, 4);
            size_t offset = 8;
            for (uint32_t i = 0; i < file_count; ++i) {
                uint16_t path_len = 0;
                std::memcpy(&path_len, decomp.data() + offset, 2);
                offset += 2;
                std::string rpath((const char*)(decomp.data() + offset), path_len);
                offset += path_len;
                uint64_t fsize = 0;
                std::memcpy(&fsize, decomp.data() + offset, 8);
                offset += 8;
                offset += (size_t)fsize;
                std::cout << "ENTRY\t" << rpath << "\t" << fsize << "\tFILE\n";
            }
        }
    } else if (decomp.size() >= 512 && precond::tar_check_header(decomp.data())) {
        size_t t_offset = 0;
        while (t_offset + 512 <= decomp.size()) {
            const char* header = (const char*)(decomp.data() + t_offset);
            if (header[0] == '\0') break;
            std::string name(header, 100);
            size_t null_pos = name.find('\0');
            if (null_pos != std::string::npos) name = name.substr(0, null_pos);
            char size_buf[12];
            std::memcpy(size_buf, header + 124, 11);
            size_buf[11] = '\0';
            uint64_t t_fsize = std::strtoull(size_buf, nullptr, 8);
            char typeflag = header[156];
            t_offset += 512;
            if (typeflag == '0' || typeflag == '\0' || typeflag == '5') {
                if (!name.empty()) {
                    std::cout << "ENTRY\t" << name << "\t" << t_fsize << "\tTAR_ENTRY\n";
                }
            }
            size_t blocks = (t_fsize + 511) / 512;
            t_offset += blocks * 512;
        }
    } else {
        std::cout << "ENTRY\tarchive\t" << orig_size << "\tSINGLE\n";
    }
}

void cmd_extract_one(const fs::path& input_prism, const std::string& entry_path, const fs::path& output_file) {
    ensure_can_overwrite_list({output_file});

    uint32_t flags = 0;
    uint64_t orig_size = 0;
    auto decomp = decompress_archive_to_memory(input_prism, flags, orig_size);
    bool is_dir = (flags & (1 << 6));

    if (is_dir) {
        if (decomp.size() >= 8 && std::memcmp(decomp.data(), "PDIR", 4) == 0) {
            uint32_t file_count = 0;
            std::memcpy(&file_count, decomp.data() + 4, 4);
            size_t offset = 8;
            for (uint32_t i = 0; i < file_count; ++i) {
                uint16_t path_len = 0;
                std::memcpy(&path_len, decomp.data() + offset, 2);
                offset += 2;
                std::string rpath((const char*)(decomp.data() + offset), path_len);
                offset += path_len;
                uint64_t fsize = 0;
                std::memcpy(&fsize, decomp.data() + offset, 8);
                offset += 8;
                const uint8_t* fdata = decomp.data() + offset;
                offset += (size_t)fsize;
                if (rpath == entry_path || fs::path(rpath).generic_string() == fs::path(entry_path).generic_string()) {
                    write_file_bytes(output_file, fdata, (size_t)fsize);
                    std::cout << "[PRISM] Extracted " << entry_path << " -> " << output_file << " (" << fsize << " bytes)\n";
                    return;
                }
            }
        }
        throw std::runtime_error("Entry not found in archive: " + entry_path);
    } else if (decomp.size() >= 512 && precond::tar_check_header(decomp.data())) {
        size_t t_offset = 0;
        while (t_offset + 512 <= decomp.size()) {
            const char* header = (const char*)(decomp.data() + t_offset);
            if (header[0] == '\0') break;
            std::string name(header, 100);
            size_t null_pos = name.find('\0');
            if (null_pos != std::string::npos) name = name.substr(0, null_pos);
            char size_buf[12];
            std::memcpy(size_buf, header + 124, 11);
            size_buf[11] = '\0';
            uint64_t t_fsize = std::strtoull(size_buf, nullptr, 8);
            char typeflag = header[156];
            t_offset += 512;
            const uint8_t* fdata = decomp.data() + t_offset;
            if (typeflag == '0' || typeflag == '\0' || typeflag == '5') {
                if (name == entry_path || fs::path(name).generic_string() == fs::path(entry_path).generic_string()) {
                    write_file_bytes(output_file, fdata, (size_t)t_fsize);
                    std::cout << "[PRISM] Extracted " << entry_path << " -> " << output_file << " (" << t_fsize << " bytes)\n";
                    return;
                }
            }
            size_t blocks = (t_fsize + 511) / 512;
            t_offset += blocks * 512;
        }
        throw std::runtime_error("Entry not found in tar: " + entry_path);
    } else {
        write_file_bytes(output_file, decomp.data(), decomp.size());
        std::cout << "[PRISM] Extracted single file -> " << output_file << " (" << decomp.size() << " bytes)\n";
    }
}

void cmd_extract_many(const fs::path& input_prism, const fs::path& out_dir, const std::vector<std::string>& entries) {
    if (entries.empty()) return;

    std::unordered_set<std::string> wanted;
    wanted.reserve(entries.size() * 2);
    for (const auto& e : entries) {
        wanted.insert(e);
        wanted.insert(fs::path(e).generic_string());
    }

    uint32_t flags = 0;
    uint64_t orig_size = 0;
    auto decomp = decompress_archive_to_memory(input_prism, flags, orig_size);
    bool is_dir = (flags & (1 << 6));

    std::vector<WriteEntry> items;
    std::vector<fs::path> empty_dirs;

    if (is_dir) {
        if (decomp.size() >= 8 && std::memcmp(decomp.data(), "PDIR", 4) == 0) {
            uint32_t file_count = 0;
            std::memcpy(&file_count, decomp.data() + 4, 4);
            size_t offset = 8;
            for (uint32_t i = 0; i < file_count; ++i) {
                if (offset + 2 > decomp.size()) throw std::runtime_error("PDIR truncated at path length");
                uint16_t path_len = 0;
                std::memcpy(&path_len, decomp.data() + offset, 2);
                offset += 2;
                if (offset + path_len + 8 > decomp.size()) throw std::runtime_error("PDIR truncated at path/size");
                std::string rpath((const char*)(decomp.data() + offset), path_len);
                offset += path_len;
                uint64_t fsize = 0;
                std::memcpy(&fsize, decomp.data() + offset, 8);
                offset += 8;
                if (offset + fsize > decomp.size()) throw std::runtime_error("PDIR truncated at file payload");
                const uint8_t* fdata = decomp.data() + offset;
                offset += (size_t)fsize;

                if (wanted.count(rpath) || wanted.count(fs::path(rpath).generic_string())) {
                    items.push_back({out_dir / fs::path(sanitize_rel_path(rpath)), fdata, (size_t)fsize});
                }
            }
        }
    } else if (decomp.size() >= 512 && precond::tar_check_header(decomp.data())) {
        size_t t_offset = 0;
        while (t_offset + 512 <= decomp.size()) {
            const char* header = (const char*)(decomp.data() + t_offset);
            if (header[0] == '\0') break;
            std::string name(header, 100);
            size_t null_pos = name.find('\0');
            if (null_pos != std::string::npos) name = name.substr(0, null_pos);
            char size_buf[12];
            std::memcpy(size_buf, header + 124, 11);
            size_buf[11] = '\0';
            uint64_t t_fsize = std::strtoull(size_buf, nullptr, 8);
            char typeflag = header[156];
            t_offset += 512;
            const uint8_t* fdata = decomp.data() + t_offset;
            if (typeflag == '0' || typeflag == '\0' || typeflag == '5') {
                if (!name.empty() && (wanted.count(name) || wanted.count(fs::path(name).generic_string()))) {
                    std::string clean_name = sanitize_rel_path(name);
                    if (typeflag == '5' || name.back() == '/') {
                        empty_dirs.push_back(out_dir / fs::path(clean_name));
                    } else {
                        items.push_back({out_dir / fs::path(clean_name), fdata, (size_t)t_fsize});
                    }
                }
            }
            size_t blocks = (t_fsize + 511) / 512;
            t_offset += blocks * 512;
        }
    } else {
        if (wanted.count("archive")) {
            items.push_back({out_dir / "archive", decomp.data(), decomp.size()});
        }
    }

    if (items.empty() && empty_dirs.empty()) {
        throw std::runtime_error("No matching entries found in archive");
    }

    if (!items.empty()) {
        write_entries_parallel(out_dir, items);
    }
    for (const auto& d : empty_dirs) {
        prism_create_directories(d);
    }
    std::cout << "[PRISM] Extracted " << (items.size() + empty_dirs.size()) << " entries -> " << out_dir << "\n";
}

struct ArchiveHeaderInfo {
    uint32_t version = 0;
    uint32_t flags = 0;
    uint64_t orig_size = 0;
    uint64_t comp_size = 0;
    uint64_t file_size = 0;
    bool has_pmet = false;
    std::string pmet_json;
};

inline ArchiveHeaderInfo read_archive_header_and_pmet(const fs::path& input_prism) {
    std::string pstr = input_prism.string();
    FILE* fp = prism_fopen(input_prism, "rb");
    if (!fp) throw std::runtime_error("Failed to open PRISM archive: " + pstr);

    uint64_t file_sz = 0;
    uint8_t hdr[28];
    std::string pmet_json;
    bool has_pmet = false;
    try {
        if (_fseeki64(fp, 0, SEEK_END) != 0) {
            throw std::runtime_error("Failed to seek PRISM archive: " + pstr);
        }
        long long file_sz_ll = _ftelli64(fp);
        if (file_sz_ll < 28) {
            throw std::runtime_error("Invalid PRISM archive: too short");
        }
        file_sz = static_cast<uint64_t>(file_sz_ll);

        if (_fseeki64(fp, 0, SEEK_SET) != 0 || std::fread(hdr, 1, 28, fp) != 28) {
            throw std::runtime_error("Failed to read PRISM archive header");
        }
        if (std::memcmp(hdr, PRISM_MAGIC, 4) != 0) {
            throw std::runtime_error("Invalid PRISM archive magic");
        }
        uint64_t comp_size = 0;
        std::memcpy(&comp_size, hdr + 20, 8);
        if (comp_size > file_sz || 28ULL + comp_size > file_sz) {
            throw std::runtime_error("Invalid PRISM archive truncated");
        }
        has_pmet = try_read_pmet_json(fp, file_sz, comp_size, pmet_json);
    } catch (...) {
        std::fclose(fp);
        throw;
    }
    std::fclose(fp);

    ArchiveHeaderInfo info;
    std::memcpy(&info.version, hdr + 4, 4);
    std::memcpy(&info.flags, hdr + 8, 4);
    std::memcpy(&info.orig_size, hdr + 12, 8);
    std::memcpy(&info.comp_size, hdr + 20, 8);
    info.file_size = file_sz;
    info.has_pmet = has_pmet;
    info.pmet_json = std::move(pmet_json);
    return info;
}

void cmd_info(const fs::path& input_prism) {
    ArchiveHeaderInfo info = read_archive_header_and_pmet(input_prism);
    double ratio = info.orig_size > 0
        ? (static_cast<double>(info.file_size) / static_cast<double>(info.orig_size)) * 100.0
        : 0.0;
    double savings = info.orig_size > 0 ? (100.0 - ratio) : 0.0;

    std::ios::fmtflags old_flags = std::cout.flags();
    std::streamsize old_prec = std::cout.precision();

    std::cout << "=== PRISM Archive Info ===\n"
              << "File               : " << input_prism << "\n"
              << "Version            : " << info.version << "\n"
              << "Algorithm          : " << describe_algorithm(info.flags) << "\n"
              << "Flags              : " << info.flags << "\n"
              << "Original Size      : " << info.orig_size << " bytes\n"
              << "Compressed Payload : " << info.comp_size << " bytes\n"
              << "Archive Size       : " << info.file_size << " bytes\n"
              << std::fixed << std::setprecision(2)
              << "Ratio              : " << ratio << "%\n"
              << "Savings            : " << savings << "%\n";
    std::cout.flags(old_flags);
    std::cout.precision(old_prec);

    std::cout << "Is Directory       : " << ((info.flags & (1u << 6)) ? "Yes" : "No") << "\n"
              << "Pipelines          :\n"
              << "  - CDC            : " << ((info.flags & (1u << 0)) ? "Yes" : "No") << "\n"
              << "  - TarDemux       : " << ((info.flags & (1u << 1)) ? "Yes" : "No") << "\n"
              << "  - Stride         : " << ((info.flags & (1u << 2)) ? "Yes" : "No") << "\n"
              << "  - Exec X86       : " << ((info.flags & (1u << 3)) ? "Yes" : "No") << "\n"
              << "  - Exec SPARC     : " << ((info.flags & (1u << 4)) ? "Yes" : "No") << "\n"
              << "  - ZipPrecomp     : " << ((info.flags & (1u << 5)) ? "Yes" : "No") << "\n"
              << "  - Parallel LZMA  : " << ((info.flags & (1u << 7)) ? "Yes" : "No") << "\n"
              << "  - Parallel LZMA2 : " << ((info.flags & (1u << 8)) ? "Yes" : "No") << "\n";

    if (info.has_pmet) {
        std::cout << "\n=== Embedded Compression Metadata ===\n"
                  << "JSON: " << info.pmet_json << "\n";
    }
}

void cmd_meta(const fs::path& input_prism) {
    ArchiveHeaderInfo info = read_archive_header_and_pmet(input_prism);
    double ratio = info.orig_size > 0
        ? (static_cast<double>(info.file_size) / static_cast<double>(info.orig_size)) * 100.0
        : 0.0;
    double savings = info.orig_size > 0 ? (100.0 - ratio) : 0.0;
    std::string file_utf8 =
#ifdef _WIN32
        win_wide_to_utf8(input_prism.wstring().c_str());
#else
        input_prism.string();
#endif
    bool valid_pmet = info.has_pmet && !info.pmet_json.empty() &&
                      info.pmet_json.front() == '{' && info.pmet_json.back() == '}';

    std::ostringstream oss;
    oss << "{\"file\":\"" << json_escape(file_utf8) << "\""
        << ",\"version\":" << info.version
        << ",\"flags\":" << info.flags
        << ",\"is_dir\":" << ((info.flags & (1u << 6)) ? "true" : "false")
        << ",\"orig_bytes\":" << info.orig_size
        << ",\"payload_bytes\":" << info.comp_size
        << ",\"archive_bytes\":" << info.file_size
        << ",\"ratio_pct\":" << std::fixed << std::setprecision(4) << ratio
        << ",\"savings_pct\":" << std::fixed << std::setprecision(4) << savings
        << ",\"algorithm\":\"" << json_escape(describe_algorithm(info.flags)) << "\""
        << ",\"has_pmet\":" << (valid_pmet ? "true" : "false")
        << ",\"pmet\":" << (valid_pmet ? info.pmet_json : "null")
        << "}";
    std::cout << oss.str() << "\n" << std::flush;
}

} // namespace prism

void print_usage() {
    std::cout << "PRISM High-Performance Compressor v1.0\n"
              << "Usage:\n"
              << "  prism c <input_path> <output.prism> [--fast|-f] [--ultra|-u] [--meta|-m] [-y|--yes|--force] [-n|--no-overwrite]\n"
              << "  prism d <input.prism> <output_path> [-y|--yes|--force] [-n|--no-overwrite]\n"
              << "  prism i <input.prism>\n"
              << "  prism meta <input.prism>\n"
              << "  prism t <input.prism> [--sha256]\n"
              << "  prism l <input.prism>\n"
              << "  prism x1 <input.prism> <entry_path> <output_file> [-y|--yes|--force] [-n|--no-overwrite]\n"
              << "  prism xm <input.prism> <out_dir> <entry1> [entry2 ...] [-y|--yes|--force] [-n|--no-overwrite]\n";
}

int main(int argc, char* argv[]) {
    try {
        prism::g_fast_mode = false;
        prism::codecs::g_ultra_mode = false;
        prism::codecs::g_comp_level = 5;
        prism::g_overwrite_mode = prism::OverwriteMode::AskInteractive;
        prism::g_embed_meta = false;
        prism::g_verify_sha256 = false;

        std::vector<std::string> pos;
        pos.reserve((size_t)argc);
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--fast" || arg == "-f") {
                prism::g_fast_mode = true;
                prism::codecs::g_ultra_mode = false;
                prism::codecs::g_comp_level = 1;
            } else if (arg == "--normal") {
                prism::g_fast_mode = false;
                prism::codecs::g_ultra_mode = false;
                prism::codecs::g_comp_level = 5;
            } else if (arg == "--ultra" || arg == "-u") {
                prism::g_fast_mode = false;
                prism::codecs::g_ultra_mode = true;
                prism::codecs::g_comp_level = 9;
            } else if (arg == "--level" && i + 1 < argc) {
                int lvl = std::clamp(std::atoi(argv[++i]), 1, 9);
                prism::codecs::g_comp_level = lvl;
                prism::g_fast_mode = (lvl <= 2);
                prism::codecs::g_ultra_mode = (lvl >= 9);
            } else if (arg.size() == 2 && arg[0] == '-' && arg[1] >= '1' && arg[1] <= '9') {
                int lvl = arg[1] - '0';
                prism::codecs::g_comp_level = lvl;
                prism::g_fast_mode = (lvl <= 2);
                prism::codecs::g_ultra_mode = (lvl >= 9);
            } else if (arg == "--meta" || arg == "-m") {
                prism::g_embed_meta = true;
            } else if (arg == "--sha256") {
                prism::g_verify_sha256 = true;
            } else if (arg == "-y" || arg == "--yes" || arg == "--force") {
                prism::g_overwrite_mode = prism::OverwriteMode::ForceYes;
            } else if (arg == "-n" || arg == "--no-overwrite") {
                prism::g_overwrite_mode = prism::OverwriteMode::NoClobber;
            } else {
                pos.push_back(std::move(arg));
            }
        }

        if (pos.size() < 2) {
            print_usage();
            return 1;
        }

        std::string cmd = pos[0];
        if (cmd == "c" || cmd == "a") {
            if (pos.size() < 3) throw std::runtime_error("Missing input or output archive path for compression");
            fs::path in = pos[1];
            fs::path out = pos[2];
            prism::cmd_compress(in, out);
        } else if (cmd == "d" || cmd == "x") {
            if (pos.size() < 3) throw std::runtime_error("Missing output path for decompression");
            fs::path in = pos[1];
            fs::path out = pos[2];
            prism::cmd_decompress(in, out);
        } else if (cmd == "i") {
            fs::path in = pos[1];
            prism::cmd_info(in);
        } else if (cmd == "meta") {
            fs::path in = pos[1];
            prism::cmd_meta(in);
        } else if (cmd == "t") {
            fs::path in = pos[1];
            prism::cmd_verify(in);
        } else if (cmd == "l") {
            fs::path in = pos[1];
            prism::cmd_list(in);
        } else if (cmd == "x1") {
            if (pos.size() < 4) throw std::runtime_error("Missing entry path or output file for x1");
            fs::path in = pos[1];
            std::string entry = pos[2];
            fs::path out = pos[3];
            prism::cmd_extract_one(in, entry, out);
        } else if (cmd == "xm") {
            if (pos.size() < 4) throw std::runtime_error("Missing out_dir or entries for xm");
            fs::path in = pos[1];
            fs::path out_dir = pos[2];
            std::vector<std::string> entries;
            entries.reserve(pos.size() - 3);
            for (size_t i = 3; i < pos.size(); ++i) {
                entries.emplace_back(pos[i]);
            }
            prism::cmd_extract_many(in, out_dir, entries);
        } else {
            std::cerr << "Unknown command: " << cmd << "\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "[PRISM ERROR] " << e.what() << "\n";
        return 1;
    }
    std::cout.flush();
    std::fflush(stdout);
    if (std::getenv("PRISM_NO_FAST_EXIT")) return 0;
    std::_Exit(0);
}
