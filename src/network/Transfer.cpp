#include "ZSend/network/Transfer.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <thread>
#include <array>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace ZSend {
namespace Network {

namespace {
bool ShouldEmitKeyLog(const std::string& step, const std::string& filename) {
    static std::mutex dedupe_mutex;
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_emit;
    const auto key = step + "|" + filename;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(dedupe_mutex);
    auto it = last_emit.find(key);
    if (it != last_emit.end() && (now - it->second) < std::chrono::seconds(10)) {
        return false;
    }
    last_emit[key] = now;
    return true;
}

std::string ComputeDebugHash(std::ifstream& file) {
    constexpr uint64_t offset_basis = 1469598103934665603ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    uint64_t hash = offset_basis;
    std::array<char, 64 * 1024> buffer{};
    auto original_pos = file.tellg();
    file.clear();
    file.seekg(0, std::ios::beg);
    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        const auto count = static_cast<size_t>(file.gcount());
        for (size_t i = 0; i < count; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= prime;
        }
    }
    file.clear();
    if (original_pos != std::ifstream::pos_type(-1)) {
        file.seekg(original_pos);
    } else {
        file.seekg(0, std::ios::beg);
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}
}

// --- Sender ---

Sender::Sender(asio::io_context& ioc) : socket_(ioc) {}

void Sender::Connect(const std::string& ip, int port, std::function<void(bool success)> cb) {
    // Run in a thread to avoid blocking if called from main
    std::thread([this, ip, port, cb]() {
        SPDLOG_INFO("[HEARTBEAT] sender-connect-thread-start");
        try {
            asio::ip::tcp::endpoint endpoint(asio::ip::make_address(ip), port);
            socket_.connect(endpoint);
            connected_ = true;
            if (cb) cb(true);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Connect failed: {}", e.what());
            if (cb) cb(false);
        }
    }).detach();
}

void Sender::SendFile(const std::string& filepath, ProgressCallback progress_cb, std::function<void(bool success)> done_cb) {
    std::thread([this, filepath, progress_cb, done_cb]() {
        SPDLOG_INFO("[HEARTBEAT] sender-send-thread-start");
        if (!connected_) {
            SPDLOG_ERROR("Not connected");
            if (done_cb) done_cb(false);
            return;
        }

        try {
            std::ifstream file(filepath, std::ios::binary | std::ios::ate);
            if (!file) {
                SPDLOG_ERROR("Cannot open file: {}", filepath);
                if (done_cb) done_cb(false);
                return;
            }

            uint64_t filesize = file.tellg();
            file.seekg(0, std::ios::beg);
            
            std::string filename = std::filesystem::path(filepath).filename().string();
            const auto hash_hex = ComputeDebugHash(file);
            file.seekg(0, std::ios::beg);

            asio::ip::tcp::endpoint peer_endpoint;
            std::error_code peer_ec;
            peer_endpoint = socket_.remote_endpoint(peer_ec);
            if (!peer_ec) {
                SPDLOG_INFO("[PROPOSAL] target={}:{} file={} size={} hash={}",
                            peer_endpoint.address().to_string(),
                            peer_endpoint.port(),
                            filename,
                            filesize,
                            hash_hex);
            } else {
                SPDLOG_INFO("[PROPOSAL] target=<unknown> file={} size={} hash={}",
                            filename,
                            filesize,
                            hash_hex);
            }

            if (ShouldEmitKeyLog("SEND_START", filename)) {
                SPDLOG_INFO("[SEND_START] file={} size={}", filename, filesize);
            }

            // 1. Handshake
            nlohmann::json handshake;
            handshake["filename"] = filename;
            handshake["filesize"] = filesize;
            std::string body = handshake.dump();

            Protocol::Header header;
            header.magic = Protocol::MAGIC;
            header.version = Protocol::VERSION;
            header.type = (uint16_t)Protocol::PacketType::Handshake;
            header.body_len = body.size();
            header.checksum = 0; // TODO

            SPDLOG_DEBUG("Sending handshake for {}", filename);
            asio::write(socket_, asio::buffer(&header, sizeof(header)));
            asio::write(socket_, asio::buffer(body));

            // 2. Wait Accept
            Protocol::Header resp_header;
            asio::read(socket_, asio::buffer(&resp_header, sizeof(resp_header)));
            if (resp_header.type != (uint16_t)Protocol::PacketType::Accept) {
                SPDLOG_WARN("Transfer rejected");
                if (done_cb) done_cb(false);
                return;
            }

            // 3. Send Data Header
            header.type = (uint16_t)Protocol::PacketType::Data;
            header.body_len = filesize;
            asio::write(socket_, asio::buffer(&header, sizeof(header)));

            // 4. Stream File
            std::vector<char> buffer(64 * 1024); // 64KB
            uint64_t sent = 0;
            auto start_time = std::chrono::steady_clock::now();

            while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
                size_t n = file.gcount();
                if (n == 0) break;
                
                asio::write(socket_, asio::buffer(buffer.data(), n));
                sent += n;

                if (progress_cb) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
                    double speed = 0;
                    if (elapsed > 0) {
                        speed = (double)sent / (elapsed / 1000.0) / (1024.0 * 1024.0); // MB/s
                    }
                    progress_cb(sent, filesize, speed);
                }
            }

            // 5. Wait Ack
            asio::read(socket_, asio::buffer(&resp_header, sizeof(resp_header)));
            if (resp_header.type == (uint16_t)Protocol::PacketType::Ack) {
                if (ShouldEmitKeyLog("SEND_DONE", filename)) {
                    SPDLOG_INFO("[SEND_DONE] file={} size={}", filename, filesize);
                }
                if (done_cb) done_cb(true);
            } else {
                SPDLOG_ERROR("Transfer failed (No Ack)");
                if (done_cb) done_cb(false);
            }

        } catch (const std::exception& e) {
            SPDLOG_ERROR("SendFile exception: {}", e.what());
            if (done_cb) done_cb(false);
        }
    }).detach();
}

void Sender::Cancel() {
    socket_.close();
}

// --- Receiver ---

Receiver::Receiver(asio::io_context& ioc, int port, const std::string& save_dir)
    : acceptor_(ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)), socket_(ioc), save_dir_(save_dir) {}

void Receiver::Start(RequestCallback request_cb, ProgressCallback progress_cb, std::function<void(bool success)> done_cb) {
    request_cb_ = request_cb;
    progress_cb_ = progress_cb;
    done_cb_ = done_cb;
    DoAccept();
}

void Receiver::DoAccept() {
    acceptor_.async_accept(socket_, [this](const std::error_code& error) {
        if (!error) {
            // Move socket to thread for blocking handling
            // Note: socket_ is a member, so we can't move it easily if we want to accept more?
            // For MVP one transfer at a time is fine.
            std::thread([this]() {
                SPDLOG_INFO("[HEARTBEAT] receiver-session-thread-start");
                HandleSession();
            }).detach();
        } else {
            SPDLOG_ERROR("Accept failed: {}", error.message());
        }
    });
}

void Receiver::HandleSession() {
    try {
        // 1. Read Handshake
        Protocol::Header header;
        asio::read(socket_, asio::buffer(&header, sizeof(header)));
        
        if (header.magic != Protocol::MAGIC) throw std::runtime_error("Invalid Magic");
        if (header.type != (uint16_t)Protocol::PacketType::Handshake) throw std::runtime_error("Expected Handshake");

        std::vector<char> body(header.body_len);
        asio::read(socket_, asio::buffer(body));
        
        auto j = nlohmann::json::parse(body);
        std::string filename = j["filename"];
        uint64_t filesize = j["filesize"];
        std::error_code remote_ec;
        auto remote_endpoint = socket_.remote_endpoint(remote_ec);
        if (!remote_ec) {
            SPDLOG_INFO("[PROPOSAL_RX] from={}:{} file={} size={}",
                        remote_endpoint.address().to_string(),
                        remote_endpoint.port(),
                        filename,
                        filesize);
        } else {
            SPDLOG_INFO("[PROPOSAL_RX] from=<unknown> file={} size={}", filename, filesize);
        }
        if (ShouldEmitKeyLog("RECV_START", filename)) {
            SPDLOG_INFO("[RECV_START] file={} size={}", filename, filesize);
        }
        
        // Callback to user
        bool accepted = true;
        if (request_cb_) {
            accepted = request_cb_(filename, filesize, "Unknown");
        }

        if (!accepted) {
            Protocol::Header resp;
            resp.magic = Protocol::MAGIC;
            resp.version = Protocol::VERSION;
            resp.type = (uint16_t)Protocol::PacketType::Reject;
            resp.body_len = 0;
            resp.checksum = 0;
            asio::write(socket_, asio::buffer(&resp, sizeof(resp)));
            SPDLOG_DEBUG("Receiver rejected {}", filename);
            if (done_cb_) done_cb_(false);
            return;
        }

        // Send Accept
        Protocol::Header resp;
        resp.magic = Protocol::MAGIC;
        resp.version = Protocol::VERSION;
        resp.type = (uint16_t)Protocol::PacketType::Accept;
        resp.body_len = 0;
        resp.checksum = 0;
        asio::write(socket_, asio::buffer(&resp, sizeof(resp)));

        // 2. Read Data Header
        asio::read(socket_, asio::buffer(&header, sizeof(header)));
        if (header.type != (uint16_t)Protocol::PacketType::Data) throw std::runtime_error("Expected Data");
        
        // 3. Stream to File
        std::filesystem::path path = std::filesystem::path(save_dir_) / filename;
        std::ofstream file(path, std::ios::binary);
        
        std::vector<char> buffer(64 * 1024);
        uint64_t received = 0;
        uint64_t total = header.body_len;
        auto start_time = std::chrono::steady_clock::now();

        while (received < total) {
            size_t to_read = (size_t)std::min((uint64_t)buffer.size(), total - received);
            asio::read(socket_, asio::buffer(buffer.data(), to_read));
            file.write(buffer.data(), to_read);
            received += to_read;

            if (progress_cb_) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
                double speed = 0;
                if (elapsed > 0) {
                    speed = (double)received / (elapsed / 1000.0) / (1024.0 * 1024.0); // MB/s
                }
                progress_cb_(received, total, speed);
            }
        }

        // 4. Send Ack
        resp.type = (uint16_t)Protocol::PacketType::Ack;
        asio::write(socket_, asio::buffer(&resp, sizeof(resp)));
        if (ShouldEmitKeyLog("RECV_DONE", filename)) {
            SPDLOG_INFO("[RECV_DONE] file={} size={}", filename, total);
        }
        
        if (done_cb_) done_cb_(true);

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Receiver exception: {}", e.what());
        if (done_cb_) done_cb_(false);
    }
}

void Receiver::Cancel() {
    socket_.close();
    acceptor_.close();
}

} // namespace Network
} // namespace ZSend
