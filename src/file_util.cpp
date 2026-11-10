#include "file_util.hpp"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <utime.h>
#include <fcntl.h>

namespace fs = std::filesystem;

namespace FileUtil {

bool collect_entries(const std::string& root_path, std::vector<FileEntry>& out_entries, std::string& out_err) {
    out_entries.clear();
    fs::path p(root_path);

    std::error_code ec;
    if (!fs::exists(p, ec)) {
        out_err = "Path does not exist: " + root_path;
        return false;
    }

    // Helper to get stat info
    auto get_stat = [](const fs::path& path, FileEntry& entry) -> bool {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            return false;
        }
        entry.size = static_cast<uint64_t>(st.st_size);
        entry.mode = static_cast<uint32_t>(st.st_mode);
        entry.mtime_sec = static_cast<int64_t>(st.st_mtim.tv_sec);
        entry.mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
        entry.atime_sec = static_cast<int64_t>(st.st_atim.tv_sec);
        entry.atime_nsec = static_cast<int64_t>(st.st_atim.tv_nsec);
        return true;
    };

    if (fs::is_directory(p, ec)) {
        // Collect root directory itself as first entry
        std::string base_dir_name = p.filename().string();
        if (base_dir_name.empty()) {
            base_dir_name = p.parent_path().filename().string();
        }
        if (base_dir_name.empty()) {
            base_dir_name = "shared_folder";
        }

        FileEntry root_entry;
        root_entry.rel_path = base_dir_name;
        root_entry.full_path = p.string();
        root_entry.is_directory = true;
        if (!get_stat(p, root_entry)) {
            out_err = "Failed to stat root directory: " + root_path;
            return false;
        }
        out_entries.push_back(root_entry);

        // Walk recursively
        for (const auto& dir_entry : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            fs::path item_path = dir_entry.path();
            fs::path rel_to_root = fs::relative(item_path, p, ec);
            std::string final_rel = (fs::path(base_dir_name) / rel_to_root).string();

            FileEntry entry;
            entry.rel_path = final_rel;
            entry.full_path = item_path.string();
            entry.is_directory = dir_entry.is_directory(ec);
            if (!get_stat(item_path, entry)) {
                continue; // Skip inaccessible items
            }
            out_entries.push_back(entry);
        }
    } else {
        // Single file
        FileEntry entry;
        entry.rel_path = p.filename().string();
        entry.full_path = p.string();
        entry.is_directory = false;
        if (!get_stat(p, entry)) {
            out_err = "Failed to stat file: " + root_path;
            return false;
        }
        out_entries.push_back(entry);
    }

    return true;
}

std::string get_downloads_dir() {
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    if (home) {
        fs::path dl = fs::path(home) / "Downloads";
        return dl.string();
    }
    return "./Downloads";
}

bool create_directories(const std::string& path) {
    std::error_code ec;
    return fs::create_directories(path, ec) || fs::exists(path, ec);
}

bool apply_metadata(const std::string& path, uint32_t mode, int64_t mtime_sec, int64_t mtime_nsec, int64_t atime_sec, int64_t atime_nsec) {
    // Apply permissions (masking standard permission bits 07777)
    chmod(path.c_str(), mode & 07777);

    // Apply times using utimensat
    struct timespec times[2];
    times[0].tv_sec = atime_sec;
    times[0].tv_nsec = atime_nsec;
    times[1].tv_sec = mtime_sec;
    times[1].tv_nsec = mtime_nsec;

    utimensat(AT_FDCWD, path.c_str(), times, 0);
    return true;
}

std::string get_local_hostname() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostname[sizeof(hostname) - 1] = '\0';
        return std::string(hostname);
    }
    return "arch-linux";
}

std::string format_bytes(uint64_t bytes) {
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * 1024.0;
    constexpr double GB = 1024.0 * 1024.0 * 1024.0;

    char buf[64];
    if (bytes >= GB) {
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / GB);
    } else if (bytes >= MB) {
        snprintf(buf, sizeof(buf), "%.2f MB", bytes / MB);
    } else if (bytes >= KB) {
        snprintf(buf, sizeof(buf), "%.2f KB", bytes / KB);
    } else {
        snprintf(buf, sizeof(buf), "%lu B", (unsigned long)bytes);
    }
    return std::string(buf);
}

std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); ++i) {
        if (in[i] == '%' && i + 2 < in.length()) {
            int value = 0;
            std::istringstream is(in.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                out += static_cast<char>(value);
                i += 2;
            } else {
                out += in[i];
            }
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

} // namespace FileUtil
