#include "ZSend/config/ConfigManager.hpp"
#include "ZSend/utils/NicknameGenerator.hpp"
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <random>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace ZSend {
namespace Config {

namespace {
constexpr uint16_t kDefaultServicePort = 53317;

std::string GenerateDeviceId() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);

    auto part = []() {
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%08x", dist(gen));
        return std::string(buf);
    };

    return part() + part();
}
}

std::string ConfigManager::GetConfigPath() {
    return "config.json"; 
}

AppConfig ConfigManager::Load() {
    AppConfig config{};
    std::string path = GetConfigPath();

    bool need_save = false;
    if (std::filesystem::exists(path)) {
        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;
            if (j.contains("nickname")) config.nickname = j["nickname"];
            if (j.contains("download_dir")) config.download_dir = j["download_dir"];
            if (j.contains("device_id")) config.device_id = j["device_id"];
            if (j.contains("service_port") && j["service_port"].is_number_integer()) {
                const int service_port = j["service_port"].get<int>();
                if (service_port > 0 && service_port <= 65535) {
                    config.service_port = static_cast<uint16_t>(service_port);
                } else {
                    need_save = true;
                }
            } else if (j.contains("service_port")) {
                need_save = true;
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to load config: {}", e.what());
        }
    }

    if (config.nickname.empty()) {
        config.nickname = Utils::NicknameGenerator::Generate();
        need_save = true;
    }
    
    if (config.download_dir.empty()) {
        config.download_dir = ".";
        need_save = true;
    }

    if (config.device_id.empty()) {
        config.device_id = GenerateDeviceId();
        need_save = true;
    }

    if (config.service_port == 0) {
        config.service_port = kDefaultServicePort;
        need_save = true;
    }

    if (need_save) {
        Save(config);
    }

    return config;
}

void ConfigManager::Save(const AppConfig& config) {
    try {
        nlohmann::json j;
        j["nickname"] = config.nickname;
        j["download_dir"] = config.download_dir;
        j["device_id"] = config.device_id;
        j["service_port"] = config.service_port;
        
        std::ofstream f(GetConfigPath());
        f << j.dump(4);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to save config: {}", e.what());
    }
}

} // namespace Config
} // namespace ZSend
