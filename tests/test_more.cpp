// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

// Broader end-to-end coverage: bulk operations, multi-select picklists,
// date literals, notes, custom formula types and describe for custom objects.

#include <chrono>
#include <thread>

#include "harness.hpp"
#include "util/time.hpp"

using namespace tcrm;
using testing::Fixture;

namespace {

bool wait_for_job(Fixture& f, const std::string& id) {
    for (int i = 0; i < 500; ++i) {
        const std::string state = f.api.get("/services/data/v1/jobs/ingest/" + id, f.admin_token).json.get_string("state");
        if (state == "JobComplete" || state == "Failed") return state == "JobComplete";
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::string run_job(Fixture& f, const std::string& object, const std::string& operation, const std::string& csv,
                    const std::string& external_id = {}) {
    json::Value body = json::Value::object();
    body.set("object", object);
    body.set("operation", operation);
    if (!external_id.empty()) body.set("externalIdFieldName", external_id);
    auto job = f.api.post("/services/data/v1/jobs/ingest", body, f.admin_token);
    if (job.status != 201) {
        testing::record_failure(__FILE__, __LINE__, "job create failed: " + job.body);
        throw testing::AbortTest("job create failed");
    }
    const std::string id = job.json.get_string("id");
    f.api.call("PUT", "/services/data/v1/jobs/ingest/" + id + "/batches", csv, f.admin_token, {{"Content-Type", "text/csv"}});
    f.api.patch("/services/data/v1/jobs/ingest/" + id, f.obj({{"state", "UploadComplete"}}), f.admin_token);
    if (!wait_for_job(f, id)) testing::record_failure(__FILE__, __LINE__, "job did not complete");
    return id;
}

}  // namespace

TEST(bulk_ops, update_upsert_and_delete) {
    Fixture f;
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/objects/Contact/fields",
                          json::parse(R"({"name":"Legacy_Id__c","type":"string","length":20,"externalId":true})"),
                          f.admin_token)
                   .status,
               201);
    const std::string insert = run_job(f, "Contact", "insert",
                                       "LastName,Legacy_Id__c,Title\nAlpha,L-1,Engineer\nBravo,L-2,Analyst\n");
    CHECK_EQ(f.api.get("/services/data/v1/jobs/ingest/" + insert, f.admin_token).json.get_int("numberRecordsFailed"),
             int64_t{0});
    auto rows = f.api.query("SELECT Id, LastName FROM Contact ORDER BY LastName", f.admin_token);
    REQUIRE_EQ(rows.json.get_int("totalSize"), int64_t{2});
    const std::string alpha = rows.json.at("records")[0].get_string("Id");

    run_job(f, "Contact", "update", "Id,Title\n" + alpha + ",Lead Engineer\n");
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Contact/" + alpha, f.admin_token).json.get_string("Title"),
             std::string("Lead Engineer"));

    const std::string upsert = run_job(f, "Contact", "upsert", "Legacy_Id__c,LastName\nL-2,Bravo-Updated\nL-3,Charlie\n",
                                       "Legacy_Id__c");
    auto results = f.api.get("/services/data/v1/jobs/ingest/" + upsert + "/successfulResults", f.admin_token);
    CHECK(results.body.find(",true,L-3,Charlie") != std::string::npos);
    CHECK(results.body.find(",false,L-2,Bravo-Updated") != std::string::npos);
    CHECK_EQ(f.api.query("SELECT Id FROM Contact", f.admin_token).json.get_int("totalSize"), int64_t{3});

    const std::string del = run_job(f, "Contact", "delete", "Id\n" + alpha + "\n003000000000000AAA\n");
    auto info = f.api.get("/services/data/v1/jobs/ingest/" + del, f.admin_token);
    CHECK_EQ(info.json.get_int("numberRecordsProcessed"), int64_t{2});
    CHECK_EQ(info.json.get_int("numberRecordsFailed"), int64_t{1});
    CHECK_EQ(f.api.query("SELECT Id FROM Contact", f.admin_token).json.get_int("totalSize"), int64_t{2});
}

TEST(bulk_ops, abort_and_state_rules) {
    Fixture f;
    auto job = f.api.post("/services/data/v1/jobs/ingest", f.obj({{"object", "Lead"}, {"operation", "insert"}}), f.admin_token);
    const std::string id = job.json.get_string("id");
    CHECK_EQ(f.api.del("/services/data/v1/jobs/ingest/" + id, f.admin_token).status, 400);
    auto aborted = f.api.patch("/services/data/v1/jobs/ingest/" + id, f.obj({{"state", "Aborted"}}), f.admin_token);
    CHECK_EQ(aborted.json.get_string("state"), std::string("Aborted"));
    CHECK_EQ(f.api.call("PUT", "/services/data/v1/jobs/ingest/" + id + "/batches", "LastName\nx\n", f.admin_token).status, 400);
    CHECK_EQ(f.api.patch("/services/data/v1/jobs/ingest/" + id, f.obj({{"state", "UploadComplete"}}), f.admin_token).status, 400);
    CHECK_EQ(f.api.patch("/services/data/v1/jobs/ingest/" + id, f.obj({{"state", "Sideways"}}), f.admin_token).status, 400);
    CHECK_EQ(f.api.get("/services/data/v1/jobs/ingest/" + id + "/failedResults", f.admin_token).status, 400);
    const std::string user = testing::address("nobulk");
    f.api.create_user(f.admin_token, user, "Nobulk", "Standard User", "Nobulk1234");
    const std::string token = f.api.login(user, "Nobulk1234");
    CHECK_EQ(f.api.post("/services/data/v1/jobs/ingest", f.obj({{"object", "Lead"}, {"operation", "insert"}}), token).status,
             403);
    CHECK_EQ(f.api.get("/services/data/v1/jobs/ingest/" + id, token).status, 404);
}

TEST(picklists, multi_select_and_includes) {
    Fixture f;
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/objects/Account/fields",
                          json::parse(R"({"name":"Channels__c","type":"multipicklist",
                                         "picklistValues":["Email","Phone","Chat","Social"],"restrictedPicklist":true})"),
                          f.admin_token)
                   .status,
               201);
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Omni"}, {"Channels__c", "Email;Chat;Social"}}));
    json::Value arr = json::Value::array();
    arr.push_back("Phone");
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Callers"}, {"Channels__c", arr}}));
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Quiet"}}));
    auto names = [&](const std::string& where) {
        auto r = f.api.query("SELECT Name FROM Account WHERE " + where + " ORDER BY Name", f.admin_token);
        std::vector<std::string> out;
        for (const json::Value& rec : r.json.at("records").as_array()) out.push_back(rec.get_string("Name"));
        return out;
    };
    CHECK(names("Channels__c INCLUDES ('Chat')") == std::vector<std::string>({"Omni"}));
    CHECK(names("Channels__c INCLUDES ('Phone', 'Email;Social')") == std::vector<std::string>({"Callers", "Omni"}));
    CHECK(names("Channels__c INCLUDES ('Email;Phone')").empty());
    CHECK(names("Channels__c EXCLUDES ('Phone')") == std::vector<std::string>({"Omni", "Quiet"}));
    auto bad = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", "Bad"}, {"Channels__c", "Fax"}}), f.admin_token);
    CHECK_EQ(bad.status, 400);
    auto rec = f.api.query("SELECT Channels__c FROM Account WHERE Name = 'Callers'", f.admin_token);
    CHECK_EQ(rec.json.at("records")[0].get_string("Channels__c"), std::string("Phone"));
}

TEST(query_dates, relative_literals) {
    Fixture f;
    const int64_t today = timeutil::today_utc();
    auto day = [&](int64_t offset) { return timeutil::format_date(today + offset); };
    f.api.create(f.admin_token, "Task", f.obj({{"Subject", "Overdue"}, {"ActivityDate", day(-10)}}));
    f.api.create(f.admin_token, "Task", f.obj({{"Subject", "Soon"}, {"ActivityDate", day(3)}}));
    f.api.create(f.admin_token, "Task", f.obj({{"Subject", "Far"}, {"ActivityDate", day(200)}}));
    f.api.create(f.admin_token, "Task", f.obj({{"Subject", "Undated"}}));
    auto count = [&](const std::string& where) {
        return f.api.query("SELECT COUNT() FROM Task WHERE " + where, f.admin_token).json.get_int("totalSize");
    };
    CHECK_EQ(count("ActivityDate = NEXT_N_DAYS:7"), int64_t{1});
    CHECK_EQ(count("ActivityDate = LAST_N_DAYS:30"), int64_t{1});
    CHECK_EQ(count("ActivityDate < TODAY"), int64_t{1});
    CHECK_EQ(count("ActivityDate > NEXT_90_DAYS"), int64_t{1});
    CHECK_EQ(count("ActivityDate != null"), int64_t{3});
    CHECK_EQ(count("ActivityDate = N_DAYS_AGO:10"), int64_t{1});
    CHECK_EQ(count("ActivityDate >= LAST_YEAR"), int64_t{3});
    CHECK_EQ(f.api.query("SELECT COUNT() FROM Task WHERE ActivityDate = LAST_N_DAYS:-1", f.admin_token).status, 400);
    CHECK_EQ(f.api.query("SELECT COUNT() FROM Task WHERE ActivityDate = LAST_N_DAYS", f.admin_token).status, 400);
}

TEST(notes, attach_to_any_record) {
    Fixture f;
    const std::string acct = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Noted"}}));
    const std::string opp = f.api.create(
        f.admin_token, "Opportunity", f.obj({{"Name", "Noted deal"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-31"}}));
    f.api.create(f.admin_token, "Note", f.obj({{"Title", "Kickoff"}, {"Body", "Went well"}, {"ParentId", acct}}));
    f.api.create(f.admin_token, "Note", f.obj({{"Title", "Pricing"}, {"ParentId", opp}}));
    auto r = f.api.query("SELECT Title, Parent.Name FROM Note ORDER BY Title", f.admin_token);
    REQUIRE_EQ(r.json.get_int("totalSize"), int64_t{2});
    CHECK_EQ(r.json.at("records")[0].at("Parent").get_string("Name"), std::string("Noted"));
    CHECK_EQ(r.json.at("records")[1].at("Parent").get_string("Name"), std::string("Noted deal"));
    auto by_parent = f.api.query("SELECT Title FROM Note WHERE ParentId = '" + acct + "'", f.admin_token);
    CHECK_EQ(by_parent.json.get_int("totalSize"), int64_t{1});
    auto bad = f.api.post("/services/data/v1/sobjects/Note", f.obj({{"Title", "Lost"}, {"ParentId", "001000000000000AAA"}}),
                          f.admin_token);
    CHECK_EQ(bad.status, 400);
    // Activities may only point Who at contacts and leads.
    auto wrong = f.api.post("/services/data/v1/sobjects/Task", f.obj({{"Subject", "x"}, {"WhoId", acct}}), f.admin_token);
    CHECK_EQ(wrong.status, 400);
}

TEST(custom_objects, formula_types_and_describe) {
    Fixture f;
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/objects",
                          f.obj({{"name", "Contract__c"}, {"label", "Contract"}, {"pluralLabel", "Contracts"}}), f.admin_token)
                   .status,
               201);
    const char* fields[] = {
        R"J({"name":"Start__c","type":"date","required":true})J",
        R"J({"name":"Months__c","type":"double","precision":3,"scale":0})J",
        R"J({"name":"End__c","type":"formula","returnType":"date","formula":"ADDMONTHS(Start__c, Months__c)"})J",
        R"J({"name":"Label__c","type":"formula","returnType":"string","formula":"Name & \" (\" & TEXT(Months__c) & \" mo)\""})J",
        R"J({"name":"Value__c","type":"currency","defaultValue":"1000"})J",
        R"J({"name":"Monthly__c","type":"formula","returnType":"currency","formula":"Value__c / Months__c"})J",
    };
    for (const char* def : fields) {
        auto r = f.api.post("/services/data/v1/tooling/objects/Contract__c/fields", json::parse(def), f.admin_token);
        if (r.status != 201) testing::record_failure(__FILE__, __LINE__, std::string(def) + " -> " + r.body);
    }
    const std::string id = f.api.create(f.admin_token, "Contract__c",
                                        f.obj({{"Name", "Support"}, {"Start__c", "2026-01-31"}, {"Months__c", 3}}));
    auto rec = f.api.get("/services/data/v1/sobjects/Contract__c/" + id, f.admin_token);
    REQUIRE_EQ(rec.status, 200);
    CHECK_EQ(rec.json.get_string("End__c"), std::string("2026-04-30"));
    CHECK_EQ(rec.json.get_string("Label__c"), std::string("Support (3 mo)"));
    CHECK_EQ(rec.json.get_int("Value__c"), int64_t{1000});
    CHECK_NEAR(rec.json.get_double("Monthly__c"), 333.33, 1e-9);
    CHECK_EQ(f.api.post("/services/data/v1/sobjects/Contract__c", f.obj({{"Name", "No start"}}), f.admin_token).status, 400);

    auto d = f.api.get("/services/data/v1/sobjects/Contract__c/describe", f.admin_token);
    REQUIRE_EQ(d.status, 200);
    CHECK_EQ(d.json.get_bool("custom", false), true);
    CHECK_EQ(d.json.get_string("labelPlural"), std::string("Contracts"));
    bool calculated = false;
    for (const json::Value& fld : d.json.at("fields").as_array()) {
        if (fld.get_string("name") == "End__c") calculated = fld.get_bool("calculated", false) && !fld.get_bool("createable", true);
    }
    CHECK(calculated);
    auto listing = f.api.get("/services/data/v1/tooling/objects", f.admin_token);
    bool listed = false;
    for (const json::Value& o : listing.json.at("records").as_array()) {
        if (o.get_string("name") == "Contract__c") listed = o.get_int("customFieldCount") == 6;
    }
    CHECK(listed);
}

TEST(api_users, listing_and_visibility) {
    Fixture f;
    const std::string a = testing::address("listed");
    const std::string id = f.api.create_user(f.admin_token, a, "Listed", "Marketing User", "Listed1234");
    f.api.patch("/services/data/v1/users/" + id, f.obj({{"title", "Marketer"}, {"department", "Growth"}}), f.admin_token);
    auto all = f.api.get("/services/data/v1/users", f.admin_token);
    REQUIRE_EQ(all.status, 200);
    CHECK_EQ(all.json.get_int("totalSize"), int64_t{2});
    auto one = f.api.get("/services/data/v1/users/" + id, f.admin_token);
    CHECK_EQ(one.json.get_string("department"), std::string("Growth"));
    CHECK_EQ(f.api.get("/services/data/v1/users/005000000000000AAA", f.admin_token).status, 404);
    auto q = f.api.query("SELECT Name, Title, ProfileName FROM User WHERE Title = 'Marketer'", f.admin_token);
    REQUIRE_EQ(q.json.get_int("totalSize"), int64_t{1});
    CHECK_EQ(q.json.at("records")[0].get_string("ProfileName"), std::string("Marketing User"));
    const std::string token = f.api.login(a, "Listed1234");
    CHECK_EQ(f.api.post("/services/data/v1/sobjects/Campaign", f.obj({{"Name", "Spring launch"}}), token).status, 201);
    CHECK_EQ(f.api.get("/services/data/v1/outbox", token).status, 403);
}

TEST(api_webhooks, validation_rules) {
    Fixture f;
    auto bad = f.api.post("/services/data/v1/webhooks", f.obj({{"name", ""}, {"url", "http://hooks.test/"}}), f.admin_token);
    CHECK_EQ(bad.status, 400);
    bad = f.api.post("/services/data/v1/webhooks", f.obj({{"name", "TLS"}, {"url", "https://hooks.test/"}}), f.admin_token);
    CHECK_EQ(bad.status, 400);
    json::Value headers = json::Value::object();
    headers.set("Host", "evil.test");
    json::Value with_host = f.obj({{"name", "Hosty"}, {"url", "http://hooks.test/"}});
    with_host.set("headers", headers);
    CHECK_EQ(f.api.post("/services/data/v1/webhooks", with_host, f.admin_token).status, 400);
    auto ok = f.api.post("/services/data/v1/webhooks", f.obj({{"name", "Plain"}, {"url", "http://hooks.test/in"}}),
                         f.admin_token);
    REQUIRE_EQ(ok.status, 201);
    CHECK_EQ(ok.json.get_bool("active", false), true);
    CHECK_EQ(f.api.post("/services/data/v1/webhooks/0Wh000000000000AAA/test", json::Value::object(), f.admin_token).status,
             404);
}
