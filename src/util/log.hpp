// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace tcrm::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

bool parse_level(std::string_view name, Level* out);
const char* level_name(Level level);

class Logger {
public:
    static Logger& instance();

    void set_level(Level level);
    Level level() const;
    bool enabled(Level level) const { return level >= level_; }

    /// Also append to a file. An empty path closes the file.
    bool set_file(const std::string& path);
    void set_stderr(bool on);

    void write(Level level, std::string_view component, std::string_view message);

private:
    Logger() = default;

    mutable std::mutex mu_;
    Level level_ = Level::Info;
    bool to_stderr_ = true;
    std::ofstream file_;
};

namespace detail {

template <typename... Args>
std::string concat(Args&&... args) {
    std::ostringstream os;
    (os << ... << std::forward<Args>(args));
    return os.str();
}

}  // namespace detail

template <typename... Args>
void debug(std::string_view component, Args&&... args) {
    Logger& l = Logger::instance();
    if (l.enabled(Level::Debug)) l.write(Level::Debug, component, detail::concat(std::forward<Args>(args)...));
}

template <typename... Args>
void info(std::string_view component, Args&&... args) {
    Logger& l = Logger::instance();
    if (l.enabled(Level::Info)) l.write(Level::Info, component, detail::concat(std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::string_view component, Args&&... args) {
    Logger& l = Logger::instance();
    if (l.enabled(Level::Warn)) l.write(Level::Warn, component, detail::concat(std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::string_view component, Args&&... args) {
    Logger& l = Logger::instance();
    if (l.enabled(Level::Error)) l.write(Level::Error, component, detail::concat(std::forward<Args>(args)...));
}

}  // namespace tcrm::log
