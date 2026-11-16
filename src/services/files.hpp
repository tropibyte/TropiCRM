// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <mutex>
#include <string>

namespace tcrm::services {

/// Stores attachment bodies on disk under <root>/orgs/<org>/files/, or in
/// memory when no root is configured.
class FileStore {
public:
    explicit FileStore(std::string root = {}) : root_(std::move(root)) {}

    /// Writes `bytes` and returns the storage key to keep on the record.
    std::string save(const std::string& org_id, const std::string& attachment_id, const std::string& file_name,
                     const std::string& bytes);

    bool load(const std::string& org_id, const std::string& key, std::string* bytes) const;
    void remove(const std::string& org_id, const std::string& key);

    /// Makes a client-supplied file name safe to use as a path component.
    static std::string sanitize(const std::string& name);

    static std::string guess_content_type(const std::string& file_name);

private:
    std::string path_for(const std::string& org_id, const std::string& key) const;

    std::string root_;
    mutable std::mutex mu_;
    std::map<std::string, std::string> memory_;  // org/key -> bytes
};

}  // namespace tcrm::services
