// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "app/application.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop = true;
}

void usage() {
    std::cout << "usage: tropicrm-server [--config FILE] [--data DIR] [--port N] [--log-level LEVEL]\n"
                 "\n"
                 "Settings come from defaults, then the config file, then TROPICRM_* environment\n"
                 "variables (e.g. TROPICRM_SERVER_PORT), then these flags.\n";
}

}  // namespace

int main(int argc, char** argv) {
    tcrm::Config config = tcrm::Config::defaults();
    std::string config_file, data_dir, port, level;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= argc) {
                std::cerr << arg << " needs a value\n";
                std::exit(2);
            }
            *out = argv[++i];
        };
        if (arg == "--config") value(&config_file);
        else if (arg == "--data") value(&data_dir);
        else if (arg == "--port") value(&port);
        else if (arg == "--log-level") value(&level);
        else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown option " << arg << "\n";
            usage();
            return 2;
        }
    }
    std::string error;
    if (!config_file.empty() && !config.load_file(config_file, &error)) {
        std::cerr << "config: " << error << "\n";
        return 1;
    }
    config.apply_environment();
    if (!data_dir.empty()) config.set("data.dir", data_dir);
    if (!port.empty()) config.set("server.port", port);
    if (!level.empty()) config.set("log.level", level);

    tcrm::log::Level lvl;
    if (!tcrm::log::parse_level(config.get("log.level", "info"), &lvl)) {
        std::cerr << "unknown log level " << config.get("log.level") << "\n";
        return 2;
    }
    tcrm::log::Logger::instance().set_level(lvl);
    if (!config.get("log.file").empty() && !tcrm::log::Logger::instance().set_file(config.get("log.file"))) {
        std::cerr << "cannot open log file " << config.get("log.file") << "\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    tcrm::Application app(config);
    if (!app.init(&error) || !app.listen(&error)) {
        std::cerr << "startup failed: " << error << "\n";
        return 1;
    }
    std::cout << "TropiCRM listening on port " << app.port() << std::endl;
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tcrm::log::info("app", "shutting down");
    app.shutdown();
    return 0;
}
