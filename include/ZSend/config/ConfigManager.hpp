#pragma once
#include <string>

namespace ZSend {
namespace Config {

struct AppConfig {
    std::string nickname;
    std::string download_dir;
    std::string device_id;
};

class ConfigManager {
public:
    static AppConfig Load();
    static void Save(const AppConfig& config);
    static std::string GetConfigPath();
};

} // namespace Config
} // namespace ZSend
