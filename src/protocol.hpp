#pragma once
#include <cstdint>
#include <string>

// Default ports
constexpr uint16_t DEFAULT_DISCOVERY_PORT = 50000;
constexpr uint16_t DEFAULT_TRANSFER_PORT  = 50001;

// Information about a discovered peer
struct PeerInfo {
    std::string hostname;   // Host name of the peer
    std::string ip;         // IPv4 address as string (e.g., "192.168.1.5")
    uint16_t    port;       // TCP listening port for file transfer
    int64_t     last_seen;  // Timestamp of last discovery ping
};

// Protocol magic headers
constexpr uint32_t PROTOCOL_MAGIC = 0x54524E53; // "TRNS"
constexpr uint32_t DISCOVERY_MAGIC = 0x44495343; // "DISC"

enum class EntryType : uint8_t {
    REGULAR_FILE = 1,
    DIRECTORY    = 2,
    END_OF_BATCH = 255
};

// Wire format structures (packed / fixed byte sizes)
#pragma pack(push, 1)

struct DiscoveryPacket {
    uint32_t magic;         // DISCOVERY_MAGIC
    uint16_t transfer_port; // TCP port open for transfers
    uint16_t is_client;     // 1 if ready to receive, 0 if sender only
    char hostname[64];      // null-terminated hostname
};

struct PacketHeader {
    uint8_t entry_type;     // EntryType (REGULAR_FILE, DIRECTORY, END_OF_BATCH)
    uint32_t path_len;      // length of UTF-8 relative path
    uint64_t file_size;     // size in bytes (0 for directory)
    uint32_t mode;          // file permission and mode bits
    int64_t  mtime_sec;     // modification time (seconds)
    int64_t  mtime_nsec;    // modification time (nanoseconds)
    int64_t  atime_sec;     // access time (seconds)
    int64_t  atime_nsec;    // access time (nanoseconds)
};

#pragma pack(pop)
