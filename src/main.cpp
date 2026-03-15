#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <future>
#include <clocale>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <CLI/CLI.hpp>
#include <asio.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "ZSend/config/ConfigManager.hpp"
#include "ZSend/utils/NicknameGenerator.hpp"
#include "ZSend/network/Discovery.hpp"
#include "ZSend/network/Transfer.hpp"

using namespace ZSend;

// Global state
Config::AppConfig g_config;
std::vector<Network::Peer> g_peers;
std::mutex g_peers_mutex;
constexpr uint16_t kServicePort = 53317;

void SetupLogging() {
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");
}

void RunInteractive(Network::Discovery& /*discovery*/, Network::Sender& sender, Network::Receiver& receiver);

int main(int argc, char** argv) {
    // Enable UTF-8 console output on Windows
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF8");
#endif

    SetupLogging();

    // 1. Load Config
    g_config = Config::ConfigManager::Load();
    spdlog::info("Welcome, {}!", g_config.nickname);

    // 2. Setup Network Context
    asio::io_context ioc;
    
    // 3. Components
    // Discovery runs on UDP port 53317
    spdlog::info("Initializing Discovery...");
    std::unique_ptr<Network::Discovery> discovery;
    std::unique_ptr<Network::Sender> sender;
    std::unique_ptr<Network::Receiver> receiver;

    try {
        discovery = std::make_unique<Network::Discovery>(ioc, kServicePort, g_config);
        spdlog::info("Discovery initialized.");
        
        // Sender/Receiver (Receiver listens on TCP port 53317)
        spdlog::info("Initializing Sender...");
        sender = std::make_unique<Network::Sender>(ioc);
        spdlog::info("Sender initialized.");

        spdlog::info("Initializing Receiver...");
        receiver = std::make_unique<Network::Receiver>(ioc, kServicePort, g_config.download_dir);
        spdlog::info("Receiver initialized.");
    } catch (const std::exception& e) {
        spdlog::critical("Initialization failed: {}", e.what());
        return -1;
    } catch (...) {
        spdlog::critical("Initialization failed with unknown error");
        return -1;
    }

    // 4. Peer Discovery Callback
    discovery->SetOnPeerFound([](const Network::Peer& peer) {
        std::lock_guard<std::mutex> lock(g_peers_mutex);
        bool found = false;
        for (auto& p : g_peers) {
            bool same_uuid = !p.uuid.empty() && !peer.uuid.empty() && p.uuid == peer.uuid;
            bool same_ip = p.ip == peer.ip;
            if (same_uuid || same_ip) {
                p.last_seen = peer.last_seen;
                p.nickname = peer.nickname;
                p.ip = peer.ip;
                p.port = peer.port;
                p.uuid = peer.uuid;
                found = true;
                break;
            }
        }
        if (!found) {
            g_peers.push_back(peer);
        }
    });

    // 5. Start Background Thread
    discovery->Start();
    std::thread io_thread([&ioc]() { ioc.run(); });

    // 6. CLI
    CLI::App app{"ZSend - LAN File Transfer"};
    app.require_subcommand(0, 1);

    auto send_cmd = app.add_subcommand("send", "Send file");
    std::string filepath, target_ip;
    send_cmd->add_option("-f,--file", filepath, "File path")->required()->check(CLI::ExistingFile);
    send_cmd->add_option("-t,--target", target_ip, "Target IP");

    auto recv_cmd = app.add_subcommand("recv", "Receive mode");

    int exit_code = 0;
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        exit_code = app.exit(e);
    }

    if (exit_code == 0) {
        if (send_cmd->parsed()) {
            if (target_ip.empty()) {
                spdlog::error("Target IP required for non-interactive mode");
                exit_code = 1;
            } else {
                std::promise<bool> done;
                auto future = done.get_future();
                
                sender->Connect(target_ip, kServicePort, [&](bool success) {
                    if (success) {
                        sender->SendFile(filepath, 
                            [](uint64_t s, uint64_t t, double speed) {
                                 std::cout << "\rProgress: " << (s * 100 / t) << "% " << std::fixed << std::setprecision(1) << speed << " MB/s" << std::flush;
                            },
                            [&](bool success) {
                                std::cout << std::endl;
                                done.set_value(success);
                            }
                        );
                    } else {
                        done.set_value(false);
                    }
                });
                
                if (!future.get()) {
                    spdlog::error("Transfer failed");
                    exit_code = 1;
                }
            }
            
        } else if (recv_cmd->parsed()) {
            std::cout << "Listening... Press Ctrl+C to exit.\n";
            
            std::atomic<bool> running{true};
            
            std::function<void()> start_listen;
            start_listen = [&]() {
                receiver->Start(
                    [](const std::string& name, uint64_t size, const std::string& sender) {
                        spdlog::info("Incoming: {} ({}) from {}", name, size, sender);
                        return true;
                    },
                    [](uint64_t s, uint64_t t, double speed) {
                         std::cout << "\rReceiving: " << (s * 100 / t) << "% " << std::fixed << std::setprecision(1) << speed << " MB/s" << std::flush;
                    },
                    [&](bool success) {
                        std::cout << std::endl;
                        if (success) spdlog::info("Finished.");
                        else spdlog::error("Failed.");
                        if (running) start_listen();
                    }
                );
            };
            
            start_listen();
            
            while(running) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            
        } else {
            RunInteractive(*discovery, *sender, *receiver);
        }
    }

    // Cleanup
    discovery->Stop();
    ioc.stop();
    if (io_thread.joinable()) io_thread.join();

    return exit_code;
}

void RunInteractive(Network::Discovery& /*discovery*/, Network::Sender& sender, Network::Receiver& receiver) {
    while (true) {
        std::cout << "\n=== ZSend (" << g_config.nickname << ") ===\n";
        std::cout << "1. Send File\n";
        std::cout << "2. Receive File (Listen)\n";
        std::cout << "3. List Peers\n";
        std::cout << "0. Exit\n";
        std::cout << "> ";
        
        int choice;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::cin.ignore(1000, '\n');
            continue;
        }

        if (choice == 0) break;
        
        if (choice == 1) { // Send
            std::string path;
            std::cout << "Enter file path: ";
            std::cin >> path;
            
            // Show peers
            std::vector<Network::Peer> current_peers;
            {
                std::lock_guard<std::mutex> lock(g_peers_mutex);
                current_peers = g_peers;
            }

            if (current_peers.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2200));
                std::lock_guard<std::mutex> lock(g_peers_mutex);
                current_peers = g_peers;
            }
            
            if (current_peers.empty()) {
                std::cout << "No peers found. Enter IP manually: ";
                std::string ip;
                std::cin >> ip;
                
                std::atomic<bool> done{false};
                sender.Connect(ip, kServicePort, [&](bool success) {
                    if (success) {
                        sender.SendFile(path, 
                            [](uint64_t s, uint64_t t, double speed) {
                                std::cout << "\rProgress: " << (s * 100 / t) << "% " << speed << " MB/s" << std::flush;
                            },
                            [&](bool) { done = true; }
                        );
                    } else {
                        std::cout << "Connect failed\n";
                        done = true;
                    }
                });
                
                while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::cout << "\nDone.\n";
                
            } else {
                std::cout << "Select peer:\n";
                for (size_t i = 0; i < current_peers.size(); ++i) {
                    std::cout << (i + 1) << ". " << current_peers[i].nickname << " (" << current_peers[i].ip << ")\n";
                }
                std::cout << (current_peers.size() + 1) << ". Manual IP\n";
                
                int p_idx;
                std::cin >> p_idx;
                std::string target_ip;
                
                if (p_idx > 0 && p_idx <= (int)current_peers.size()) {
                    target_ip = current_peers[p_idx - 1].ip;
                } else {
                    std::cout << "Enter IP: ";
                    std::cin >> target_ip;
                }
                
                std::atomic<bool> done{false};
                sender.Connect(target_ip, kServicePort, [&](bool success) {
                    if (success) {
                        sender.SendFile(path, 
                            [](uint64_t s, uint64_t t, double speed) {
                                std::cout << "\rProgress: " << (s * 100 / t) << "% " << speed << " MB/s" << std::flush;
                            },
                            [&](bool) { done = true; }
                        );
                    } else {
                        std::cout << "Connect failed\n";
                        done = true;
                    }
                });
                
                while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::cout << "\nDone.\n";
            }
            
        } else if (choice == 2) { // Receive
            std::cout << "Listening... (Press Enter to stop)\n";
            std::atomic<bool> done{false};
            
            receiver.Start(
                [](const std::string& name, uint64_t size, const std::string&) {
                    std::cout << "\nIncoming: " << name << " (" << size << " bytes). Accept? (y/n): ";
                    char c;
                    std::cin >> c;
                    return (c == 'y' || c == 'Y');
                },
                [](uint64_t s, uint64_t t, double speed) {
                     std::cout << "\rReceiving: " << (s * 100 / t) << "% " << speed << " MB/s" << std::flush;
                },
                [&](bool) {
                    done = true;
                }
            );
            
            while (!done) {
                // How to interrupt? 
                // receiver.Cancel() can be called from another thread.
                // But here we are blocking on 'done'.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::cout << "\nFinished.\n";
            
        } else if (choice == 3) {
            std::lock_guard<std::mutex> lock(g_peers_mutex);
            std::cout << "Peers:\n";
            for (const auto& p : g_peers) {
                std::cout << "- " << p.nickname << " (" << p.ip << ")\n";
            }
        }
    }
}
