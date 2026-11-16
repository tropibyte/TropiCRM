// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

// tropicrm-admin: offline administration of a TropiCRM data directory.
// Run it while the server is stopped; it opens the same files.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

#include "app/application.hpp"
#include "engine/records.hpp"
#include "meta/codec.hpp"
#include "query/executor.hpp"
#include "storage/database.hpp"
#include "util/crypto.hpp"
#include "util/id.hpp"
#include "util/csv.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"

namespace {

constexpr const char* kUsage = R"(usage: tropicrm-admin --data DIR <command> [arguments]

Commands:
  orgs                                   list orgs
  create-org NAME USERNAME PASSWORD      create an org and its first admin
  users ORG_ID                           list an org's users
  query USERNAME "SELECT ..."            run a query as a user, print JSON
  export ORG_ID OBJECT [FILE]            write every record of an object as CSV
  import USERNAME OBJECT FILE            insert records from a CSV file
  snapshot                               compact every org's log into a snapshot
  check ORG_ID                           verify references, indexes and roll-ups
  rebuild-indexes ORG_ID                 drop and rebuild every secondary index
  reset-password USERNAME NEW_PASSWORD   set a user's password
  export-org ORG_ID FILE                 write the org's metadata and records as JSON
  hash-key KEY                           print the SHA-256 for platform.admin_key_sha256
)";

int fail(const std::string& msg) {
    std::cerr << "error: " << msg << "\n";
    return 1;
}

tcrm::engine::Principal login_as(tcrm::Application& app, const std::string& username) {
    auto org = app.registry().find_by_username(username);
    if (!org) throw std::runtime_error("no such user: " + username);
    std::shared_lock<std::shared_mutex> lock(org->mutex());
    const tcrm::auth::User* u = org->users.find_by_username(username);
    if (u == nullptr) throw std::runtime_error("no such user: " + username);
    return tcrm::api::Authenticator::principal_for(org, *u);
}

int cmd_orgs(tcrm::Application& app) {
    for (const auto& org : app.registry().all()) {
        std::shared_lock<std::shared_mutex> lock(org->mutex());
        std::cout << org->id() << "  " << (org->active() ? "active  " : "disabled") << "  " << org->users.size()
                  << " users  " << org->name() << "\n";
    }
    return 0;
}

int cmd_users(tcrm::Application& app, const std::string& org_id) {
    auto org = app.registry().find(org_id);
    if (!org) return fail("no such org " + org_id);
    std::shared_lock<std::shared_mutex> lock(org->mutex());
    for (const auto& u : org->users.all()) {
        const tcrm::security::Profile* p = org->profiles.find(u.profile_id);
        std::cout << u.id << "  " << u.username << "  " << u.full_name() << "  " << (p ? p->name : "?")
                  << (u.active ? "" : "  (inactive)") << "\n";
    }
    return 0;
}

int cmd_export(tcrm::Application& app, const std::string& org_id, const std::string& object, const std::string& file) {
    auto org = app.registry().find(org_id);
    if (!org) return fail("no such org " + org_id);
    std::shared_lock<std::shared_mutex> lock(org->mutex());
    const tcrm::meta::ObjectDef* obj = org->schema.object(object);
    if (obj == nullptr) return fail("no such object " + object);
    std::ostringstream out;
    std::vector<std::string> header;
    for (const auto& f : obj->fields) header.push_back(f.name);
    out << tcrm::csv::format_row(header);
    if (const tcrm::storage::Table* t = org->db.find_table(obj->name)) {
        for (const auto& [id, r] : t->rows()) {
            if (r.is_deleted()) continue;
            std::vector<std::string> row;
            for (const auto& f : obj->fields) row.push_back(r.text(f.name));
            out << tcrm::csv::format_row(row);
        }
    }
    if (file.empty() || file == "-") {
        std::cout << out.str();
    } else {
        std::ofstream f(file, std::ios::binary);
        f << out.str();
        if (!f) return fail("cannot write " + file);
    }
    return 0;
}

int cmd_import(tcrm::Application& app, const std::string& username, const std::string& object, const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail("cannot read " + file);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();
    tcrm::engine::Principal who = login_as(app, username);
    tcrm::csv::Reader reader(data);
    std::vector<std::string> header, row;
    if (!reader.next(&header)) return fail("empty file");
    size_t ok = 0, failed = 0;
    while (reader.next(&row)) {
        if (row.size() != header.size()) {
            ++failed;
            continue;
        }
        tcrm::json::Value fields = tcrm::json::Value::object();
        for (size_t i = 0; i < header.size(); ++i) {
            if (!row[i].empty()) fields.set(std::string(tcrm::str::trim(header[i])), row[i]);
        }
        try {
            app.records().create(who, object, fields);
            ++ok;
        } catch (const tcrm::ApiError& e) {
            ++failed;
            std::cerr << "line " << reader.line() << ": " << e.code() << ": " << e.what() << "\n";
        }
    }
    if (!reader.error().empty()) std::cerr << reader.error() << "\n";
    std::cout << ok << " inserted, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

struct CheckReport {
    size_t records = 0;
    size_t problems = 0;

    void problem(const std::string& what) {
        ++problems;
        if (problems <= 50) std::cout << "  " << what << "\n";
        if (problems == 51) std::cout << "  (more problems not shown)\n";
    }
};

int cmd_check(tcrm::Application& app, const std::string& org_id) {
    auto org = app.registry().find(org_id);
    if (!org) return fail("no such org " + org_id);
    std::shared_lock<std::shared_mutex> lock(org->mutex());
    CheckReport report;
    for (const tcrm::meta::ObjectDef* obj : org->schema.objects()) {
        if (obj->name == "User") continue;
        const tcrm::storage::Table* t = org->db.find_table(obj->name);
        if (t == nullptr) continue;
        for (const auto& [id, r] : t->rows()) {
            ++report.records;
            if (r.id != id || r.text("Id") != id) report.problem(obj->name + " " + id + ": id mismatch");
            if (tcrm::ids::key_prefix(id) != obj->key_prefix) report.problem(obj->name + " " + id + ": wrong key prefix");
            if (r.is_deleted()) continue;
            for (const tcrm::meta::FieldDef& f : obj->fields) {
                const tcrm::FieldValue& v = r.get(f.name);
                if (f.required && !f.is_calculated() && !f.system && v.is_null()) {
                    report.problem(obj->name + " " + id + ": required field " + f.name + " is empty");
                }
                if (!tcrm::meta::is_reference_type(f.type) || v.kind() != tcrm::ValueKind::String) continue;
                if (f.reference_to == "User") {
                    if (org->users.find(v.as_string()) == nullptr) {
                        report.problem(obj->name + " " + id + ": " + f.name + " points at unknown user " + v.as_string());
                    }
                    continue;
                }
                if (!tcrm::engine::load_record(*org, v.as_string())) {
                    report.problem(obj->name + " " + id + ": " + f.name + " points at missing record " + v.as_string());
                }
            }
        }
        // Every index entry must match its record, and every value must be indexed.
        for (const std::string& field : t->indexed_fields()) {
            const tcrm::storage::Index* idx = t->index(field);
            size_t indexed_values = 0;
            for (const auto& [id, r] : t->rows()) {
                const tcrm::FieldValue& v = r.get(field);
                if (v.is_null()) continue;
                ++indexed_values;
                const auto holders = idx->find(v);
                if (std::find(holders.begin(), holders.end(), id) == holders.end()) {
                    report.problem(obj->name + "." + field + ": " + id + " is missing from the index");
                }
            }
            if (idx->size() != indexed_values) {
                report.problem(obj->name + "." + field + ": index has " + std::to_string(idx->size()) + " entries for " +
                               std::to_string(indexed_values) + " values");
            }
        }
    }
    std::cout << report.records << " records checked, " << report.problems << " problems\n";
    return report.problems == 0 ? 0 : 1;
}

int cmd_rebuild_indexes(tcrm::Application& app, const std::string& org_id) {
    auto org = app.registry().find(org_id);
    if (!org) return fail("no such org " + org_id);
    std::unique_lock<std::shared_mutex> lock(org->mutex());
    size_t rebuilt = 0;
    for (const tcrm::meta::ObjectDef* obj : org->schema.objects()) {
        tcrm::storage::Table& t = org->db.table(obj->name);
        for (const tcrm::meta::FieldDef& f : obj->fields) {
            if (!f.indexed && !f.unique && !f.external_id) continue;
            t.drop_index(f.name);
            t.ensure_index(f.name, f.unique || f.external_id);
            ++rebuilt;
        }
    }
    std::cout << rebuilt << " indexes rebuilt\n";
    return 0;
}

int cmd_reset_password(tcrm::Application& app, const std::string& username, const std::string& password) {
    auto org = app.registry().find_by_username(username);
    if (!org) return fail("no such user " + username);
    std::string reason;
    if (!tcrm::auth::password_meets_policy(password, &reason)) return fail(reason);
    std::unique_lock<std::shared_mutex> lock(org->mutex());
    tcrm::auth::User* u = org->users.find_by_username(username);
    if (u == nullptr) return fail("no such user " + username);
    tcrm::auth::set_password(u, password, app.registry().pbkdf2_iterations());
    u->failed_logins = 0;
    u->locked_until = 0;
    org->save_users();
    std::cout << "password updated for " << u->username << "\n";
    return 0;
}

int cmd_export_org(tcrm::Application& app, const std::string& org_id, const std::string& file) {
    auto org = app.registry().find(org_id);
    if (!org) return fail("no such org " + org_id);
    std::shared_lock<std::shared_mutex> lock(org->mutex());
    tcrm::json::Value doc = tcrm::json::Value::object();
    doc.set("orgId", org->id());
    doc.set("name", org->name());
    doc.set("schema", org->schema.custom_to_json());
    doc.set("settings", org->settings.to_json());
    doc.set("sharing", org->sharing.to_json());
    doc.set("automation", org->automation.to_json());
    tcrm::json::Value tables = tcrm::json::Value::object();
    size_t count = 0;
    for (const std::string& name : org->db.table_names()) {
        tcrm::json::Value rows = tcrm::json::Value::array();
        for (const auto& [id, r] : org->db.find_table(name)->rows()) {
            rows.push_back(tcrm::storage::record_to_json(r));
            ++count;
        }
        tables.set(name, std::move(rows));
    }
    doc.set("records", std::move(tables));
    std::ofstream out(file, std::ios::binary);
    out << doc.dump(true) << "\n";
    if (!out) return fail("cannot write " + file);
    std::cout << count << " records exported to " << file << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string data_dir;
    if (args.size() >= 2 && args[0] == "--data") {
        data_dir = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.empty()) {
        std::cerr << kUsage;
        return 2;
    }
    const std::string cmd = args[0];
    if (cmd == "hash-key") {
        if (args.size() != 2) return fail("hash-key KEY");
        std::cout << tcrm::crypto::to_hex(tcrm::crypto::sha256(args[1])) << "\n";
        return 0;
    }
    if (data_dir.empty()) return fail("--data DIR is required");

    tcrm::log::Logger::instance().set_level(tcrm::log::Level::Warn);
    tcrm::Config config = tcrm::Config::defaults();
    config.set("data.dir", data_dir);
    config.set("log.requests", "false");
    tcrm::Application app(config);
    std::string error;
    if (!app.init(&error)) return fail(error);

    try {
        if (cmd == "orgs") return cmd_orgs(app);
        if (cmd == "create-org") {
            if (args.size() != 4) return fail("create-org NAME USERNAME PASSWORD");
            tcrm::engine::OrgRegistry::NewOrg spec;
            spec.name = args[1];
            spec.admin_username = args[2];
            spec.admin_last_name = "Admin";
            spec.admin_email = args[2];
            spec.admin_password = args[3];
            auto org = app.registry().create(spec);
            std::cout << org->id() << "\n";
            return 0;
        }
        if (cmd == "users") {
            if (args.size() != 2) return fail("users ORG_ID");
            return cmd_users(app, args[1]);
        }
        if (cmd == "query") {
            if (args.size() != 3) return fail("query USERNAME \"SELECT ...\"");
            tcrm::engine::Principal who = login_as(app, args[1]);
            tcrm::query::ExecOptions opts;
            opts.batch_size = 50000;
            std::cout << tcrm::query::execute(args[2], who, opts, nullptr).to_json().dump(true) << "\n";
            return 0;
        }
        if (cmd == "export") {
            if (args.size() < 3) return fail("export ORG_ID OBJECT [FILE]");
            return cmd_export(app, args[1], args[2], args.size() > 3 ? args[3] : "");
        }
        if (cmd == "import") {
            if (args.size() != 4) return fail("import USERNAME OBJECT FILE");
            return cmd_import(app, args[1], args[2], args[3]);
        }
        if (cmd == "snapshot") {
            app.registry().snapshot_if_needed(0);
            return 0;
        }
        if (cmd == "check") {
            if (args.size() != 2) return fail("check ORG_ID");
            return cmd_check(app, args[1]);
        }
        if (cmd == "rebuild-indexes") {
            if (args.size() != 2) return fail("rebuild-indexes ORG_ID");
            return cmd_rebuild_indexes(app, args[1]);
        }
        if (cmd == "reset-password") {
            if (args.size() != 3) return fail("reset-password USERNAME NEW_PASSWORD");
            return cmd_reset_password(app, args[1], args[2]);
        }
        if (cmd == "export-org") {
            if (args.size() != 3) return fail("export-org ORG_ID FILE");
            return cmd_export_org(app, args[1], args[2]);
        }
    } catch (const tcrm::ApiError& e) {
        return fail(e.code() + ": " + e.what());
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    std::cerr << kUsage;
    return 2;
}
