#include "ZSend/utils/NicknameGenerator.hpp"
#include <vector>
#include <random>

namespace ZSend {
namespace Utils {

std::string NicknameGenerator::Generate() {
    static const std::vector<std::string> adjectives = {
        "勤奋的", "勇敢的", "快乐的", "聪明的", "诚实的", "活泼的", "冷静的", "友好的", "热情的", "善良的"
    };
    static const std::vector<std::string> nouns = {
        "蜜蜂", "狮子", "兔子", "熊猫", "老虎", "海豚", "雄鹰", "松鼠", "考拉", "大象"
    };

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> adj_dist(0, (int)adjectives.size() - 1);
    std::uniform_int_distribution<> noun_dist(0, (int)nouns.size() - 1);

    return adjectives[adj_dist(gen)] + nouns[noun_dist(gen)];
}

} // namespace Utils
} // namespace ZSend
