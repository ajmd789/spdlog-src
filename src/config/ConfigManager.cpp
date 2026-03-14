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
    AppConfig config;
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
        } catch (const std::exception& e) {
            spdlog::error("Failed to load config: {}", e.what());
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
        
        std::ofstream f(GetConfigPath());
        f << j.dump(4);
    } catch (const std::exception& e) {
        spdlog::error("Failed to save config: {}", e.what());
    }
}

} // namespace Config
} // namespace ZSend
