#pragma once

// Minimal command-line helpers shared by the executables.

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace telemetry::cli {

/// Parses "--name value" / "--flag" style arguments.
class Args {
public:
    Args(int argc, char** argv) : args_(argv + 1, argv + argc) {}

    bool flag(std::string_view name) {
        for (std::size_t i = 0; i < args_.size(); ++i) {
            if (args_[i] == name) {
                args_.erase(args_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    std::optional<std::string> value(std::string_view name) {
        for (std::size_t i = 0; i < args_.size(); ++i) {
            if (args_[i] == name) {
                if (i + 1 >= args_.size()) {
                    fail("missing value for " + std::string(name));
                }
                std::string result = args_[i + 1];
                args_.erase(args_.begin() + static_cast<std::ptrdiff_t>(i),
                            args_.begin() + static_cast<std::ptrdiff_t>(i) + 2);
                return result;
            }
        }
        return std::nullopt;
    }

    template <typename T>
    std::optional<T> number(std::string_view name) {
        const auto text = value(name);
        if (!text) {
            return std::nullopt;
        }
        T result{};
        const char* begin = text->data();
        const char* end = begin + text->size();
        const auto [ptr, ec] = std::from_chars(begin, end, result);
        if (ec != std::errc() || ptr != end) {
            fail("invalid number for " + std::string(name) + ": " + *text);
        }
        return result;
    }

    /// Call after consuming all known options.
    void reject_unknown() const {
        if (!args_.empty()) {
            fail("unknown argument: " + args_.front());
        }
    }

    [[noreturn]] static void fail(const std::string& message) {
        std::cerr << "error: " << message << " (use --help)\n";
        std::exit(2);
    }

private:
    std::vector<std::string> args_;
};

}  // namespace telemetry::cli
