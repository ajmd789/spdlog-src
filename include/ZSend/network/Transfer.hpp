#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <asio.hpp>
#include "ZSend/network/Protocol.hpp"

namespace ZSend {
namespace Network {

class Transfer {
public:
    using ProgressCallback = std::function<void(uint64_t transferred, uint64_t total, double speed_mbps)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    virtual ~Transfer() = default;
    virtual void Cancel() = 0;
};

class Sender : public Transfer {
public:
    Sender(asio::io_context& ioc);
    void Connect(const std::string& ip, int port, std::function<void(bool success)> cb);
    void SendFile(const std::string& filepath, ProgressCallback progress_cb, std::function<void(bool success)> done_cb);
    void Cancel() override;

private:
    asio::ip::tcp::socket socket_;
    std::atomic<bool> connected_{false};
    // ... impl details
};

class Receiver : public Transfer {
public:
    using RequestCallback = std::function<bool(const std::string& filename, uint64_t size, const std::string& sender)>;

    Receiver(asio::io_context& ioc, int port, const std::string& save_dir);
    void Start(RequestCallback request_cb, ProgressCallback progress_cb, std::function<void(bool success)> done_cb);
    void Cancel() override;

private:
    void DoAccept();
    void HandleSession();
    
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::string save_dir_;
    RequestCallback request_cb_;
    ProgressCallback progress_cb_;
    std::function<void(bool success)> done_cb_;
};

} // namespace Network
} // namespace ZSend
