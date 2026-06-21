#include "common/Config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace
{
std::string trim(const std::string& text)
{
    auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });

    auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();

    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}
}

Config::Config(const std::string& filename)
{
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open config file: " + filename);
    }

    std::string line;
    int lineNo = 0;
    while (std::getline(ifs, line)) {
        ++lineNo;

        std::string content = trim(line);
        if (content.empty() || content[0] == '#') {
            continue;
        }

        std::size_t pos = content.find('=');
        if (pos == std::string::npos) {
            throw std::runtime_error("invalid config line " + std::to_string(lineNo) + ": " + line);
        }

        std::string key = trim(content.substr(0, pos));
        std::string value = trim(content.substr(pos + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("invalid config line " + std::to_string(lineNo) + ": " + line);
        }

        items_[key] = value;
    }
}

const std::string& Config::get(const std::string& key) const
{
    auto it = items_.find(key);
    if (it == items_.end()) {
        throw std::runtime_error("missing config key: " + key);
    }
    return it->second;
}
