#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <future>
#include <clocale>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <CLI/CLI.hpp>
#include <asio.hpp>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
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

namespace {
std::filesystem::path GetExecutablePath() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) return {};

    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) return {};
    }

    buffer.resize(size);
    return std::filesystem::path(buffer);
#elif defined(__APPLE__)
    std::vector<char> buffer(1024);
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    }

    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), ec);
    return ec ? std::filesystem::path(buffer.data()) : canonical;
#else
    std::array<char, PATH_MAX> buffer{};
    const auto len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0) return {};
    buffer[static_cast<size_t>(len)] = '\0';
    return std::filesystem::path(buffer.data());
#endif
}

std::filesystem::path GetExecutableDir() {
    auto exe_path = GetExecutablePath();
    if (exe_path.empty()) return std::filesystem::current_path();

    auto exe_dir = exe_path.parent_path();
    return exe_dir.empty() ? std::filesystem::current_path() : exe_dir;
}
} // namespace

void SetupLogging() {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);
    console_sink->set_pattern("[%H:%M:%S] [%^%l%$] %v");

    std::vector<spdlog::sink_ptr> sinks{console_sink};

    auto log_dir = GetExecutableDir() / "log";
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec) {
        std::cerr << "Failed to create log directory '" << log_dir.u8string() << "': " << ec.message() << "\n";
    } else {
        try {
            const auto log_file = (log_dir / "zsend.log").u8string();
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file, 5 * 1024 * 1024, 3);
            file_sink->set_level(spdlog::level::debug);
            file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
            sinks.push_back(file_sink);
        } catch (const std::exception& e) {
            std::cerr << "Failed to create log file sink: " << e.what() << "\n";
        }
    }

    auto logger = std::make_shared<spdlog::logger>("zsend", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_on(spdlog::level::info);

    spdlog::info("Log directory: {}", log_dir.u8string());
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
    spdlog::info("Download dir: {}", g_config.download_dir);

    // 2. Setup Network Context
    asio::io_context ioc;
    
    // 3. Components
    // Discovery runs on UDP port 8888
    Network::Discovery discovery(ioc, 8888, g_config);
    
    // Sender/Receiver (Receiver listens on TCP port 8888)
    Network::Sender sender(ioc);
    Network::Receiver receiver(ioc, 8888, g_config.download_dir);

    // 4. Peer Discovery Callback
    discovery.SetOnPeerFound([](const Network::Peer& peer) {
        bool is_new = false;
        {
            std::lock_guard<std::mutex> lock(g_peers_mutex);
            bool found = false;
            for (auto& p : g_peers) {
                if (p.ip == peer.ip) {
                    p.last_seen = peer.last_seen;
                    p.nickname = peer.nickname;
                    found = true;
                    break;
                }
            }
            if (!found) {
                g_peers.push_back(peer);
                is_new = true;
            }
        }
        if (is_new) spdlog::info("Peer discovered: {} ({})", peer.nickname, peer.ip);
    });

    // 5. Start Background Thread
    discovery.Start();
    std::thread io_thread([&ioc]() { ioc.run(); });

    // 6. CLI
    CLI::App app{"ZSend - LAN File Transfer"};
    app.require_subcommand(0, 1);

    auto send_cmd = app.add_subcommand("send", "Send file");
    std::string filepath, target_ip;
    send_cmd->add_option("-f,--file", filepath, "File path")->required()->check(CLI::ExistingFile);
    send_cmd->add_option("-t,--target", target_ip, "Target IP");

    auto recv_cmd = app.add_subcommand("recv", "Receive mode");

    CLI11_PARSE(app, argc, argv);

    if (send_cmd->parsed()) {
        if (target_ip.empty()) {
            spdlog::error("Target IP required for non-interactive mode");
            return 1;
        }
        std::promise<bool> done;
        auto future = done.get_future();
        
        sender.Connect(target_ip, 8888, [&](bool success) {
            if (success) {
                sender.SendFile(filepath, 
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
        }
        
    } else if (recv_cmd->parsed()) {
        std::cout << "Listening... Press Ctrl+C to exit.\n";
        
        // Simple loop to restart receiver after completion
        // Note: This is a hacky way to loop.
        std::atomic<bool> running{true};
        
        std::function<void()> start_listen;
        start_listen = [&]() {
            receiver.Start(
                [](const std::string& name, uint64_t size, const std::string& sender) {
                    spdlog::info("Incoming: {} ({}) from {}", name, size, sender);
                    return true; // Auto-accept
                },
                [](uint64_t s, uint64_t t, double speed) {
                     std::cout << "\rReceiving: " << (s * 100 / t) << "% " << std::fixed << std::setprecision(1) << speed << " MB/s" << std::flush;
                },
                [&](bool success) {
                    std::cout << std::endl;
                    if (success) spdlog::info("Finished.");
                    else spdlog::error("Failed.");
                    if (running) start_listen(); // Restart
                }
            );
        };
        
        start_listen();
        
        // Block main thread
        while(running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } else {
        RunInteractive(discovery, sender, receiver);
    }

    // Cleanup
    discovery.Stop();
    ioc.stop();
    if (io_thread.joinable()) io_thread.join();

    return 0;
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
                std::cout << "No peers found. Enter IP manually: ";
                std::string ip;
                std::cin >> ip;
                
                std::atomic<bool> done{false};
                sender.Connect(ip, 8888, [&](bool success) {
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
                sender.Connect(target_ip, 8888, [&](bool success) {
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
