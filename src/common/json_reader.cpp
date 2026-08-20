#include "common/json_reader.hpp"

#include <fstream>

namespace telemetry::detail {

nlohmann::json load_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw ConfigError("cannot open config file: " + path);
    }
    nlohmann::json root = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false,
                                                /*ignore_comments=*/true);
    if (root.is_discarded()) {
        throw ConfigError("config file is not valid JSON: " + path);
    }
    return root;
}

}  // namespace telemetry::detail
