#include "ZSend/network/Discovery.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <iostream>

namespace ZSend {
namespace Network {

Discovery::Discovery(asio::io_context& ioc, int port, const Config::AppConfig& config)
    : ioc_(ioc), socket_(ioc, asio::ip::udp::endpoint(asio::ip::udp::v4(), port)),
      broadcast_timer_(ioc), config_(config), running_(false), port_(port) {
    socket_.set_option(asio::socket_base::broadcast(true));
}

Discovery::~Discovery() {
    Stop();
}

void Discovery::Start() {
    if (running_) return;
    running_ = true;
    StartReceive();
    BroadcastPresence();
}

void Discovery::Stop() {
    running_ = false;
    broadcast_timer_.cancel();
    if (socket_.is_open()) {
        socket_.close();
    }
}

void Discovery::SetOnPeerFound(PeerFoundCallback cb) {
    on_peer_found_ = cb;
}

void Discovery::StartReceive() {
    socket_.async_receive_from(
        asio::buffer(recv_buffer_), sender_endpoint_,
        [this](const std::error_code& error, std::size_t bytes_transferred) {
            HandleReceive(error, bytes_transferred);
        });
}

void Discovery::HandleReceive(const std::error_code& error, std::size_t bytes_transferred) {
    if (!running_ || error == asio::error::operation_aborted) return;

    if (!error) {
        try {
            std::string data(recv_buffer_.data(), bytes_transferred);
            auto j = nlohmann::json::parse(data);
            
            // Basic validation
            if (j.contains("magic") && j["magic"] == "ZSEND_DISCOVERY") {
                Peer peer;
                peer.nickname = j.value("nickname", "Unknown");
                peer.ip = sender_endpoint_.address().to_string();
                peer.port = j.value("port", 0);
                peer.uuid = j.value("uuid", "");
                peer.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                
                // Don't report self? We can check IP or UUID.
                // For MVP, just report all valid packets.
                
                if (on_peer_found_) {
                    on_peer_found_(peer);
                }
            }
        } catch (const std::exception& e) {
            // Malformed packet, ignore
        }
    }

    StartReceive();
}

void Discovery::BroadcastPresence() {
    if (!running_) return;

    nlohmann::json j;
    j["magic"] = "ZSEND_DISCOVERY";
    j["nickname"] = config_.nickname;
    j["port"] = port_; // Listening TCP port for file transfer (simplified)
    // j["uuid"] = ...

    auto data = j.dump();
    // Broadcast to port 8888 (hardcoded for now or same as port_)
    auto endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::broadcast(), port_);

    socket_.async_send_to(
        asio::buffer(data), endpoint,
        [this](const std::error_code& error, std::size_t) {
            HandleBroadcast(error);
        });

    broadcast_timer_.expires_after(std::chrono::seconds(2));
    broadcast_timer_.async_wait([this](const std::error_code& error) {
        if (!error) BroadcastPresence();
    });
}

void Discovery::HandleBroadcast(const std::error_code& error) {
    if (error && error != asio::error::operation_aborted) {
        spdlog::warn("Broadcast failed: {}", error.message());
    }
}

} // namespace Network
} // namespace ZSend
