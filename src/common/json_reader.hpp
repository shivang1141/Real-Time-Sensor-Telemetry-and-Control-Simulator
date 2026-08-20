#pragma once

// Internal helper for strict JSON configuration parsing. Not part of the public API.

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <type_traits>

#include "telemetry/app/config_error.hpp"

namespace telemetry::detail {

/// Reads fields from a JSON object with type checking, defaults and a precise error path
/// ("processing.workers: expected unsigned integer"). finish() rejects unknown keys so
/// that typos in configuration files fail loudly instead of being silently ignored.
class ObjectReader {
public:
    ObjectReader(const nlohmann::json& object, std::string path)
        : object_(object), path_(std::move(path)) {
        if (!object_.is_object()) {
            throw ConfigError(display_path() + ": expected an object");
        }
    }

    template <typename T>
    T get(const char* key, T default_value) {
        const auto value = get_optional<T>(key);
        return value ? *value : default_value;
    }

    template <typename T>
    std::optional<T> get_optional(const char* key) {
        used_.insert(key);
        const auto it = object_.find(key);
        if (it == object_.end() || it->is_null()) {
            return std::nullopt;
        }
        return convert<T>(*it, key);
    }

    template <typename T>
    T require(const char* key) {
        const auto value = get_optional<T>(key);
        if (!value) {
            throw ConfigError(child_path(key) + ": required field missing");
        }
        return *value;
    }

    /// Returns the child object/array or nullptr if absent.
    const nlohmann::json* child(const char* key) {
        used_.insert(key);
        const auto it = object_.find(key);
        return it == object_.end() || it->is_null() ? nullptr : &*it;
    }

    std::string child_path(const std::string& key) const {
        return path_.empty() ? key : path_ + "." + key;
    }

    void finish() const {
        for (const auto& item : object_.items()) {
            if (used_.count(item.key()) == 0) {
                throw ConfigError(child_path(item.key()) + ": unknown field");
            }
        }
    }

private:
    std::string display_path() const { return path_.empty() ? "<root>" : path_; }

    template <typename T>
    T convert(const nlohmann::json& value, const char* key) const {
        if constexpr (std::is_same_v<T, bool>) {
            if (!value.is_boolean()) {
                throw ConfigError(child_path(key) + ": expected boolean");
            }
            return value.get<bool>();
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (!value.is_string()) {
                throw ConfigError(child_path(key) + ": expected string");
            }
            return value.get<std::string>();
        } else if constexpr (std::is_floating_point_v<T>) {
            if (!value.is_number()) {
                throw ConfigError(child_path(key) + ": expected number");
            }
            return value.get<T>();
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            if (!value.is_number_unsigned()) {
                throw ConfigError(child_path(key) + ": expected unsigned integer");
            }
            const auto raw = value.get<std::uint64_t>();
            if (raw > std::numeric_limits<T>::max()) {
                throw ConfigError(child_path(key) + ": value too large");
            }
            return static_cast<T>(raw);
        } else if constexpr (std::is_integral_v<T>) {
            if (!value.is_number_integer()) {
                throw ConfigError(child_path(key) + ": expected integer");
            }
            const auto raw = value.get<std::int64_t>();
            if (raw < std::numeric_limits<T>::min() || raw > std::numeric_limits<T>::max()) {
                throw ConfigError(child_path(key) + ": value out of range");
            }
            return static_cast<T>(raw);
        } else {
            static_assert(sizeof(T) == 0, "unsupported config field type");
        }
    }

    const nlohmann::json& object_;
    std::string path_;
    std::set<std::string> used_;
};

/// Parses a JSON file, mapping I/O and syntax errors to ConfigError.
nlohmann::json load_json_file(const std::string& path);

}  // namespace telemetry::detail
