// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/log.hpp"

#include <iostream>

#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::log {

bool parse_level(std::string_view name, Level* out) {
    const std::string n = str::to_lower(str::trim(name));
    if (n == "debug") *out = Level::Debug;
    else if (n == "info") *out = Level::Info;
    else if (n == "warn" || n == "warning") *out = Level::Warn;
    else if (n == "error") *out = Level::Error;
    else if (n == "off" || n == "none") *out = Level::Off;
    else return false;
    return true;
}

const char* level_name(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Off: return "OFF";
    }
    return "?";
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_level(Level level) {
    std::lock_guard<std::mutex> lock(mu_);
    level_ = level;
}

Level Logger::level() const {
    std::lock_guard<std::mutex> lock(mu_);
    return level_;
}

bool Logger::set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_.is_open()) file_.close();
    if (path.empty()) return true;
    file_.open(path, std::ios::app);
    return file_.is_open();
}

void Logger::set_stderr(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    to_stderr_ = on;
}

void Logger::write(Level level, std::string_view component, std::string_view message) {
    std::string line = timeutil::format_datetime(timeutil::now_ms());
    line += ' ';
    line += level_name(level);
    line += " [";
    line.append(component);
    line += "] ";
    line.append(message);
    line += '\n';
    std::lock_guard<std::mutex> lock(mu_);
    if (to_stderr_) std::cerr << line;
    if (file_.is_open()) {
        file_ << line;
        file_.flush();
    }
}

}  // namespace tcrm::log
