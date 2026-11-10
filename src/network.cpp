#include "network.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <ifaddrs.h>

namespace fs = std::filesystem;

namespace {
    // Helper to send all bytes over TCP
    bool send_exact(int sock, const void* data, size_t len) {
        const char* ptr = static_cast<const char*>(data);
        size_t total = 0;
        while (total < len) {
            ssize_t n = send(sock, ptr + total, len - total, 0);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    // Helper to receive exact bytes over TCP
    bool recv_exact(int sock, void* data, size_t len) {
        char* ptr = static_cast<char*>(data);
        size_t total = 0;
        while (total < len) {
            ssize_t n = recv(sock, ptr + total, len - total, 0);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    // Get current monotonic seconds
    int64_t get_now_sec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Check if IP belongs to local machine
    bool is_local_ip(const std::string& ip_str) {
        if (ip_str == "127.0.0.1" || ip_str == "localhost") return true;
        struct ifaddrs *ifaddr, *ifa;
        if (getifaddrs(&ifaddr) == -1) return false;
        bool found = false;
        for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                char host[NI_MAXHOST];
                if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0) {
                    if (ip_str == host) {
                        found = true;
                        break;
                    }
                }
            }
        }
        freeifaddrs(ifaddr);
        return found;
    }
}

NetworkManager::NetworkManager(uint16_t discovery_port, uint16_t transfer_port)
    : discovery_port_(discovery_port), transfer_port_(transfer_port) {}

NetworkManager::~NetworkManager() {
    stop_discovery();
    stop_server();
}

void NetworkManager::set_is_client(bool is_client) {
    is_client_.store(is_client);
}

void NetworkManager::set_peer_callback(PeerUpdateCallback cb) {
    peer_cb_ = cb;
}

std::vector<PeerInfo> NetworkManager::get_peers() {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return peers_;
}

bool NetworkManager::start_discovery(bool is_client) {
    if (discovery_running_.load()) return true;

    is_client_.store(is_client);

    // Create UDP socket
    udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock_ < 0) {
        perror("UDP socket creation failed");
        return false;
    }

    // Allow broadcast
    int broadcast = 1;
    if (setsockopt(udp_sock_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        perror("setsockopt SO_BROADCAST failed");
        close(udp_sock_);
        return false;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    // Bind to 0.0.0.0:discovery_port_
    struct sockaddr_in recv_addr{};
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(discovery_port_);
    recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(udp_sock_, (struct sockaddr*)&recv_addr, sizeof(recv_addr)) < 0) {
        perror("UDP bind failed");
        close(udp_sock_);
        return false;
    }

    // Set non-blocking or timeout
    struct timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    discovery_running_.store(true);

    std::thread([this]() {
        discovery_loop();
    }).detach();

    return true;
}

void NetworkManager::stop_discovery() {
    if (discovery_running_.exchange(false)) {
        if (udp_sock_ >= 0) {
            close(udp_sock_);
            udp_sock_ = -1;
        }
    }
}

void NetworkManager::discovery_loop() {
    std::string my_hostname = FileUtil::get_local_hostname();
    auto last_broadcast = std::chrono::steady_clock::now() - std::chrono::seconds(5);

    // Prepare broadcast destination 255.255.255.255
    struct sockaddr_in bcast_addr{};
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_port = htons(discovery_port_);
    bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    while (discovery_running_.load()) {
        auto now = std::chrono::steady_clock::now();

        // Broadcast presence every 2 seconds
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_broadcast).count() >= 2) {
            last_broadcast = now;

            DiscoveryPacket pkt{};
            pkt.magic = DISCOVERY_MAGIC;
            pkt.transfer_port = transfer_port_;
            pkt.is_client = is_client_.load() ? 1 : 0;
            strncpy(pkt.hostname, my_hostname.c_str(), sizeof(pkt.hostname) - 1);

            sendto(udp_sock_, &pkt, sizeof(pkt), 0, (struct sockaddr*)&bcast_addr, sizeof(bcast_addr));
        }

        // Receive incoming broadcast packets
        DiscoveryPacket in_pkt{};
        struct sockaddr_in sender_addr{};
        socklen_t addr_len = sizeof(sender_addr);

        ssize_t n = recvfrom(udp_sock_, &in_pkt, sizeof(in_pkt), 0, (struct sockaddr*)&sender_addr, &addr_len);
        if (n == sizeof(DiscoveryPacket) && in_pkt.magic == DISCOVERY_MAGIC) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sender_addr.sin_addr), ip_str, INET_ADDRSTRLEN);

            // Skip messages from self (unless debugging on loopback)
            if (!is_local_ip(ip_str) || in_pkt.transfer_port != transfer_port_) {
                in_pkt.hostname[sizeof(in_pkt.hostname) - 1] = '\0';
                std::string host = in_pkt.hostname;
                int64_t cur_time = get_now_sec();

                bool updated = false;
                std::vector<PeerInfo> current_list;
                {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    bool found = false;
                    for (auto& p : peers_) {
                        if (p.ip == ip_str && p.port == in_pkt.transfer_port) {
                            p.hostname = host;
                            p.last_seen = cur_time;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        PeerInfo new_peer;
                        new_peer.hostname = host;
                        new_peer.ip = ip_str;
                        new_peer.port = in_pkt.transfer_port;
                        new_peer.last_seen = cur_time;
                        peers_.push_back(new_peer);
                        updated = true;
                    }

                    // Remove stale peers (> 8 seconds without broadcast)
                    auto it = peers_.begin();
                    while (it != peers_.end()) {
                        if (cur_time - it->last_seen > 8) {
                            it = peers_.erase(it);
                            updated = true;
                        } else {
                            ++it;
                        }
                    }
                    current_list = peers_;
                }

                if (updated && peer_cb_) {
                    peer_cb_(current_list);
                }
            }
        }

        // Clean stale peers periodically even if no packets received
        {
            int64_t cur_time = get_now_sec();
            bool changed = false;
            std::vector<PeerInfo> current_list;
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                auto it = peers_.begin();
                while (it != peers_.end()) {
                    if (cur_time - it->last_seen > 8) {
                        it = peers_.erase(it);
                        changed = true;
                    } else {
                        ++it;
                    }
                }
                current_list = peers_;
            }
            if (changed && peer_cb_) {
                peer_cb_(current_list);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

bool NetworkManager::start_server(const std::string& save_dir, ProgressCallback progress_cb) {
    if (server_running_.load()) return true;

    server_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock_ < 0) {
        perror("TCP server socket failed");
        return false;
    }

    int reuse = 1;
    setsockopt(server_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(transfer_port_);

    if (bind(server_sock_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("TCP server bind failed");
        close(server_sock_);
        server_sock_ = -1;
        return false;
    }

    if (listen(server_sock_, 10) < 0) {
        perror("TCP server listen failed");
        close(server_sock_);
        server_sock_ = -1;
        return false;
    }

    server_running_.store(true);

    std::thread([this, save_dir, progress_cb]() {
        server_listen_loop(save_dir, progress_cb);
    }).detach();

    return true;
}

void NetworkManager::stop_server() {
    if (server_running_.exchange(false)) {
        if (server_sock_ >= 0) {
            close(server_sock_);
            server_sock_ = -1;
        }
    }
}

void NetworkManager::server_listen_loop(std::string save_dir, ProgressCallback progress_cb) {
    while (server_running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_sock_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (!server_running_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Spawn thread to handle transfer
        std::thread([this, client_fd, save_dir, progress_cb]() {
            handle_incoming_connection(client_fd, save_dir, progress_cb);
            close(client_fd);
        }).detach();
    }
}

void NetworkManager::handle_incoming_connection(int client_fd, const std::string& save_dir, ProgressCallback progress_cb) {
    // Make sure destination folder exists
    FileUtil::create_directories(save_dir);

    uint64_t total_received_bytes = 0;
    std::vector<std::pair<std::string, PacketHeader>> applied_meta_queue;

    while (true) {
        PacketHeader hdr{};
        if (!recv_exact(client_fd, &hdr, sizeof(hdr))) {
            break;
        }

        if (hdr.entry_type == static_cast<uint8_t>(EntryType::END_OF_BATCH)) {
            // End of batch received!
            break;
        }

        if (hdr.path_len > 4096) {
            if (progress_cb) progress_cb("", total_received_bytes, total_received_bytes, true, "Invalid path length received");
            return;
        }

        std::string rel_path(hdr.path_len, '\0');
        if (!recv_exact(client_fd, &rel_path[0], hdr.path_len)) {
            if (progress_cb) progress_cb("", total_received_bytes, total_received_bytes, true, "Failed receiving path name");
            return;
        }

        fs::path dest_path = fs::path(save_dir) / rel_path;

        if (hdr.entry_type == static_cast<uint8_t>(EntryType::DIRECTORY)) {
            FileUtil::create_directories(dest_path.string());
            applied_meta_queue.push_back({dest_path.string(), hdr});
            if (progress_cb) progress_cb("Directory: " + rel_path, total_received_bytes, 0, false, "");
        } else if (hdr.entry_type == static_cast<uint8_t>(EntryType::REGULAR_FILE)) {
            FileUtil::create_directories(dest_path.parent_path().string());

            std::ofstream out_file(dest_path, std::ios::binary);
            if (!out_file.is_open()) {
                if (progress_cb) progress_cb(rel_path, total_received_bytes, 0, true, "Cannot create file: " + dest_path.string());
                return;
            }

            constexpr size_t CHUNK_SIZE = 65536;
            char buffer[CHUNK_SIZE];
            uint64_t remaining = hdr.file_size;

            while (remaining > 0) {
                size_t to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
                if (!recv_exact(client_fd, buffer, to_read)) {
                    out_file.close();
                    if (progress_cb) progress_cb(rel_path, total_received_bytes, 0, true, "Connection dropped during file transfer");
                    return;
                }
                out_file.write(buffer, to_read);
                remaining -= to_read;
                total_received_bytes += to_read;

                if (progress_cb) {
                    progress_cb("Receiving: " + rel_path, total_received_bytes, 0, false, "");
                }
            }
            out_file.close();

            // Store for metadata restoration
            applied_meta_queue.push_back({dest_path.string(), hdr});
        }
    }

    // Apply metadata (permissions and timestamps) in reverse order so directory timestamps aren't modified by child writes
    for (auto it = applied_meta_queue.rbegin(); it != applied_meta_queue.rend(); ++it) {
        FileUtil::apply_metadata(it->first, it->second.mode, it->second.mtime_sec, it->second.mtime_nsec, it->second.atime_sec, it->second.atime_nsec);
    }

    if (progress_cb) {
        progress_cb("Transfer complete!", total_received_bytes, total_received_bytes, true, "");
    }
}

bool NetworkManager::send_paths(const std::string& peer_ip, uint16_t peer_port, const std::vector<std::string>& local_paths, ProgressCallback progress_cb) {
    if (local_paths.empty()) {
        if (progress_cb) progress_cb("", 0, 0, true, "No files or folders selected");
        return false;
    }

    // Collect all files/directories to send
    std::vector<FileEntry> all_entries;
    uint64_t total_bytes_to_send = 0;

    for (const auto& path : local_paths) {
        std::vector<FileEntry> entries;
        std::string err;
        if (!FileUtil::collect_entries(path, entries, err)) {
            if (progress_cb) progress_cb("", 0, 0, true, err);
            return false;
        }
        for (const auto& e : entries) {
            if (!e.is_directory) {
                total_bytes_to_send += e.size;
            }
            all_entries.push_back(e);
        }
    }

    // Connect to peer TCP server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        if (progress_cb) progress_cb("", 0, 0, true, "Failed to create TCP socket");
        return false;
    }

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &serv_addr.sin_addr) <= 0) {
        close(sock);
        if (progress_cb) progress_cb("", 0, 0, true, "Invalid peer IP address");
        return false;
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        if (progress_cb) progress_cb("", 0, 0, true, "Could not connect to peer " + peer_ip + ":" + std::to_string(peer_port));
        return false;
    }

    uint64_t bytes_sent_so_far = 0;

    for (const auto& entry : all_entries) {
        PacketHeader hdr{};
        hdr.entry_type = entry.is_directory ? static_cast<uint8_t>(EntryType::DIRECTORY) : static_cast<uint8_t>(EntryType::REGULAR_FILE);
        hdr.path_len = static_cast<uint32_t>(entry.rel_path.length());
        hdr.file_size = entry.is_directory ? 0 : entry.size;
        hdr.mode = entry.mode;
        hdr.mtime_sec = entry.mtime_sec;
        hdr.mtime_nsec = entry.mtime_nsec;
        hdr.atime_sec = entry.atime_sec;
        hdr.atime_nsec = entry.atime_nsec;

        if (!send_exact(sock, &hdr, sizeof(hdr))) {
            close(sock);
            if (progress_cb) progress_cb(entry.rel_path, bytes_sent_so_far, total_bytes_to_send, true, "Failed sending header");
            return false;
        }

        if (!send_exact(sock, entry.rel_path.data(), entry.rel_path.length())) {
            close(sock);
            if (progress_cb) progress_cb(entry.rel_path, bytes_sent_so_far, total_bytes_to_send, true, "Failed sending path string");
            return false;
        }

        if (!entry.is_directory) {
            std::ifstream in_file(entry.full_path, std::ios::binary);
            if (!in_file.is_open()) {
                close(sock);
                if (progress_cb) progress_cb(entry.rel_path, bytes_sent_so_far, total_bytes_to_send, true, "Failed opening local file: " + entry.full_path);
                return false;
            }

            constexpr size_t CHUNK_SIZE = 65536;
            char buffer[CHUNK_SIZE];
            uint64_t file_remaining = entry.size;

            while (file_remaining > 0) {
                size_t to_read = (file_remaining < CHUNK_SIZE) ? file_remaining : CHUNK_SIZE;
                in_file.read(buffer, to_read);
                size_t read_bytes = in_file.gcount();

                if (read_bytes == 0) break;

                if (!send_exact(sock, buffer, read_bytes)) {
                    in_file.close();
                    close(sock);
                    if (progress_cb) progress_cb(entry.rel_path, bytes_sent_so_far, total_bytes_to_send, true, "Connection dropped while sending file data");
                    return false;
                }

                file_remaining -= read_bytes;
                bytes_sent_so_far += read_bytes;

                if (progress_cb) {
                    progress_cb("Sending: " + entry.rel_path, bytes_sent_so_far, total_bytes_to_send, false, "");
                }
            }
            in_file.close();
        } else {
            if (progress_cb) {
                progress_cb("Sending Directory: " + entry.rel_path, bytes_sent_so_far, total_bytes_to_send, false, "");
            }
        }
    }

    // Send END_OF_BATCH
    PacketHeader end_hdr{};
    end_hdr.entry_type = static_cast<uint8_t>(EntryType::END_OF_BATCH);
    send_exact(sock, &end_hdr, sizeof(end_hdr));

    close(sock);

    if (progress_cb) {
        progress_cb("Transfer complete!", total_bytes_to_send, total_bytes_to_send, true, "");
    }
    return true;
}
