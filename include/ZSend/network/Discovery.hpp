#pragma once
#include <string>
#include <functional>
#include <asio.hpp>
#include <array>
#include "ZSend/config/ConfigManager.hpp"

namespace ZSend {
namespace Network {

struct Peer {
    std::string nickname;
    std::string ip;
    uint16_t port;
    std::string uuid;
    long long last_seen;
};

class Discovery {
public:
    using PeerFoundCallback = std::function<void(const Peer&)>;

    Discovery(asio::io_context& ioc, int port, const Config::AppConfig& config);
    ~Discovery();
    
    void Start();
    void Stop();
    void SetOnPeerFound(PeerFoundCallback cb);

private:
    void StartReceive();
    void HandleReceive(const std::error_code& error, std::size_t bytes_transferred);
    void BroadcastPresence();
    void HandleBroadcast(const std::error_code& error);

    asio::io_context& ioc_;
    asio::ip::udp::socket socket_;
    asio::ip::udp::endpoint sender_endpoint_;
    std::array<char, 1024> recv_buffer_;
    asio::steady_timer broadcast_timer_;
    Config::AppConfig config_;
    PeerFoundCallback on_peer_found_;
    bool running_;
    int port_;
};

} // namespace Network
} // namespace ZSend
