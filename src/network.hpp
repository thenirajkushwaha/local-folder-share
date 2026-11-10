#pragma once
#include "protocol.hpp"
#include "file_util.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

class NetworkManager {
public:
    using ProgressCallback = std::function<void(const std::string& current_item, uint64_t bytes_sent_or_recv, uint64_t total_bytes, bool done, const std::string& err)>;
    using PeerUpdateCallback = std::function<void(const std::vector<PeerInfo>& peers)>;

    NetworkManager(uint16_t discovery_port = DEFAULT_DISCOVERY_PORT, uint16_t transfer_port = DEFAULT_TRANSFER_PORT);
    ~NetworkManager();

    // Start auto-discovery background thread (broadcasts & listens)
    bool start_discovery(bool is_client);
    void stop_discovery();
    void set_is_client(bool is_client);

    // Get current discovered peers
    std::vector<PeerInfo> get_peers();
    void set_peer_callback(PeerUpdateCallback cb);

    // Server mode: listen for incoming transfers on transfer_port
    bool start_server(const std::string& save_dir, ProgressCallback progress_cb);
    void stop_server();
    bool is_server_running() const { return server_running_.load(); }

    // Client/Sender mode: send files/folders to a remote peer (IP & port)
    bool send_paths(const std::string& peer_ip, uint16_t peer_port, const std::vector<std::string>& local_paths, ProgressCallback progress_cb);

private:
    void discovery_loop();
    void server_listen_loop(std::string save_dir, ProgressCallback progress_cb);
    void handle_incoming_connection(int client_fd, const std::string& save_dir, ProgressCallback progress_cb);

    uint16_t discovery_port_;
    uint16_t transfer_port_;

    std::atomic<bool> discovery_running_{false};
    std::atomic<bool> server_running_{false};
    std::atomic<bool> is_client_{true};

    int udp_sock_{-1};
    int server_sock_{-1};

    std::mutex peers_mutex_;
    std::vector<PeerInfo> peers_;
    PeerUpdateCallback peer_cb_{nullptr};
};
