#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <future>
#include <clocale>
#include <filesystem>
#include <memory>
#include <cstdlib>

#include <spdlog/spdlog.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
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

void SetupLogging() {
    std::filesystem::create_directories("log");
    std::vector<spdlog::sink_ptr> sinks;
    auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    sinks.push_back(stderr_sink);
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("log/zesnd.log", true);
    sinks.push_back(file_sink);
#if defined(__APPLE__) && defined(ZSEND_ASAN_ENABLED)
    const char* user = std::getenv("USER");
    const std::string username = (user != nullptr && user[0] != '\0') ? user : "unknown";
    auto tmp_file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("/tmp/zsend_" + username + ".log", true);
    sinks.push_back(tmp_file_sink);
#endif
    auto logger = std::make_shared<spdlog::logger>("zsend", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("%Y-%m-%dT%H:%M:%S.%e %^%l%$ %t %@ %v");
    spdlog::flush_on(spdlog::level::info);
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
    SPDLOG_INFO("[HEARTBEAT] main-start");

    // 1. Load Config
    g_config = Config::ConfigManager::Load();
    SPDLOG_INFO("Welcome, {}!", g_config.nickname);
    const uint16_t service_port = g_config.service_port;
    SPDLOG_INFO("Service port: {}", service_port);

    // 2. Setup Network Context
    asio::io_context ioc;
    
    // 3. Components
    // Discovery runs on UDP port 53317
    SPDLOG_DEBUG("Initializing Discovery...");
    std::unique_ptr<Network::Discovery> discovery;
    std::unique_ptr<Network::Sender> sender;
    std::unique_ptr<Network::Receiver> receiver;

    try {
        discovery = std::make_unique<Network::Discovery>(ioc, service_port, g_config);
        SPDLOG_DEBUG("Discovery initialized.");
        
        // Sender/Receiver (Receiver listens on TCP port 53317)
        SPDLOG_DEBUG("Initializing Sender...");
        sender = std::make_unique<Network::Sender>(ioc);
        SPDLOG_DEBUG("Sender initialized.");

        SPDLOG_DEBUG("Initializing Receiver...");
        receiver = std::make_unique<Network::Receiver>(ioc, service_port, g_config.download_dir);
        SPDLOG_DEBUG("Receiver initialized.");
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("Initialization failed: {}", e.what());
        return -1;
    } catch (...) {
        SPDLOG_CRITICAL("Initialization failed with unknown error");
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
    std::thread io_thread([&ioc]() {
        SPDLOG_INFO("[HEARTBEAT] io-thread-start");
        ioc.run();
    });

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
                SPDLOG_ERROR("Target IP required for non-interactive mode");
                exit_code = 1;
            } else {
                std::promise<bool> done;
                auto future = done.get_future();
                
                sender->Connect(target_ip, service_port, [&](bool success) {
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

                SPDLOG_INFO("[UI_BLOCK] reason=future_wait_send_done");
                if (!future.get()) {
                    SPDLOG_ERROR("Transfer failed");
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
                        SPDLOG_DEBUG("Incoming: {} ({}) from {}", name, size, sender);
                        return true;
                    },
                    [](uint64_t s, uint64_t t, double speed) {
                         std::cout << "\rReceiving: " << (s * 100 / t) << "% " << std::fixed << std::setprecision(1) << speed << " MB/s" << std::flush;
                    },
                    [&](bool success) {
                        std::cout << std::endl;
                        if (success) SPDLOG_DEBUG("Finished.");
                        else SPDLOG_ERROR("Failed.");
                        if (running) start_listen();
                    }
                );
            };
            
            start_listen();
            
            SPDLOG_INFO("[UI_BLOCK] reason=recv_loop_wait");
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
                sender.Connect(ip, g_config.service_port, [&](bool success) {
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
                
                SPDLOG_INFO("[UI_BLOCK] reason=wait_send_done");
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
                sender.Connect(target_ip, g_config.service_port, [&](bool success) {
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
                
                SPDLOG_INFO("[UI_BLOCK] reason=wait_send_done");
                while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::cout << "\nDone.\n";
            }
            
        } else if (choice == 2) { // Receive
            std::cout << "Listening... (Press Enter to stop)\n";
            std::atomic<bool> done{false};
            
            receiver.Start(
                [](const std::string& name, uint64_t size, const std::string&) {
                    SPDLOG_INFO("[UI_BLOCK] reason=stdin_wait_confirm");
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
            
            SPDLOG_INFO("[UI_BLOCK] reason=wait_receive_done");
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
