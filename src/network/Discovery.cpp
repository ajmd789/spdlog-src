#include "ZSend/network/Discovery.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <iostream>

namespace ZSend {
namespace Network {

// 发现模块构造：
// 1) 绑定 UDP 监听端口；
// 2) 开启广播发送能力；
// 3) 预采集本机 IPv4 地址集合，用于过滤“自己发给自己”的广播包。
Discovery::Discovery(asio::io_context& ioc, int port, const Config::AppConfig& config)
    : ioc_(ioc), socket_(ioc, asio::ip::udp::endpoint(asio::ip::udp::v4(), port)),
      broadcast_timer_(ioc), config_(config), local_ips_(CollectLocalIps()), running_(false), port_(port) {
    socket_.set_option(asio::socket_base::broadcast(true));
}

Discovery::~Discovery() {
    Stop();
}

// 启动发现流程：
// - 开始异步接收其他节点广播；
// - 立即广播一次自己的在线信息；
// - 后续由定时器每 2 秒继续广播。
void Discovery::Start() {
    if (running_) return;
    running_ = true;
    spdlog::info("Discovery started (udp:{})", port_);
    StartReceive();
    BroadcastPresence();
}

// 停止发现流程：
// - 关闭运行标志；
// - 取消广播定时器；
// - 关闭 UDP socket，终止收发。
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

// 判断某个 IP 是否属于当前机器。
bool Discovery::IsSelfIp(const std::string& ip) const {
    return local_ips_.find(ip) != local_ips_.end();
}

// 收集本机 IPv4 地址集合：
// - 包含回环地址 127.0.0.1；
// - 通过主机名解析补充本机网卡地址；
// - 仅保留 IPv4，用于当前广播发现场景。
std::unordered_set<std::string> Discovery::CollectLocalIps() const {
    std::unordered_set<std::string> ips;
    ips.insert("127.0.0.1");

    try {
        asio::ip::tcp::resolver resolver(ioc_);
        std::error_code ec;
        auto results = resolver.resolve(asio::ip::host_name(), "", ec);
        if (!ec) {
            for (const auto& entry : results) {
                const auto& addr = entry.endpoint().address();
                if (addr.is_v4()) {
                    ips.insert(addr.to_string());
                }
            }
        }
    } catch (const std::exception&) {
    }

    return ips;
}

// 挂起下一次异步接收，持续监听 UDP 广播包。
void Discovery::StartReceive() {
    socket_.async_receive_from(
        asio::buffer(recv_buffer_), sender_endpoint_,
        [this](const std::error_code& error, std::size_t bytes_transferred) {
            HandleReceive(error, bytes_transferred);
        });
}

// 处理一次接收事件：
// 1) 解析 JSON；
// 2) 校验发现协议 magic；
// 3) 构造 Peer；
// 4) 过滤本机来源 IP；
// 5) 回调上层更新设备列表；
// 6) 无论成功失败都继续下一轮接收。
void Discovery::HandleReceive(const std::error_code& error, std::size_t bytes_transferred) {
    if (!running_ || error == asio::error::operation_aborted) return;

    if (!error) {
        try {
            std::string data(recv_buffer_.data(), bytes_transferred);
            spdlog::info("UDP recv {}:{} {}", sender_endpoint_.address().to_string(), sender_endpoint_.port(), data);
            auto j = nlohmann::json::parse(data);
            
            // 协议基础校验：必须是 ZSend 的发现报文。
            if (j.contains("magic") && j["magic"] == "ZSEND_DISCOVERY") {
                Peer peer;
                peer.nickname = j.value("nickname", "Unknown");
                peer.ip = sender_endpoint_.address().to_string();
                peer.port = j.value("port", 0);
                peer.uuid = j.value("uuid", "");
                peer.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                // 过滤本机广播，避免“搜索到自己”。
                if (IsSelfIp(peer.ip)) {
                    StartReceive();
                    return;
                }

                if (on_peer_found_) {
                    on_peer_found_(peer);
                }
            }
        } catch (const std::exception& e) {
            // 非法/损坏报文，直接忽略，不影响后续接收循环。
        }
    }

    StartReceive();
}

// 广播本机在线信息：
// - 广播协议魔术字；
// - 广播昵称；
// - 广播文件传输端口；
// - 每 2 秒重复广播一次，维持“在线可发现”状态。
void Discovery::BroadcastPresence() {
    if (!running_) return;

    nlohmann::json j;
    j["magic"] = "ZSEND_DISCOVERY";
    j["nickname"] = config_.nickname;
    j["port"] = port_;

    auto data = j.dump();
    // 广播到当前配置端口（当前项目默认 8888）。
    auto endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::broadcast(), port_);
    spdlog::info("UDP send {}:{} {}", endpoint.address().to_string(), endpoint.port(), data);

    socket_.async_send_to(
        asio::buffer(data), endpoint,
        [this, endpoint, data](const std::error_code& error, std::size_t bytes_transferred) {
            HandleBroadcast(error, bytes_transferred, endpoint, data);
        });

    broadcast_timer_.expires_after(std::chrono::seconds(2));
    broadcast_timer_.async_wait([this](const std::error_code& error) {
        if (!error) BroadcastPresence();
    });
}

// 广播发送回调：记录异常，便于排查网络或防火墙问题。
void Discovery::HandleBroadcast(const std::error_code& error,
                                std::size_t bytes_transferred,
                                const asio::ip::udp::endpoint& endpoint,
                                const std::string& data) {
    if (error && error != asio::error::operation_aborted) {
        spdlog::warn("Broadcast failed {}:{} {} {}", endpoint.address().to_string(), endpoint.port(), error.message(), data);
        return;
    }
    spdlog::info("Broadcast ok {}:{} {} {}", endpoint.address().to_string(), endpoint.port(), bytes_transferred, data);
}

} // namespace Network
} // namespace ZSend
