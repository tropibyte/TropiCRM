// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <filesystem>
#include <fstream>

#include "harness.hpp"
#include "meta/codec.hpp"
#include "meta/schema.hpp"
#include "security/profile.hpp"
#include "security/sharing.hpp"
#include "services/search.hpp"
#include "storage/database.hpp"
#include "util/crypto.hpp"
#include "util/time.hpp"

using namespace tcrm;
namespace fs = std::filesystem;

namespace {

/// A unique scratch directory removed at the end of the test.
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("tropicrm-test-" + crypto::to_hex(crypto::random_bytes(6)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

Record sample(const std::string& id, const std::string& name) {
    Record r;
    r.id = id;
    r.object = "Account";
    r.version = 3;
    r.set("Id", FieldValue::string(id));
    r.set("Name", FieldValue::string(name));
    r.set("NumberOfEmployees", FieldValue::integer(42));
    r.set("AnnualRevenue", FieldValue::number(12.5));
    r.set("IsDeleted", FieldValue::boolean(false));
    r.set("Founded", FieldValue::date(20000));
    r.set("CreatedDate", FieldValue::datetime(1767225600000));
    return r;
}

}  // namespace

TEST(storage, record_json_round_trip) {
    const Record r = sample("001000000000001AAA", "Acme");
    const Record back = storage::record_from_json(storage::record_to_json(r));
    CHECK_EQ(back.id, r.id);
    CHECK_EQ(back.object, r.object);
    CHECK_EQ(back.version, uint64_t{3});
    CHECK(back.get("Founded").kind() == ValueKind::Date);
    CHECK_EQ(back.get("Founded").as_date(), int64_t{20000});
    CHECK(back.get("CreatedDate").kind() == ValueKind::DateTime);
    CHECK_EQ(back.get("CreatedDate").as_datetime(), int64_t{1767225600000});
    CHECK_EQ(back.get("NumberOfEmployees").as_int(), int64_t{42});
    CHECK_NEAR(back.get("AnnualRevenue").as_double(), 12.5, 1e-12);
    CHECK_EQ(back.get("IsDeleted").as_bool(), false);
}

TEST(storage, table_indexes) {
    storage::Table t("Contact");
    t.ensure_index("Email", false);
    for (int i = 0; i < 5; ++i) {
        Record r;
        r.id = "003" + std::to_string(i);
        r.object = "Contact";
        r.set("Email", FieldValue::string(i < 3 ? "shared" : "solo" + std::to_string(i)));
        r.set("Score", FieldValue::integer(i * 10));
        t.put(r);
    }
    CHECK_EQ(t.index("Email")->find(FieldValue::string("shared")).size(), size_t{3});
    t.ensure_index("Score", false);
    const FieldValue lo = FieldValue::integer(10), hi = FieldValue::integer(30);
    CHECK_EQ(t.index("Score")->range(&lo, true, &hi, false).size(), size_t{2});
    CHECK_EQ(t.index("Score")->range(&lo, false, nullptr, false).size(), size_t{3});
    // Changing a value moves the index entry.
    Record r = *t.get("0030");
    r.set("Email", FieldValue::string("moved"));
    t.put(r);
    CHECK_EQ(t.index("Email")->find(FieldValue::string("shared")).size(), size_t{2});
    CHECK_EQ(t.index("Email")->find(FieldValue::string("moved")).size(), size_t{1});
    CHECK(t.index("Email")->taken_by_other(FieldValue::string("moved"), "0031"));
    CHECK(!t.index("Email")->taken_by_other(FieldValue::string("moved"), "0030"));
    CHECK(t.erase("0030"));
    CHECK(!t.erase("0030"));
    CHECK(t.index("Email")->find(FieldValue::string("moved")).empty());
    CHECK_EQ(t.size(), size_t{4});
}

TEST(storage, wal_crc_and_replay) {
    TempDir dir;
    const std::string path = (dir.path / "wal.log").string();
    {
        storage::Wal wal;
        std::string err;
        REQUIRE(wal.open(path, &err));
        for (int i = 0; i < 3; ++i) {
            json::Value e = json::Value::object();
            e.set("n", i);
            CHECK(wal.append(e));
        }
        CHECK_EQ(wal.entries(), uint64_t{3});
    }
    // Simulate a crash halfway through a fourth append.
    {
        std::ofstream out(path, std::ios::app | std::ios::binary);
        out << "deadbeef 40 {\"n\":";
    }
    std::vector<int64_t> seen;
    storage::Wal::ReplayStats stats;
    std::string err;
    REQUIRE(storage::Wal::replay(path, [&](const json::Value& e) { seen.push_back(e.get_int("n")); }, &stats, &err));
    CHECK_EQ(seen.size(), size_t{3});
    CHECK_EQ(stats.applied, size_t{3});
    CHECK(stats.truncated_tail);
    CHECK_EQ(storage::crc32("123456789"), uint32_t{0xCBF43926});
}

TEST(storage, database_reopens_from_log_and_snapshot) {
    TempDir dir;
    {
        storage::Database db(dir.str());
        std::string err;
        REQUIRE(db.open(&err));
        db.put(sample("001000000000001AAA", "First"));
        db.put(sample("001000000000002AAA", "Second"));
        json::Value meta = json::Value::object();
        meta.set("answer", 42);
        db.put_meta("settings", meta);
        db.erase("Account", "001000000000002AAA");
        CHECK_EQ(db.log_entries(), uint64_t{4});
    }
    {
        storage::Database db(dir.str());
        std::string err;
        REQUIRE(db.open(&err));
        REQUIRE(db.find_table("Account") != nullptr);
        CHECK_EQ(db.find_table("Account")->size(), size_t{1});
        CHECK_EQ(db.meta("settings")->get_int("answer"), int64_t{42});
        REQUIRE(db.snapshot(&err));
        CHECK_EQ(db.log_entries(), uint64_t{0});
        db.put(sample("001000000000003AAA", "Third"));
    }
    {
        storage::Database db(dir.str());
        std::string err;
        REQUIRE(db.open(&err));
        CHECK_EQ(db.find_table("Account")->size(), size_t{2});
        CHECK_EQ(db.find_table("Account")->get("001000000000003AAA")->text("Name"), std::string("Third"));
        CHECK(db.approximate_bytes() > 0);
    }
}

TEST(storage, orgs_survive_a_restart) {
    TempDir dir;
    Config config = testing::test_config();
    config.set("data.dir", dir.str());
    const std::string user = testing::address("persist");
    std::string account_id;
    {
        testing::TestApp api(config);
        api.signup("Durable Inc", user, "Durable2026x");
        const std::string token = api.login(user, "Durable2026x");
        account_id = api.create(token, "Account", json::parse(R"({"Name":"Survivor","Industry":"Energy"})"));
        REQUIRE_EQ(api.post("/services/data/v1/tooling/objects/Account/fields",
                            json::parse(R"({"name":"Tier__c","type":"picklist","picklistValues":["Gold","Silver"]})"),
                            token)
                       .status,
                   201);
        REQUIRE_EQ(api.patch("/services/data/v1/sobjects/Account/" + account_id, json::parse(R"({"Tier__c":"Gold"})"), token)
                       .status,
                   204);
        api.app.shutdown();
    }
    {
        testing::TestApp api(config);
        const std::string token = api.login(user, "Durable2026x");
        auto r = api.get("/services/data/v1/sobjects/Account/" + account_id, token);
        REQUIRE_EQ(r.status, 200);
        CHECK_EQ(r.json.get_string("Name"), std::string("Survivor"));
        CHECK_EQ(r.json.get_string("Tier__c"), std::string("Gold"));
        // New ids don't collide with stored ones.
        const std::string next = api.create(token, "Account", json::parse(R"({"Name":"Newcomer"})"));
        CHECK(next > account_id);
        CHECK_EQ(api.get("/services/data/v1/search?q=survivor", token).json.at("searchRecords").size(), size_t{1});
        api.app.shutdown();
    }
    CHECK(fs::exists(dir.path / "signing.key"));
}

TEST(schema, standard_objects_and_relationships) {
    const meta::Schema s = meta::Schema::standard();
    const meta::ObjectDef* account = s.object("account");
    REQUIRE(account != nullptr);
    CHECK_EQ(account->key_prefix, std::string("001"));
    CHECK(account->field("NAME") != nullptr);
    CHECK(account->field("OwnerId") != nullptr);
    CHECK(s.object_by_prefix("003") == s.object("Contact"));
    CHECK(s.object("Contact")->relationship_field("Account") != nullptr);
    bool has_contacts = false, has_opps = false;
    for (const auto& c : account->children) {
        has_contacts |= c.relationship_name == "Contacts";
        has_opps |= c.relationship_name == "Opportunities";
    }
    CHECK(has_contacts);
    CHECK(has_opps);
    CHECK(s.object("Case")->field("CaseNumber")->type == meta::FieldType::AutoNumber);
    CHECK(s.objects().size() >= 13);
}

TEST(schema, custom_definitions_round_trip) {
    meta::Schema s = meta::Schema::standard();
    s.create_custom_object("Invoice__c", "Invoice", "Invoices", "Billing");
    meta::FieldDef amount;
    amount.name = "Total__c";
    amount.type = meta::FieldType::Currency;
    s.add_custom_field("Invoice__c", amount);
    meta::FieldDef link;
    link.name = "Account__c";
    link.type = meta::FieldType::Lookup;
    link.reference_to = "account";
    s.add_custom_field("Invoice__c", link);
    meta::FieldDef roll;
    roll.name = "InvoiceTotal__c";
    roll.type = meta::FieldType::RollupSummary;
    roll.rollup = {"Invoice__c", "Account__c", "sum", "Total__c"};
    s.add_custom_field("Account", roll);
    CHECK_EQ(s.object("Invoice__c")->field("Account__c")->reference_to, std::string("Account"));
    CHECK_EQ(s.object("Invoice__c")->field("Account__c")->relationship_name, std::string("Account__r"));

    meta::Schema restored = meta::Schema::standard();
    restored.load_custom_json(s.custom_to_json());
    const meta::ObjectDef* inv = restored.object("Invoice__c");
    REQUIRE(inv != nullptr);
    CHECK_EQ(inv->key_prefix, s.object("Invoice__c")->key_prefix);
    CHECK(inv->field("Total__c") != nullptr);
    CHECK_EQ(restored.object("Account")->field("InvoiceTotal__c")->rollup.op, std::string("SUM"));
    bool rel = false;
    for (const auto& c : restored.object("Account")->children) rel |= c.child_object == "Invoice__c";
    CHECK(rel);

    CHECK_API_ERROR(s.create_custom_object("Invoice__c", "", "", ""), "DUPLICATE_DEVELOPER_NAME");
    CHECK_API_ERROR(s.create_custom_object("Bad Name__c", "", "", ""), "INVALID_NAME");
    CHECK_API_ERROR(s.create_custom_object("double__x__c", "", "", ""), "INVALID_NAME");
    meta::FieldDef bad;
    bad.name = "Broken__c";
    bad.type = meta::FieldType::Lookup;
    bad.reference_to = "Nope";
    CHECK_API_ERROR(s.add_custom_field("Account", bad), "INVALID_FIELD");
    meta::FieldDef md;
    md.name = "Parent__c";
    md.type = meta::FieldType::MasterDetail;
    md.reference_to = "Account";
    CHECK_API_ERROR(s.add_custom_field("Contact", md), "INVALID_TYPE");
    CHECK_API_ERROR(s.remove_custom_field("Account", "Name"), "CANNOT_DELETE");
    const uint64_t v = s.version();
    s.remove_custom_field("Invoice__c", "Total__c");
    CHECK(s.version() > v);
}

TEST(schema, field_codec) {
    const meta::Schema s = meta::Schema::standard();
    const meta::ObjectDef* opp = s.object("Opportunity");
    const meta::FieldDef& amount = *opp->field("Amount");
    CHECK_NEAR(meta::value_from_json(amount, json::Value(10.006)).as_double(), 10.01, 1e-9);
    CHECK_NEAR(meta::value_from_json(amount, json::Value("12.5")).as_double(), 12.5, 1e-9);
    CHECK(meta::value_from_json(amount, json::Value()).is_null());
    CHECK_API_ERROR(meta::value_from_json(amount, json::Value(true)), "INVALID_FIELD");
    const meta::FieldDef& close = *opp->field("CloseDate");
    CHECK_EQ(timeutil::format_date(meta::value_from_json(close, json::Value("2026-07-04")).as_date()),
             std::string("2026-07-04"));
    CHECK_EQ(meta::value_to_json(close, FieldValue::date(0)).as_string(), std::string("1970-01-01"));
    const meta::FieldDef& stage = *opp->field("StageName");
    CHECK_API_ERROR(meta::value_from_json(stage, json::Value("Haggling")), "INVALID_OR_NULL_FOR_RESTRICTED_PICKLIST");
    const meta::FieldDef& account = *opp->field("AccountId");
    CHECK_EQ(meta::value_from_json(account, json::Value("001A0000006Vm9r")).as_string(), std::string("001A0000006Vm9rIAC"));
    CHECK_EQ(meta::value_from_text(*s.object("Contact")->field("DoNotCall"), "yes").as_bool(), true);
    CHECK(meta::value_from_text(*s.object("Contact")->field("Phone"), "").is_null());
    meta::FieldDef multi;
    multi.name = "Tags__c";
    multi.type = meta::FieldType::MultiPicklist;
    multi.picklist = {{"a", "a", false, true}, {"b", "b", false, true}};
    multi.restricted_picklist = true;
    CHECK_EQ(meta::value_from_json(multi, json::parse(R"(["b","a","b"])")).as_string(), std::string("b;a"));
    CHECK_API_ERROR(meta::value_from_json(multi, json::Value("a;z")), "INVALID_OR_NULL_FOR_RESTRICTED_PICKLIST");
    CHECK(meta::is_valid_email(testing::address("first.last+tag", "sub.example")));
    CHECK(!meta::is_valid_email("missing-at.test"));
    CHECK(!meta::is_valid_email("two@@at.test"));
}

TEST(schema, profiles_and_sharing_json) {
    for (const security::Profile& p : security::default_profiles()) {
        const security::Profile back = security::Profile::from_json(p.to_json());
        CHECK_EQ(back.id, p.id);
        CHECK_EQ(back.modify_all_data, p.modify_all_data);
        CHECK_EQ(back.object_permissions("Account").edit, p.object_permissions("Account").edit);
    }
    security::Profile limited;
    limited.objects["Account"] = security::ObjectPermissions::read_only();
    limited.fields["Account.AnnualRevenue"] = {false, false};
    CHECK(!limited.field_permission("Account", "AnnualRevenue").read);
    CHECK(limited.field_permission("Account", "Name").read);
    CHECK(!limited.object_permissions("Contact").read);

    security::SharingModel m = security::SharingModel::standard();
    m.rules.push_back({"02c1", "rule", "Account", "Industry", "Energy", "00E1", security::Access::Edit});
    m.manual.push_back({"001x", "005x", security::Access::Read, "why"});
    const security::SharingModel back = security::SharingModel::from_json(m.to_json());
    CHECK(back.default_for("Lead") == security::OrgDefault::PublicReadWrite);
    CHECK(back.default_for("Unknown__c") == security::OrgDefault::Private);
    REQUIRE_EQ(back.rules.size(), size_t{1});
    CHECK(back.rules[0].access == security::Access::Edit);
    REQUIRE_EQ(back.manual.size(), size_t{1});
    CHECK_EQ(back.manual[0].reason, std::string("why"));

    security::RoleHierarchy h;
    h.put({"00E1", "CEO", ""});
    h.put({"00E2", "VP", "00E1"});
    h.put({"00E3", "Rep", "00E2"});
    CHECK(h.is_above("00E1", "00E3"));
    CHECK(!h.is_above("00E3", "00E1"));
    CHECK_EQ(h.role_and_subordinates("00E2").size(), size_t{2});
    CHECK_API_ERROR(h.put({"00E4", "Orphan", "00E9"}), "INVALID_FIELD");
    CHECK_API_ERROR(h.remove("00E2"), "DELETE_FAILED");
}

TEST(search_index, tokenize_and_rank) {
    const auto toks = services::tokenize("Acme, Inc. -- the ROAD-runner co. (est. 1949)");
    CHECK_EQ(toks.size(), size_t{8});
    CHECK_EQ(toks[0], std::string("acme"));
    services::SearchIndex idx;
    idx.index("Account", "a1", services::tokenize("Acme Rockets"));
    idx.index("Account", "a2", services::tokenize("Acme Acme Anvils"));
    idx.index("Contact", "c1", services::tokenize("Wile Coyote Acme"));
    auto hits = idx.search({"acme"}, {}, 10);
    CHECK_EQ(hits.size(), size_t{3});
    hits = idx.search({"acme", "rock"}, {}, 10);
    REQUIRE_EQ(hits.size(), size_t{1});
    CHECK_EQ(hits[0].id, std::string("a1"));
    hits = idx.search({"acme"}, {"Contact"}, 10);
    REQUIRE_EQ(hits.size(), size_t{1});
    CHECK_EQ(hits[0].object, std::string("Contact"));
    idx.index("Account", "a1", services::tokenize("Renamed Rockets"));
    CHECK_EQ(idx.search({"acme"}, {}, 10).size(), size_t{2});
    idx.remove("a2");
    CHECK_EQ(idx.search({"anvils"}, {}, 10).size(), size_t{0});
    CHECK_EQ(idx.search({"acme"}, {}, 1).size(), size_t{1});
}
