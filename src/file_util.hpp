#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <sys/stat.h>

struct FileEntry {
    std::string rel_path;       // Relative path within folder or file name
    std::string full_path;      // Absolute or source path on disk
    bool is_directory;
    uint64_t size;
    uint32_t mode;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t atime_sec;
    int64_t atime_nsec;
};

namespace FileUtil {
    // Collect all entries under root_path (recursively if directory)
    // base_name can be used to set root folder name in target
    bool collect_entries(const std::string& root_path, std::vector<FileEntry>& out_entries, std::string& out_err);

    // Get default Downloads directory (~/Downloads)
    std::string get_downloads_dir();

    // Create directories recursively
    bool create_directories(const std::string& path);

    // Set file / directory metadata (permissions, timestamps)
    bool apply_metadata(const std::string& path, uint32_t mode, int64_t mtime_sec, int64_t mtime_nsec, int64_t atime_sec, int64_t atime_nsec);

    // Get local hostname
    std::string get_local_hostname();

    // Format bytes for display (e.g. 1.2 MB)
    std::string format_bytes(uint64_t bytes);

    // URL decode for file:/// drop paths
    std::string url_decode(const std::string& in);
}
