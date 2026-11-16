// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "auth/user.hpp"
#include "engine/automation_types.hpp"
#include "meta/schema.hpp"
#include "security/profile.hpp"
#include "security/sharing.hpp"
#include "storage/database.hpp"
#include "util/id.hpp"
#include "util/time.hpp"

namespace tcrm::engine {

struct BusinessHours {
    int start_minute = 9 * 60;
    int end_minute = 17 * 60;
    unsigned workdays = 0x3E;  // bit per day_of_week(): Monday..Friday
    std::map<std::string, int> sla_hours = {{"High", 4}, {"Medium", 24}, {"Low", 72}};
};

struct OrgSettings {
    std::string timezone = "America/New_York";
    std::string currency = "USD";  // corporate currency
    std::string locale = "en_US";
    std::map<std::string, double> currency_rates;  // units of the currency per 1 corporate unit
    int64_t api_calls_per_day = 15000;
    int64_t storage_mb = 1024;
    int64_t max_query_rows = 2000;
    int recycle_bin_days = 15;
    bool duplicate_rules = true;
    bool web_forms = true;  // public web-to-lead / web-to-case endpoints
    BusinessHours hours;

    json::Value to_json() const;
    static OrgSettings from_json(const json::Value& v);
};

/// Daily API call accounting for one org.
struct ApiUsage {
    std::atomic<uint32_t> calls{0};
    std::atomic<int64_t> day{0};
};

/// One tenant. Everything here is guarded by mutex(): take it shared to
/// read and exclusive to change anything.
class Org {
public:
    Org(std::string id, std::string name, std::string pod, std::string dir);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::string& pod() const { return pod_; }
    int64_t created_at() const { return created_at_; }
    bool active() const { return active_; }
    void set_active(bool on);
    void set_name(const std::string& name);

    std::shared_mutex& mutex() const { return mu_; }

    /// Loads persisted state, or reports that the org has none yet.
    bool open(std::string* error, bool* is_new);

    /// Sets up a brand-new org: standard schema, default profiles and
    /// sharing, and the first administrator.
    void initialize(const std::string& admin_username, const std::string& admin_first, const std::string& admin_last,
                    const std::string& admin_email, const std::string& admin_password, uint32_t pbkdf2_iterations);

    // Persist one part of the org's configuration.
    void save_header();
    void save_schema();
    void save_users();
    void save_profiles();
    void save_roles();
    void save_sharing();
    void save_settings();
    void save_automation();
    void save_counters();

    std::string new_id(const std::string& key_prefix) { return ids.next(key_prefix); }
    timeutil::TimeZone tz() const;

    /// Next value for an auto-number field, e.g. "Case.CaseNumber".
    int64_t next_autonumber(const std::string& key);

    meta::Schema schema;
    storage::Database db;
    auth::UserStore users;
    security::ProfileStore profiles;
    security::RoleHierarchy roles;
    security::SharingModel sharing;
    OrgSettings settings;
    AutomationConfig automation;
    ids::Generator ids;
    std::map<std::string, int64_t> autonumbers;
    ApiUsage usage;

private:
    std::string id_;
    std::string name_;
    std::string pod_;
    int64_t created_at_ = 0;
    bool active_ = true;
    mutable std::shared_mutex mu_;
};

/// All orgs on this server.
class OrgRegistry {
public:
    /// `root` empty keeps everything in memory.
    OrgRegistry(std::string root, uint32_t pbkdf2_iterations);

    bool load(std::string* error);

    struct NewOrg {
        std::string name;
        std::string admin_username;
        std::string admin_first_name;
        std::string admin_last_name;
        std::string admin_email;
        std::string admin_password;
        std::string timezone;
        std::string currency;
    };

    /// Throws ApiError for invalid input.
    std::shared_ptr<Org> create(const NewOrg& spec);

    std::shared_ptr<Org> find(const std::string& id) const;
    std::shared_ptr<Org> find_by_username(const std::string& username) const;
    std::vector<std::shared_ptr<Org>> all() const;

    /// Usernames are unique platform-wide; the registry keeps the index.
    void index_user(const std::string& username, const std::string& org_id);
    void unindex_user(const std::string& username);
    bool username_taken(const std::string& username) const;

    /// Writes a snapshot for every org whose log has grown past `threshold`.
    void snapshot_if_needed(uint64_t threshold);

    uint32_t pbkdf2_iterations() const { return iterations_; }

private:
    std::string org_dir(const std::string& id) const;

    std::string root_;
    uint32_t iterations_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<Org>> orgs_;
    std::unordered_map<std::string, std::string> usernames_;  // lowercase username -> org id
    uint64_t next_pod_ = 1;
    ids::Generator org_ids_{"00", 1};
};

}  // namespace tcrm::engine
