#include "ZSend/network/Transfer.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <thread>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace ZSend {
namespace Network {

// --- Sender ---

Sender::Sender(asio::io_context& ioc) : socket_(ioc) {}

void Sender::Connect(const std::string& ip, int port, std::function<void(bool success)> cb) {
    // Run in a thread to avoid blocking if called from main
    std::thread([this, ip, port, cb]() {
        try {
            spdlog::info("Connecting to {}:{} ...", ip, port);
            asio::ip::tcp::endpoint endpoint(asio::ip::make_address(ip), port);
            socket_.connect(endpoint);
            connected_.store(true, std::memory_order_relaxed);
            spdlog::info("Connected to {}:{}", ip, port);
            if (cb) cb(true);
        } catch (const std::exception& e) {
            connected_.store(false, std::memory_order_relaxed);
            spdlog::error("Connect to {}:{} failed: {}", ip, port, e.what());
            if (cb) cb(false);
        }
    }).detach();
}

void Sender::SendFile(const std::string& filepath, ProgressCallback progress_cb, std::function<void(bool success)> done_cb) {
    std::thread([this, filepath, progress_cb, done_cb]() {
        if (!connected_.load(std::memory_order_relaxed)) {
            spdlog::error("Not connected");
            if (done_cb) done_cb(false);
            return;
        }

        try {
            std::ifstream file(filepath, std::ios::binary | std::ios::ate);
            if (!file) {
                spdlog::error("Cannot open file: {}", filepath);
                if (done_cb) done_cb(false);
                return;
            }

            uint64_t filesize = file.tellg();
            file.seekg(0, std::ios::beg);
            
            std::string filename = std::filesystem::path(filepath).filename().string();
            spdlog::info("Sending '{}' ({} bytes)", filename, filesize);

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

            asio::write(socket_, asio::buffer(&header, sizeof(header)));
            asio::write(socket_, asio::buffer(body));

            // 2. Wait Accept
            Protocol::Header resp_header;
            asio::read(socket_, asio::buffer(&resp_header, sizeof(resp_header)));
            if (resp_header.type != (uint16_t)Protocol::PacketType::Accept) {
                spdlog::warn("Transfer rejected by receiver");
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
                spdlog::info("Transfer complete!");
                if (done_cb) done_cb(true);
            } else {
                spdlog::error("Transfer failed (No Ack)");
                if (done_cb) done_cb(false);
            }

        } catch (const std::exception& e) {
            spdlog::error("SendFile exception: {}", e.what());
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

    std::error_code ec;
    auto ep = acceptor_.local_endpoint(ec);
    if (!ec) {
        spdlog::info("Receiver listening on {}:{} (save_dir='{}')", ep.address().to_string(), ep.port(), save_dir_);
    } else {
        spdlog::info("Receiver listening (save_dir='{}')", save_dir_);
    }

    DoAccept();
}

void Receiver::DoAccept() {
    acceptor_.async_accept(socket_, [this](const std::error_code& error) {
        if (!error) {
            std::error_code ec;
            auto remote = socket_.remote_endpoint(ec);
            if (!ec) {
                spdlog::info("Incoming connection from {}:{}", remote.address().to_string(), remote.port());
            }

            // Move socket to thread for blocking handling
            // Note: socket_ is a member, so we can't move it easily if we want to accept more?
            // For MVP one transfer at a time is fine.
            std::thread([this]() {
                HandleSession();
            }).detach();
        } else {
            spdlog::error("Accept failed: {}", error.message());
        }
    });
}

void Receiver::HandleSession() {
    try {
        std::string sender = "Unknown";
        {
            std::error_code ec;
            auto remote = socket_.remote_endpoint(ec);
            if (!ec) sender = remote.address().to_string();
        }

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
        const auto safe_name = std::filesystem::path(filename).filename().string();
        if (safe_name != filename) {
            spdlog::warn("Sanitized filename '{}' -> '{}'", filename, safe_name);
            filename = safe_name;
        }

        spdlog::info("Incoming file '{}' ({} bytes) from {}", filename, filesize, sender);
        
        // Callback to user
        bool accepted = true;
        if (request_cb_) {
            accepted = request_cb_(filename, filesize, sender);
        }

        if (!accepted) {
            spdlog::info("Transfer rejected for '{}' from {}", filename, sender);
            Protocol::Header resp;
            resp.magic = Protocol::MAGIC;
            resp.version = Protocol::VERSION;
            resp.type = (uint16_t)Protocol::PacketType::Reject;
            resp.body_len = 0;
            resp.checksum = 0;
            asio::write(socket_, asio::buffer(&resp, sizeof(resp)));
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
        spdlog::debug("Accepted '{}' from {}", filename, sender);

        // 2. Read Data Header
        asio::read(socket_, asio::buffer(&header, sizeof(header)));
        if (header.type != (uint16_t)Protocol::PacketType::Data) throw std::runtime_error("Expected Data");
        
        // 3. Stream to File
        std::error_code ec;
        std::filesystem::create_directories(save_dir_, ec);
        if (ec) spdlog::warn("Failed to create save_dir '{}': {}", save_dir_, ec.message());

        std::filesystem::path path = std::filesystem::path(save_dir_) / filename;
        std::ofstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open output file");
        spdlog::info("Saving to '{}'", path.string());
        
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
        spdlog::info("Received '{}' ({} bytes) from {}", filename, received, sender);
        
        if (done_cb_) done_cb_(true);

    } catch (const std::exception& e) {
        spdlog::error("Receiver exception: {}", e.what());
        if (done_cb_) done_cb_(false);
    }
}

void Receiver::Cancel() {
    socket_.close();
    acceptor_.close();
}

} // namespace Network
} // namespace ZSend
