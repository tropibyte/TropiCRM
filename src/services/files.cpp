// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/files.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "model/errors.hpp"
#include "util/strutil.hpp"

namespace fs = std::filesystem;

namespace tcrm::services {

std::string FileStore::sanitize(const std::string& name) {
    std::string safe = str::replace_all(name, "../", "");
    safe = str::replace_all(safe, "..\\", "");
    if (!safe.empty() && (safe[0] == '/' || safe[0] == '\\')) safe.erase(0, 1);
    std::string out;
    for (char c : safe) {
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20) {
            out += '_';
        } else {
            out += c;
        }
    }
    if (str::trim(out).empty()) out = "file";
    return out;
}

std::string FileStore::guess_content_type(const std::string& file_name) {
    const std::string n = str::to_lower(file_name);
    if (str::iends_with(n, ".pdf")) return "application/pdf";
    if (str::iends_with(n, ".png")) return "image/png";
    if (str::iends_with(n, ".jpg") || str::iends_with(n, ".jpeg")) return "image/jpeg";
    if (str::iends_with(n, ".gif")) return "image/gif";
    if (str::iends_with(n, ".txt")) return "text/plain";
    if (str::iends_with(n, ".csv")) return "text/csv";
    if (str::iends_with(n, ".json")) return "application/json";
    if (str::iends_with(n, ".docx")) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (str::iends_with(n, ".xlsx")) return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    return "application/octet-stream";
}

std::string FileStore::path_for(const std::string& org_id, const std::string& key) const {
    return (fs::path(root_) / "orgs" / org_id / "files" / key).string();
}

std::string FileStore::save(const std::string& org_id, const std::string& attachment_id, const std::string& file_name,
                            const std::string& bytes) {
    const std::string key = attachment_id + "/" + sanitize(file_name);
    if (root_.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        memory_[org_id + "/" + key] = bytes;
        return key;
    }
    const fs::path path = path_for(org_id, key);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) throw errors::internal("cannot create attachment directory: " + ec.message());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw errors::internal("cannot write attachment");
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw errors::internal("short write storing attachment");
    return key;
}

bool FileStore::load(const std::string& org_id, const std::string& key, std::string* bytes) const {
    if (root_.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = memory_.find(org_id + "/" + key);
        if (it == memory_.end()) return false;
        *bytes = it->second;
        return true;
    }
    std::ifstream in(path_for(org_id, key), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *bytes = ss.str();
    return true;
}

void FileStore::remove(const std::string& org_id, const std::string& key) {
    if (root_.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        memory_.erase(org_id + "/" + key);
        return;
    }
    std::error_code ec;
    fs::remove(path_for(org_id, key), ec);
}

}  // namespace tcrm::services
