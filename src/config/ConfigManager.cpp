#include "ZSend/config/ConfigManager.hpp"
#include "ZSend/utils/NicknameGenerator.hpp"
#include <fstream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace ZSend {
namespace Config {

std::string ConfigManager::GetConfigPath() {
    return "config.json"; 
}

AppConfig ConfigManager::Load() {
    AppConfig config;
    std::string path = GetConfigPath();
    
    bool loaded = false;
    if (std::filesystem::exists(path)) {
        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;
            if (j.contains("nickname")) config.nickname = j["nickname"];
            if (j.contains("download_dir")) config.download_dir = j["download_dir"];
            loaded = true;
        } catch (const std::exception& e) {
            spdlog::error("Failed to load config: {}", e.what());
        }
    }

    if (config.nickname.empty()) {
        config.nickname = Utils::NicknameGenerator::Generate();
        // If we generated a new nickname, we should save it, but maybe not overwrite existing download_dir if it was valid?
        // Actually if it was empty, it means we didn't load it or it wasn't there.
        // If loaded is true but nickname empty, we just set it.
        // We will save later.
        Save(config); 
    }
    
    if (config.download_dir.empty()) {
        config.download_dir = ".";
    }

    return config;
}

void ConfigManager::Save(const AppConfig& config) {
    try {
        nlohmann::json j;
        j["nickname"] = config.nickname;
        j["download_dir"] = config.download_dir;
        
        std::ofstream f(GetConfigPath());
        f << j.dump(4);
    } catch (const std::exception& e) {
        spdlog::error("Failed to save config: {}", e.what());
    }
}

} // namespace Config
} // namespace ZSend
