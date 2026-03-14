#include "ZSend/network/Discovery.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace ZSend {
namespace Network {

namespace {
constexpr const char* kDiscoveryMagic = "ZSEND_DISCOVERY";
constexpr const char* kDiscoveryMulticastGroup = "239.255.88.88";
constexpr std::chrono::seconds kPresenceInterval{2};

template <typename T>
void SortAndUnique(std::vector<T>& items) {
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
}
} // namespace

// 发现模块构造：
// 1) 绑定 UDP 监听端口；
// 2) 开启广播发送能力；
// 3) 预采集本机 IPv4 地址集合，用于过滤“自己发给自己”的广播包。
Discovery::Discovery(asio::io_context& ioc, int port, const Config::AppConfig& config)
    : ioc_(ioc),
      socket_(ioc),
      broadcast_timer_(ioc),
      config_(config),
      running_(false),
      port_(port) {
    CollectLocalNetworkInfo();
    ConfigureSocket();
}

Discovery::~Discovery() {
    Stop();
}

void Discovery::CollectLocalNetworkInfo() {
    local_ips_.clear();
    interface_addrs_v4_.clear();
    broadcast_addrs_v4_.clear();

    local_ips_.insert("127.0.0.1");

#ifdef _WIN32
    ULONG buffer_size = 15 * 1024;
    std::vector<BYTE> buffer(buffer_size);

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
        reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &buffer_size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
            &buffer_size);
    }

    if (ret == NO_ERROR) {
        for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter != nullptr;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                if (!unicast->Address.lpSockaddr) continue;
                if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;

                auto* sockaddr_ipv4 = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                auto bytes = asio::ip::address_v4::bytes_type{};
                std::memcpy(bytes.data(), &sockaddr_ipv4->sin_addr, bytes.size());
                asio::ip::address_v4 addr(bytes);

                local_ips_.insert(addr.to_string());
                interface_addrs_v4_.push_back(addr);

                ULONG prefix_len = unicast->OnLinkPrefixLength;
                if (prefix_len <= 32) {
                    uint32_t mask = (prefix_len == 0) ? 0u : (0xFFFFFFFFu << (32u - prefix_len));
                    uint32_t ip_u = addr.to_uint();
                    uint32_t broadcast_u = ip_u | (~mask);
                    asio::ip::address_v4 broadcast_addr(broadcast_u);
                    broadcast_addrs_v4_.push_back(broadcast_addr);
                }
            }
        }
    } else {
        spdlog::warn("GetAdaptersAddresses(AF_INET) failed: {}", ret);
    }
#else
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;

            if (ifa->ifa_addr->sa_family == AF_INET) {
                auto* sockaddr_ipv4 = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
                auto bytes = asio::ip::address_v4::bytes_type{};
                std::memcpy(bytes.data(), &sockaddr_ipv4->sin_addr, bytes.size());
                asio::ip::address_v4 addr(bytes);
                local_ips_.insert(addr.to_string());

                if ((ifa->ifa_flags & IFF_LOOPBACK) == 0) {
                    interface_addrs_v4_.push_back(addr);
                }

                if ((ifa->ifa_flags & IFF_BROADCAST) != 0 && ifa->ifa_broadaddr &&
                    ifa->ifa_broadaddr->sa_family == AF_INET) {
                    auto* sockaddr_broadcast = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr);
                    auto bbytes = asio::ip::address_v4::bytes_type{};
                    std::memcpy(bbytes.data(), &sockaddr_broadcast->sin_addr, bbytes.size());
                    asio::ip::address_v4 baddr(bbytes);
                    broadcast_addrs_v4_.push_back(baddr);
                }
            }
        }
        freeifaddrs(ifaddr);
    } else {
        spdlog::warn("getifaddrs failed");
    }
#endif

    SortAndUnique(interface_addrs_v4_);
    SortAndUnique(broadcast_addrs_v4_);

    // Fallback (best effort) - keep behavior close to old implementation if enumeration is empty.
    if (interface_addrs_v4_.empty()) {
        try {
            asio::ip::tcp::resolver resolver(ioc_);
            std::error_code ec;
            auto results = resolver.resolve(asio::ip::host_name(), "", ec);
            if (!ec) {
                for (const auto& entry : results) {
                    const auto& addr = entry.endpoint().address();
                    if (addr.is_v4()) {
                        auto v4 = addr.to_v4();
                        local_ips_.insert(v4.to_string());
                        if (v4 != asio::ip::address_v4::loopback()) interface_addrs_v4_.push_back(v4);
                    }
                }
                SortAndUnique(interface_addrs_v4_);
            }
        } catch (const std::exception&) {
        }
    }

    if (broadcast_addrs_v4_.empty()) {
        broadcast_addrs_v4_.push_back(asio::ip::address_v4::broadcast());
    }

    spdlog::info("Discovery local IPv4 interfaces: {}", interface_addrs_v4_.size());
    for (const auto& addr : interface_addrs_v4_) {
        spdlog::debug("Discovery iface v4: {}", addr.to_string());
    }
    spdlog::info("Discovery broadcast targets: {}", broadcast_addrs_v4_.size());
    for (const auto& addr : broadcast_addrs_v4_) {
        spdlog::debug("Discovery broadcast v4: {}", addr.to_string());
    }
}

void Discovery::ConfigureSocket() {
    std::error_code ec;

    socket_.open(asio::ip::udp::v4(), ec);
    if (ec) {
        spdlog::error("Discovery socket open failed: {}", ec.message());
        return;
    }

    socket_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        spdlog::warn("Discovery set reuse_address failed: {}", ec.message());
        ec.clear();
    }

    socket_.set_option(asio::socket_base::broadcast(true), ec);
    if (ec) {
        spdlog::warn("Discovery set broadcast failed: {}", ec.message());
        ec.clear();
    }

    socket_.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), port_), ec);
    if (ec) {
        spdlog::error("Discovery bind UDP {} failed: {}", port_, ec.message());
        socket_.close();
        return;
    }

    try {
        multicast_group_ = asio::ip::make_address_v4(kDiscoveryMulticastGroup);
    } catch (const std::exception& e) {
        spdlog::error("Invalid multicast group '{}': {}", kDiscoveryMulticastGroup, e.what());
        multicast_group_ = asio::ip::address_v4{};
    }

    if (!multicast_group_.is_unspecified()) {
        socket_.set_option(asio::ip::multicast::hops(1), ec);
        if (ec) {
            spdlog::warn("Discovery set multicast hops failed: {}", ec.message());
            ec.clear();
        }
        JoinMulticastGroups();
    }
}

void Discovery::JoinMulticastGroups() {
    if (multicast_group_.is_unspecified()) return;

    std::error_code ec;
    if (interface_addrs_v4_.empty()) {
        socket_.set_option(asio::ip::multicast::join_group(multicast_group_), ec);
        if (ec) {
            spdlog::warn("Discovery join multicast {} failed: {}", multicast_group_.to_string(), ec.message());
            return;
        }
        spdlog::info("Discovery joined multicast group {}", multicast_group_.to_string());
        return;
    }

    int joined = 0;
    for (const auto& iface : interface_addrs_v4_) {
        socket_.set_option(asio::ip::multicast::join_group(multicast_group_, iface), ec);
        if (ec) {
            spdlog::warn("Discovery join multicast {} on {} failed: {}", multicast_group_.to_string(), iface.to_string(),
                ec.message());
            ec.clear();
            continue;
        }
        ++joined;
        spdlog::debug("Discovery joined multicast {} on {}", multicast_group_.to_string(), iface.to_string());
    }

    spdlog::info("Discovery joined multicast group {} on {} interface(s)", multicast_group_.to_string(), joined);
}

// 启动发现流程：
// - 开始异步接收其他节点广播；
// - 立即广播一次自己的在线信息；
// - 后续由定时器每 2 秒继续广播。
void Discovery::Start() {
    if (running_) return;
    if (!socket_.is_open()) {
        spdlog::error("Discovery socket is not open; discovery will not run");
        return;
    }
    running_ = true;
    spdlog::info("Discovery started: udp_port={}, multicast_group={}", port_,
        multicast_group_.is_unspecified() ? std::string{"-"} : multicast_group_.to_string());
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

    if (error) {
        spdlog::debug("Discovery receive failed: {}", error.message());
        StartReceive();
        return;
    }

    const auto sender_ip = sender_endpoint_.address().to_string();
    const auto sender_port = sender_endpoint_.port();
    spdlog::debug("Discovery packet {} bytes from {}:{}", bytes_transferred, sender_ip, sender_port);

    if (!error) {
        try {
            std::string data(recv_buffer_.data(), bytes_transferred);
            auto j = nlohmann::json::parse(data);
            
            // 协议基础校验：必须是 ZSend 的发现报文。
            if (j.contains("magic") && j["magic"] == kDiscoveryMagic) {
                Peer peer;
                peer.nickname = j.value("nickname", "Unknown");
                peer.ip = sender_ip;
                peer.port = j.value("port", 0);
                peer.uuid = j.value("uuid", "");
                peer.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                // 过滤本机广播，避免“搜索到自己”。
                if (IsSelfIp(peer.ip)) {
                    spdlog::debug("Discovery ignored self packet from {}", peer.ip);
                    StartReceive();
                    return;
                }

                if (on_peer_found_) {
                    on_peer_found_(peer);
                }
            }
        } catch (const std::exception& e) {
            spdlog::debug("Discovery parse failed from {}:{}: {}", sender_ip, sender_port, e.what());
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
    j["magic"] = kDiscoveryMagic;
    j["nickname"] = config_.nickname;
    j["port"] = port_;

    const auto payload = j.dump();
    std::error_code ec;

    // 1) Multicast (preferred, works better across networks than limited broadcast).
    if (!multicast_group_.is_unspecified()) {
        socket_.send_to(asio::buffer(payload), asio::ip::udp::endpoint(multicast_group_, port_), 0, ec);
        if (ec) HandleBroadcast(ec);
        ec.clear();
    }

    // 2) Broadcast (fallback; can be blocked/dropped on some networks/OSes).
    for (const auto& baddr : broadcast_addrs_v4_) {
        socket_.send_to(asio::buffer(payload), asio::ip::udp::endpoint(baddr, port_), 0, ec);
        if (ec) HandleBroadcast(ec);
        ec.clear();
    }

    broadcast_timer_.expires_after(kPresenceInterval);
    broadcast_timer_.async_wait([this](const std::error_code& error) {
        if (!error) BroadcastPresence();
    });
}

// 广播发送回调：记录异常，便于排查网络或防火墙问题。
void Discovery::HandleBroadcast(const std::error_code& error) {
    if (error && error != asio::error::operation_aborted) {
        spdlog::warn("Discovery send failed: {}", error.message());
    }
}

} // namespace Network
} // namespace ZSend
