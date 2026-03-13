#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ZSend {
namespace Protocol {

constexpr uint32_t MAGIC = 0x5A534E44; // "ZSND"
constexpr uint16_t VERSION = 1;

enum class PacketType : uint16_t {
    Handshake = 1,
    Accept = 2,
    Reject = 3,
    Data = 4,
    Ack = 5,
    Error = 6,
    Cancel = 7
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint64_t body_len;
    uint32_t checksum; // CRC32 of body
};
#pragma pack(pop)

// Helper structs for body serialization (JSON)
struct HandshakeBody {
    std::string filename;
    uint64_t filesize;
    std::string file_hash; // SHA256/CRC32 of entire file if pre-calculated
    std::string sender_nick;
};

} // namespace Protocol
} // namespace ZSend
