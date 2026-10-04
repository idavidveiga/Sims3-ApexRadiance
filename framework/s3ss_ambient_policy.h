#pragma once
#include <toml++/toml.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>

namespace S3SSAmbientPolicy {
struct Correction { std::array<float, 3> rgb{}; std::string text; };
// Only the saved debug-variable override is removed. Patch switches and all other settings survive.
inline std::optional<Correction> Prepare(const std::string& text) {
    try {
        auto root = toml::parse(text);
        auto* settings = root["settings"].as_table();
        auto* values = root["settings"]["BradyBunchBlue RGB"]["value"].as_array();
        if (!settings || !values || values->size() != 3) return std::nullopt;
        Correction result;
        for (size_t i = 0; i < 3; ++i) {
            const auto value = (*values)[i].value<double>();
            if (!value || !std::isfinite(*value) || *value < 0 || *value > 1) return std::nullopt;
            result.rgb[i] = static_cast<float>(*value);
        }
        settings->erase("BradyBunchBlue RGB");
        if (settings->empty()) root.erase("settings");
        // Preserve comments and layout for the ordinary standalone section saved by S3SS.
        size_t start = std::string::npos, end = text.size(), offset = 0;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            const auto first = line.find_first_not_of(" \t\r");
            if (first != std::string::npos && line[first] == '[') {
                if (start != std::string::npos) { end = offset; break; }
                const auto headerEnd = line.find(']', first);
                if (headerEnd != std::string::npos) {
                    std::string header = line.substr(first, headerEnd - first + 1);
                    header.erase(std::remove_if(header.begin(), header.end(), [](char c) { return c == ' ' || c == '\t'; }), header.end());
                    if (header == "[settings.'BradyBunchBlueRGB']" || header == "[settings.\"BradyBunchBlueRGB\"]") start = offset;
                }
            }
            offset += line.size() + 1;
        }
        if (start != std::string::npos) {
            result.text = text.substr(0, start) + text.substr(end);
            if (toml::parse(result.text) == root) return result;
        }
        std::ostringstream output;
        output << toml::toml_formatter(root);
        result.text = output.str();
        if (toml::parse(result.text) != root) return std::nullopt;
        return result;
    } catch (const toml::parse_error&) { return std::nullopt; }
}
}
