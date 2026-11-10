#include "cli.hpp"
#include "network.hpp"
#include "file_util.hpp"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

namespace {
    std::atomic<bool> g_cli_running{true};

    void sig_handler(int) {
        g_cli_running.store(false);
    }
}

static void print_usage(const char* prog) {
    std::cout << "========================================================\n"
              << "          Arch Linux Peer-to-Peer File Transfer         \n"
              << "========================================================\n"
              << "Usage:\n"
              << "  " << prog << " --cli receive              Run as Client (receiver, saves to ~/Downloads)\n"
              << "  " << prog << " --cli list                 Discover peers on the local network\n"
              << "  " << prog << " --cli send <peer-idx|ip> <path1> [path2 ...]\n"
              << "                              Send file(s) or folder(s) to a peer\n"
              << "  " << prog << "                           Run GUI mode (Default)\n"
              << "========================================================\n";
}

int cli_main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string command = argv[1];

    if (command == "receive" || command == "--receive" || command == "-r") {
        signal(SIGINT, sig_handler);
        signal(SIGTERM, sig_handler);

        std::string save_dir = FileUtil::get_downloads_dir();
        std::cout << "[Client Mode] Starting receiver...\n";
        std::cout << "[Client Mode] Saving incoming files to: " << save_dir << "\n";
        std::cout << "[Client Mode] Hostname: " << FileUtil::get_local_hostname() << "\n";

        NetworkManager net;
        net.start_discovery(true); // Broadcast that we are a client receiver

        bool started = net.start_server(save_dir, [](const std::string& item, uint64_t bytes, uint64_t total, bool done, const std::string& err) {
            (void)total;
            if (!err.empty()) {
                std::cout << "\n[Error] " << err << "\n";
            } else if (done) {
                std::cout << "\n[✓] " << item << " (" << FileUtil::format_bytes(bytes) << " received)\n";
            } else {
                std::cout << "\r[↓] " << item << " [" << FileUtil::format_bytes(bytes) << "]" << std::flush;
            }
        });

        if (!started) {
            std::cerr << "Failed to start listening server on port " << DEFAULT_TRANSFER_PORT << "\n";
            return 1;
        }

        std::cout << "[Client Mode] Listening on TCP port " << DEFAULT_TRANSFER_PORT << " and broadcasting presence on UDP " << DEFAULT_DISCOVERY_PORT << "\n";
        std::cout << "Press Ctrl+C to stop.\n";

        while (g_cli_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::cout << "\nStopping receiver...\n";
        net.stop_server();
        net.stop_discovery();
        return 0;
    } else if (command == "list" || command == "--list" || command == "-l") {
        std::cout << "[Discovery] Scanning local network for peers (3 seconds)...\n";
        NetworkManager net;
        net.start_discovery(false);

        for (int i = 0; i < 15; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        auto peers = net.get_peers();
        std::cout << "\nDiscovered Peers (" << peers.size() << " found):\n";
        std::cout << "--------------------------------------------------------\n";
        std::cout << std::left << std::setw(6) << "Index" 
                  << std::setw(25) << "Hostname" 
                  << std::setw(18) << "IP Address" 
                  << std::setw(8)  << "Port" << "\n";
        std::cout << "--------------------------------------------------------\n";

        for (size_t i = 0; i < peers.size(); ++i) {
            std::cout << std::left << std::setw(6) << ("[" + std::to_string(i + 1) + "]")
                      << std::setw(25) << peers[i].hostname
                      << std::setw(18) << peers[i].ip
                      << std::setw(8)  << peers[i].port << "\n";
        }
        std::cout << "--------------------------------------------------------\n";

        net.stop_discovery();
        return 0;
    } else if (command == "send" || command == "--send" || command == "-s") {
        if (argc < 4) {
            std::cout << "Usage: " << argv[0] << " --cli send <peer-idx|ip> <path1> [path2 ...]\n";
            return 1;
        }

        std::string target = argv[2];
        std::vector<std::string> paths_to_send;
        for (int i = 3; i < argc; ++i) {
            paths_to_send.push_back(argv[i]);
        }

        NetworkManager net;
        net.start_discovery(false);

        std::string target_ip = target;
        uint16_t target_port = DEFAULT_TRANSFER_PORT;

        // Check if target is an index
        bool is_num = true;
        for (char c : target) {
            if (!isdigit(c)) { is_num = false; break; }
        }

        if (is_num) {
            std::cout << "[Sender] Discovering peers to resolve index " << target << "...\n";
            for (int i = 0; i < 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            auto peers = net.get_peers();
            int idx = std::stoi(target) - 1;
            if (idx >= 0 && idx < static_cast<int>(peers.size())) {
                target_ip = peers[idx].ip;
                target_port = peers[idx].port;
                std::cout << "[Sender] Selected peer: " << peers[idx].hostname << " (" << target_ip << ":" << target_port << ")\n";
            } else {
                std::cerr << "Invalid peer index: " << target << ". Discovered " << peers.size() << " peers.\n";
                return 1;
            }
        }

        std::cout << "[Sender] Connecting to " << target_ip << ":" << target_port << "...\n";
        bool ok = net.send_paths(target_ip, target_port, paths_to_send, [](const std::string& item, uint64_t bytes, uint64_t total, bool done, const std::string& err) {
            if (!err.empty()) {
                std::cout << "\n[Error] " << err << "\n";
            } else if (done) {
                std::cout << "\n[✓] " << item << " (" << FileUtil::format_bytes(bytes) << " sent)\n";
            } else {
                double pct = (total > 0) ? (100.0 * bytes / total) : 0.0;
                std::cout << "\r[↑] " << item << " [" << FileUtil::format_bytes(bytes) << " / " << FileUtil::format_bytes(total) 
                          << "] (" << std::fixed << std::setprecision(1) << pct << "%)" << std::flush;
            }
        });

        net.stop_discovery();
        return ok ? 0 : 1;
    } else {
        print_usage(argv[0]);
        return 1;
    }
}
