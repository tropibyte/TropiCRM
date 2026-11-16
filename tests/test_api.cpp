// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <chrono>
#include <mutex>
#include <thread>

#include "harness.hpp"
#include "http/server.hpp"
#include "util/crypto.hpp"

using namespace tcrm;
using testing::Fixture;

namespace {

std::string error_code(const testing::TestApp::Resp& r) {
    if (r.json.is_array() && !r.json.empty()) return r.json[0].get_string("errorCode");
    return r.json.get_string("error");
}

testing::TestApp::Resp form_login(testing::TestApp& api, const std::string& user, const std::string& password) {
    return api.call("POST", "/services/oauth2/token",
                    "grant_type=password&username=" + url::percent_encode(user) + "&password=" + url::percent_encode(password),
                    {}, {{"Content-Type", "application/x-www-form-urlencoded"}});
}

template <typename Pred>
bool wait_for(Pred pred, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

}  // namespace

TEST(api_auth, password_login_and_errors) {
    Fixture f;
    auto ok = form_login(f.api, f.admin_user, Fixture::kPassword);
    REQUIRE_EQ(ok.status, 200);
    CHECK_EQ(ok.json.get_string("token_type"), std::string("Bearer"));
    CHECK_EQ(ok.json.get_string("org_id"), f.org_id);
    CHECK(!ok.json.get_string("refresh_token").empty());
    CHECK(ok.headers.get("Set-Cookie").find("sid=") == 0);

    auto bad = form_login(f.api, f.admin_user, "wrong-password1");
    CHECK_EQ(bad.status, 400);
    CHECK_EQ(bad.json.get_string("error"), std::string("invalid_grant"));
    CHECK_EQ(form_login(f.api, testing::address("nobody"), "whatever1").status, 400);
    auto missing = f.api.call("POST", "/services/oauth2/token", "grant_type=password", {},
                              {{"Content-Type", "application/x-www-form-urlencoded"}});
    CHECK_EQ(missing.json.get_string("error"), std::string("invalid_request"));
    auto unsupported = f.api.call("POST", "/services/oauth2/token", "grant_type=magic", {},
                                  {{"Content-Type", "application/x-www-form-urlencoded"}});
    CHECK_EQ(unsupported.json.get_string("error"), std::string("unsupported_grant_type"));

    // JSON bodies work too.
    json::Value body = json::Value::object();
    body.set("grant_type", "password");
    body.set("username", f.admin_user);
    body.set("password", Fixture::kPassword);
    CHECK_EQ(f.api.post("/services/oauth2/token", body).status, 200);
}

TEST(api_auth, lockout_after_repeated_failures) {
    Fixture f;
    for (int i = 0; i < 5; ++i) CHECK_EQ(form_login(f.api, f.admin_user, "nope-nope-" + std::to_string(i)).status, 400);
    auto locked = form_login(f.api, f.admin_user, Fixture::kPassword);
    CHECK_EQ(locked.status, 400);
    CHECK(locked.body.find("locked") != std::string::npos);
}

TEST(api_auth, tokens_refresh_revoke_and_cookies) {
    Fixture f;
    auto login = form_login(f.api, f.admin_user, Fixture::kPassword);
    const std::string access = login.json.get_string("access_token");
    const std::string refresh = login.json.get_string("refresh_token");

    CHECK_EQ(f.api.get("/services/oauth2/userinfo", access).status, 200);
    CHECK_EQ(f.api.get("/services/oauth2/userinfo", access).json.get_string("organization_id"), f.org_id);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects", "").status, 401);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects", "v1.garbage.token").status, 401);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects", access + "x").status, 401);
    auto via_cookie = f.api.call("GET", "/services/oauth2/userinfo", {}, {}, {{"Cookie", "theme=dark; sid=" + access}});
    CHECK_EQ(via_cookie.status, 200);

    auto refreshed = f.api.call("POST", "/services/oauth2/token", "grant_type=refresh_token&refresh_token=" + refresh, {},
                                {{"Content-Type", "application/x-www-form-urlencoded"}});
    REQUIRE_EQ(refreshed.status, 200);
    const std::string access2 = refreshed.json.get_string("access_token");
    CHECK(access2 != access);
    CHECK_EQ(f.api.get("/services/oauth2/userinfo", access2).status, 200);

    CHECK_EQ(f.api.call("POST", "/services/oauth2/revoke", "token=" + access, {},
                        {{"Content-Type", "application/x-www-form-urlencoded"}})
                 .status,
             200);
    CHECK_EQ(f.api.get("/services/oauth2/userinfo", access).status, 401);
    CHECK_EQ(f.api.get("/services/oauth2/userinfo", access2).status, 200);
    CHECK_EQ(f.api.call("POST", "/services/oauth2/token", "grant_type=refresh_token&refresh_token=rt_bogus", {},
                        {{"Content-Type", "application/x-www-form-urlencoded"}})
                 .status,
             400);
}

TEST(api_auth, password_reset_flow) {
    Fixture f;
    const std::string user = testing::address("forgetful");
    f.api.create_user(f.admin_token, user, "Forgetful", "Standard User", "Original123");
    CHECK_EQ(f.api.post("/services/auth/password/forgot", f.obj({{"username", user}})).status, 200);
    CHECK_EQ(f.api.post("/services/auth/password/forgot", f.obj({{"username", testing::address("ghost")}})).status, 200);
    const auto mails = f.api.app.outbox().recent(f.org_id, 10);
    REQUIRE(!mails.empty());
    const std::string body = mails[0].body;
    const std::string marker = "password: ";
    const size_t at = body.find(marker);
    REQUIRE(at != std::string::npos);
    const std::string code = body.substr(at + marker.size(), 10);

    auto wrong = f.api.post("/services/auth/password/reset",
                            f.obj({{"username", user}, {"code", "WRONGCODE1"}, {"newPassword", "Brandnew123"}}));
    CHECK_EQ(wrong.status, 400);
    auto weak = f.api.post("/services/auth/password/reset",
                           f.obj({{"username", user}, {"code", code}, {"newPassword", "short"}}));
    CHECK_EQ(weak.status, 400);
    auto ok = f.api.post("/services/auth/password/reset",
                         f.obj({{"username", user}, {"code", code}, {"newPassword", "Brandnew123"}}));
    CHECK_EQ(ok.status, 200);
    CHECK_EQ(form_login(f.api, user, "Original123").status, 400);
    CHECK_EQ(form_login(f.api, user, "Brandnew123").status, 200);
    // Codes work once.
    CHECK_EQ(f.api.post("/services/auth/password/reset",
                        f.obj({{"username", user}, {"code", code}, {"newPassword", "Another1234"}}))
                 .status,
             400);
}

TEST(api_auth, api_keys) {
    Fixture f;
    auto created = f.api.post("/services/data/v1/users/me/apikeys", f.obj({{"label", "ETL"}}), f.admin_token);
    REQUIRE_EQ(created.status, 201);
    const std::string key = created.json.get_string("key");
    CHECK(key.rfind("tck_", 0) == 0);
    auto with_key = f.api.call("GET", "/services/data/v1/sobjects", {}, {}, {{"Authorization", "ApiKey " + key}});
    CHECK_EQ(with_key.status, 200);
    CHECK_EQ(f.api.call("GET", "/services/data/v1/sobjects", {}, {}, {{"Authorization", "ApiKey " + key + "0"}}).status,
             401);
    CHECK_EQ(f.api.call("GET", "/services/data/v1/sobjects", {}, {}, {{"Authorization", "ApiKey nonsense"}}).status, 401);
    auto list = f.api.get("/services/data/v1/users/me/apikeys", f.admin_token);
    REQUIRE_EQ(list.json.at("records").size(), size_t{1});
    CHECK(!list.json.at("records")[0].contains("hash"));
    const std::string key_id = created.json.get_string("id");
    CHECK_EQ(f.api.del("/services/data/v1/users/me/apikeys/" + key_id, f.admin_token).status, 204);
    CHECK_EQ(f.api.call("GET", "/services/data/v1/sobjects", {}, {}, {{"Authorization", "ApiKey " + key}}).status, 401);
}

TEST(api_basics, discovery_and_errors) {
    Fixture f;
    auto versions = f.api.get("/services/data", "");
    REQUIRE_EQ(versions.status, 200);
    CHECK_EQ(versions.json[0].get_string("url"), std::string("/services/data/v1"));
    CHECK_EQ(f.api.get("/healthz", "").json.get_string("status"), std::string("ok"));
    auto resources = f.api.get("/services/data/v1", f.admin_token);
    CHECK_EQ(resources.json.get_string("query"), std::string("/services/data/v1/query"));
    CHECK(!resources.headers.get("Sforce-Limit-Info").empty());

    auto global = f.api.get("/services/data/v1/sobjects", f.admin_token);
    REQUIRE_EQ(global.status, 200);
    CHECK(global.json.at("sobjects").size() >= 12);

    auto describe = f.api.get("/services/data/v1/sobjects/Opportunity/describe", f.admin_token);
    REQUIRE_EQ(describe.status, 200);
    CHECK_EQ(describe.json.get_string("keyPrefix"), std::string("006"));
    bool stage = false;
    for (const json::Value& fld : describe.json.at("fields").as_array()) {
        if (fld.get_string("name") == "StageName") {
            stage = true;
            CHECK_EQ(fld.get_string("type"), std::string("picklist"));
            CHECK(fld.at("picklistValues").size() >= 10);
            CHECK_EQ(fld.get_bool("nillable", true), false);
        }
    }
    CHECK(stage);
    auto account = f.api.get("/services/data/v1/sobjects/Account/describe", f.admin_token);
    bool contacts = false;
    for (const json::Value& rel : account.json.at("childRelationships").as_array()) {
        contacts |= rel.get_string("relationshipName") == "Contacts";
    }
    CHECK(contacts);

    auto missing = f.api.get("/services/data/v1/nope", f.admin_token);
    CHECK_EQ(missing.status, 404);
    CHECK_EQ(error_code(missing), std::string("NOT_FOUND"));
    auto wrong_method = f.api.call("PUT", "/services/data/v1/query", "{}", f.admin_token);
    CHECK_EQ(wrong_method.status, 405);
    CHECK(!wrong_method.headers.get("Allow").empty());
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Nope/describe", f.admin_token).status, 404);

    auto limits = f.api.get("/services/data/v1/limits", f.admin_token);
    REQUIRE_EQ(limits.status, 200);
    CHECK_EQ(limits.json.at("DailyApiRequests").get_int("Max"), int64_t{15000});

    auto recent = f.api.get("/services/data/v1/sobjects/Account", f.admin_token);
    CHECK_EQ(recent.status, 200);
    CHECK(recent.json.at("recentItems").is_array());
}

TEST(api_basics, cors_preflight) {
    Fixture f;
    auto pre = f.api.call("OPTIONS", "/services/data/v1/query", {}, {}, {{"Origin", "http://app.local.test"}});
    CHECK_EQ(pre.status, 204);
    CHECK(!pre.headers.get("Access-Control-Allow-Methods").empty());
}

TEST(api_basics, tenant_isolation) {
    Fixture a;
    const std::string other_admin = testing::address("admin", "othercorp");
    a.api.signup("Other Corp", other_admin, "Another2026x");
    const std::string other = a.api.login(other_admin, "Another2026x");
    const std::string acct = a.api.create(a.admin_token, "Account", a.obj({{"Name", "Secret"}}));
    CHECK_EQ(a.api.get("/services/data/v1/sobjects/Account/" + acct, other).status, 404);
    CHECK_EQ(a.api.query("SELECT Id FROM Account", other).json.get_int("totalSize"), int64_t{0});
    CHECK_EQ(a.api.patch("/services/data/v1/sobjects/Account/" + acct, a.obj({{"Name", "Mine"}}), other).status, 404);
    CHECK_EQ(a.api.get("/services/data/v1/search?q=secret", other).json.at("searchRecords").size(), size_t{0});
    // Duplicate usernames are rejected at signup.
    json::Value dup = json::Value::object();
    dup.set("company", "Dupe");
    dup.set("username", a.admin_user);
    dup.set("password", "Whatever2026");
    CHECK_EQ(a.api.post("/services/signup", dup).status, 400);
}

TEST(api_basics, search) {
    Fixture f;
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Globex Corporation"}}));
    f.api.create(f.admin_token, "Contact", f.obj({{"FirstName", "Hank"}, {"LastName", "Scorpio"}}));
    f.api.create(f.admin_token, "Lead", f.obj({{"LastName", "Globetrotter"}, {"Company", "Travel Inc"}}));
    auto r = f.api.get("/services/data/v1/search?q=glob", f.admin_token);
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.at("searchRecords").size(), size_t{2});
    r = f.api.get("/services/data/v1/search?q=glob&objects=Account", f.admin_token);
    REQUIRE_EQ(r.json.at("searchRecords").size(), size_t{1});
    CHECK_EQ(r.json.at("searchRecords")[0].get_string("Name"), std::string("Globex Corporation"));
    r = f.api.get("/services/data/v1/search?q=hank+scorpio", f.admin_token);
    REQUIRE_EQ(r.json.at("searchRecords").size(), size_t{1});
    CHECK_EQ(f.api.get("/services/data/v1/search?q=x", f.admin_token).status, 400);
    CHECK_EQ(f.api.get("/services/data/v1/search?q=glob&objects=Nope", f.admin_token).status, 400);
}

TEST(api_composite, references_between_subrequests) {
    Fixture f;
    json::Value body = json::parse(R"({"allOrNone": false, "compositeRequest": [
        {"method":"POST","url":"/services/data/v1/sobjects/Account","referenceId":"acct","body":{"Name":"Composite Co"}},
        {"method":"POST","url":"/services/data/v1/sobjects/Contact","referenceId":"ct",
         "body":{"LastName":"Ref","AccountId":"@{acct.id}"}},
        {"method":"GET","url":"/services/data/v1/sobjects/Account/@{acct.id}?fields=Name","referenceId":"read"},
        {"method":"GET","url":"/services/data/v1/query?q=SELECT+Id+FROM+Contact","referenceId":"q"}
    ]})");
    auto r = f.api.post("/services/data/v1/composite", body, f.admin_token);
    REQUIRE_EQ(r.status, 200);
    const json::Value& responses = r.json.at("compositeResponse");
    REQUIRE_EQ(responses.size(), size_t{4});
    CHECK_EQ(responses[0].get_int("httpStatusCode"), int64_t{201});
    CHECK_EQ(responses[1].get_int("httpStatusCode"), int64_t{201});
    CHECK_EQ(responses[2].at("body").get_string("Name"), std::string("Composite Co"));
    CHECK_EQ(responses[3].at("body").get_int("totalSize"), int64_t{1});

    json::Value failing = json::parse(R"({"allOrNone": true, "compositeRequest": [
        {"method":"POST","url":"/services/data/v1/sobjects/Account","referenceId":"bad","body":{"Phone":"1"}},
        {"method":"POST","url":"/services/data/v1/sobjects/Account","referenceId":"skipped","body":{"Name":"Never"}}
    ]})");
    r = f.api.post("/services/data/v1/composite", failing, f.admin_token);
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.at("compositeResponse")[0].get_int("httpStatusCode"), int64_t{400});
    CHECK_EQ(r.json.at("compositeResponse")[1].at("body")[0].get_string("errorCode"), std::string("PROCESSING_HALTED"));
    CHECK_EQ(f.api.query("SELECT Id FROM Account WHERE Name = 'Never'", f.admin_token).json.get_int("totalSize"), int64_t{0});

    json::Value bad_ref = json::parse(R"({"compositeRequest": [
        {"method":"GET","url":"/services/data/v1/sobjects/Account/@{nothing.id}","referenceId":"x"}]})");
    r = f.api.post("/services/data/v1/composite", bad_ref, f.admin_token);
    CHECK_EQ(r.status, 400);
    CHECK_EQ(f.api.post("/services/data/v1/composite", json::parse(R"({"compositeRequest": []})"), f.admin_token).status, 400);
}

TEST(api_users, manage_users) {
    Fixture f;
    const std::string name = testing::address("newhire");
    const std::string id = f.api.create_user(f.admin_token, name, "Hire", "Standard User", "Welcome123");
    const std::string token = f.api.login(name, "Welcome123");
    auto me = f.api.get("/services/data/v1/users/me", token);
    REQUIRE_EQ(me.status, 200);
    CHECK_EQ(me.json.get_string("id"), id);
    CHECK_EQ(me.json.get_string("username"), name);

    CHECK_EQ(f.api.patch("/services/data/v1/users/me", f.obj({{"title", "Analyst"}, {"timezone", "Europe/Paris"}}), token)
                 .status,
             204);
    CHECK_EQ(f.api.get("/services/data/v1/users/me", token).json.get_string("timezone"), std::string("Europe/Paris"));
    CHECK_EQ(f.api.patch("/services/data/v1/users/me", f.obj({{"profileId", "00e000000000001AAA"}}), token).status, 403);
    CHECK_EQ(f.api.patch("/services/data/v1/users/me", f.obj({{"timezone", "Nowhere/Land"}}), token).status, 400);
    CHECK_EQ(f.api.post("/services/data/v1/users", f.obj({{"username", testing::address("x")}, {"lastName", "X"}}), token)
                 .status,
             403);

    // Password change needs the current password.
    CHECK_EQ(f.api.post("/services/data/v1/users/me/password",
                        f.obj({{"currentPassword", "wrong"}, {"newPassword", "Changed1234"}}), token)
                 .status,
             400);
    CHECK_EQ(f.api.post("/services/data/v1/users/me/password",
                        f.obj({{"currentPassword", "Welcome123"}, {"newPassword", "Changed1234"}}), token)
                 .status,
             204);
    CHECK_EQ(form_login(f.api, name, "Changed1234").status, 200);

    // Admins can deactivate users; inactive users can't log in.
    CHECK_EQ(f.api.patch("/services/data/v1/users/" + id, f.obj({{"active", false}}), f.admin_token).status, 204);
    CHECK_EQ(form_login(f.api, name, "Changed1234").status, 400);
    CHECK_EQ(f.api.patch("/services/data/v1/users/me", f.obj({{"active", false}}), f.admin_token).status, 400);
    CHECK_EQ(f.api.post("/services/data/v1/users", f.obj({{"username", "no-at-sign"}, {"lastName", "X"}}), f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.post("/services/data/v1/users", f.obj({{"username", name}, {"lastName", "Again"}}), f.admin_token)
                 .status,
             400);
}

TEST(api_reports, tabular_and_summary) {
    Fixture f;
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Beta"}, {"Industry", "Technology"}}));
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Alpha"}, {"Industry", "Technology"}}));
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Gamma"}, {"Industry", "Retail"}}));
    json::Value tabular = json::parse(R"({"name":"Tech accounts","object":"Account","columns":["Name","Industry"],
        "filter":"Industry = 'Technology'","sortBy":"Name"})");
    auto created = f.api.post("/services/data/v1/analytics/reports", tabular, f.admin_token);
    REQUIRE_EQ(created.status, 201);
    const std::string id = created.json.get_string("id");
    auto run = f.api.post("/services/data/v1/analytics/reports/" + id + "/run", json::Value::object(), f.admin_token);
    REQUIRE_EQ(run.status, 200);
    REQUIRE_EQ(run.json.at("rows").size(), size_t{2});
    CHECK_EQ(run.json.at("rows")[0][0].as_string(), std::string("Alpha"));
    CHECK_EQ(run.json.at("reportMetadata").get_string("reportFormat"), std::string("TABULAR"));

    json::Value summary = json::parse(R"J({"name":"By industry","object":"Account","groupings":["Industry"],
        "aggregate":"COUNT(Id)","sortBy":"Industry"})J");
    created = f.api.post("/services/data/v1/analytics/reports", summary, f.admin_token);
    REQUIRE_EQ(created.status, 201);
    const std::string sid = created.json.get_string("id");
    run = f.api.post("/services/data/v1/analytics/reports/" + sid + "/run", json::Value::object(), f.admin_token);
    REQUIRE_EQ(run.status, 200);
    REQUIRE_EQ(run.json.at("rows").size(), size_t{2});
    CHECK_EQ(run.json.at("rows")[0][0].as_string(), std::string("Retail"));
    CHECK_EQ(run.json.at("rows")[1][1].as_int(), int64_t{2});
    CHECK_NEAR(run.json.get_double("grandTotal"), 3.0, 1e-9);

    auto csv_run = f.api.call("POST", "/services/data/v1/analytics/reports/" + sid + "/run?format=csv", "{}", f.admin_token);
    REQUIRE_EQ(csv_run.status, 200);
    CHECK_EQ(csv_run.headers.get("Content-Type"), std::string("text/csv"));
    CHECK_EQ(csv_run.body, std::string("Industry,COUNT(Id)\r\nRetail,1\r\nTechnology,2\r\n"));

    CHECK_EQ(f.api.get("/services/data/v1/analytics/reports", f.admin_token).json.at("records").size(), size_t{2});
    auto bad = f.api.post("/services/data/v1/analytics/reports",
                          json::parse(R"({"name":"Broken","object":"Account","columns":["Name"],"filter":"Name ="})"),
                          f.admin_token);
    CHECK_EQ(bad.status, 400);
    CHECK_EQ(f.api.del("/services/data/v1/analytics/reports/" + id, f.admin_token).status, 204);
    CHECK_EQ(f.api.get("/services/data/v1/analytics/reports/" + id, f.admin_token).status, 404);
}

TEST(api_bulk, insert_job) {
    Fixture f;
    auto job = f.api.post("/services/data/v1/jobs/ingest", f.obj({{"object", "Account"}, {"operation", "insert"}}),
                          f.admin_token);
    REQUIRE_EQ(job.status, 201);
    const std::string id = job.json.get_string("id");
    CHECK_EQ(job.json.get_string("state"), std::string("Open"));
    const std::string data = "Name,Industry,AnnualRevenue\nBulk One,Technology,1000\n\"Bulk, Two\",Retail,\n"
                             "Bulk Three,NotAnIndustry,5\n,Technology,1\n";
    CHECK_EQ(f.api.call("PUT", "/services/data/v1/jobs/ingest/" + id + "/batches", data, f.admin_token,
                        {{"Content-Type", "text/csv"}})
                 .status,
             201);
    auto closed = f.api.patch("/services/data/v1/jobs/ingest/" + id, f.obj({{"state", "UploadComplete"}}), f.admin_token);
    REQUIRE_EQ(closed.status, 200);
    REQUIRE(wait_for([&] {
        const std::string state = f.api.get("/services/data/v1/jobs/ingest/" + id, f.admin_token).json.get_string("state");
        return state == "JobComplete" || state == "Failed";
    }));
    auto info = f.api.get("/services/data/v1/jobs/ingest/" + id, f.admin_token);
    CHECK_EQ(info.json.get_string("state"), std::string("JobComplete"));
    CHECK_EQ(info.json.get_int("numberRecordsProcessed"), int64_t{4});
    CHECK_EQ(info.json.get_int("numberRecordsFailed"), int64_t{1});
    CHECK_EQ(f.api.query("SELECT Id FROM Account", f.admin_token).json.get_int("totalSize"), int64_t{3});
    auto failed = f.api.get("/services/data/v1/jobs/ingest/" + id + "/failedResults", f.admin_token);
    CHECK(failed.body.find("REQUIRED_FIELD_MISSING") != std::string::npos);
    auto succeeded = f.api.get("/services/data/v1/jobs/ingest/" + id + "/successfulResults", f.admin_token);
    CHECK(succeeded.body.find("\"Bulk, Two\"") != std::string::npos);
    CHECK_EQ(f.api.get("/services/data/v1/jobs/ingest", f.admin_token).json.at("records").size(), size_t{1});
    CHECK_EQ(f.api.del("/services/data/v1/jobs/ingest/" + id, f.admin_token).status, 204);
    CHECK_EQ(f.api.get("/services/data/v1/jobs/ingest/" + id, f.admin_token).status, 404);

    auto bad = f.api.post("/services/data/v1/jobs/ingest", f.obj({{"object", "Account"}, {"operation", "merge"}}), f.admin_token);
    CHECK_EQ(bad.status, 400);
}

TEST(api_bulk, unknown_column_fails_job) {
    Fixture f;
    auto job = f.api.post("/services/data/v1/jobs/ingest", f.obj({{"object", "Contact"}, {"operation", "insert"}}),
                          f.admin_token);
    const std::string id = job.json.get_string("id");
    f.api.call("PUT", "/services/data/v1/jobs/ingest/" + id + "/batches", "LastName,Shoe_Size__c\nX,9\n", f.admin_token);
    f.api.patch("/services/data/v1/jobs/ingest/" + id, f.obj({{"state", "UploadComplete"}}), f.admin_token);
    REQUIRE(wait_for([&] {
        return f.api.get("/services/data/v1/jobs/ingest/" + id, f.admin_token).json.get_string("state") == "Failed";
    }));
    CHECK(f.api.get("/services/data/v1/jobs/ingest/" + id, f.admin_token).json.get_string("errorMessage").find("Shoe_Size__c") !=
          std::string::npos);
}

TEST(api_files, attachments_round_trip) {
    Fixture f;
    const std::string acct = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Has Files"}}));
    auto up = f.api.call("POST", "/services/data/v1/sobjects/Account/" + acct + "/attachments", "hello, file", f.admin_token,
                         {{"X-File-Name", "notes.txt"}, {"Content-Type", "application/octet-stream"}});
    REQUIRE_EQ(up.status, 201);
    const std::string att = up.json.get_string("id");
    auto meta = f.api.get("/services/data/v1/sobjects/Attachment/" + att, f.admin_token);
    REQUIRE_EQ(meta.status, 200);
    CHECK_EQ(meta.json.get_string("Name"), std::string("notes.txt"));
    CHECK_EQ(meta.json.get_string("ContentType"), std::string("text/plain"));
    CHECK_EQ(meta.json.get_int("BodyLength"), int64_t{11});
    CHECK_EQ(meta.json.get_string("ContentHash"), crypto::to_hex(crypto::sha256("hello, file")));
    CHECK(!meta.json.contains("_StorageKey"));
    auto body = f.api.get("/services/data/v1/sobjects/Attachment/" + att + "/body", f.admin_token);
    REQUIRE_EQ(body.status, 200);
    CHECK_EQ(body.body, std::string("hello, file"));
    CHECK(body.headers.get("Content-Disposition").find("notes.txt") != std::string::npos);
    auto q = f.api.query("SELECT Name, Parent.Name FROM Attachment", f.admin_token);
    CHECK_EQ(q.json.at("records")[0].at("Parent").get_string("Name"), std::string("Has Files"));
    CHECK_EQ(f.api.call("POST", "/services/data/v1/sobjects/Account/" + acct + "/attachments", "x", f.admin_token).status, 400);
}

TEST(api_webhooks, delivery_with_signature) {
    std::mutex mu;
    std::vector<http::Request> received;
    http::ServerOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.workers = 2;
    http::Server receiver(opts, [&](http::Request& req) {
        std::lock_guard<std::mutex> lock(mu);
        received.push_back(req);
        http::Response r;
        r.status = 200;
        r.body = "thanks";
        r.close_connection = true;
        return r;
    });
    std::string err;
    REQUIRE(receiver.start(&err));

    Fixture f;
    json::Value hook = json::Value::object();
    hook.set("name", "CI hook");
    hook.set("url", "http://127.0.0.1:" + std::to_string(receiver.port()) + "/hook");
    auto created = f.api.post("/services/data/v1/webhooks", hook, f.admin_token);
    REQUIRE_EQ(created.status, 201);
    const std::string hook_id = created.json.get_string("id");
    const std::string secret = created.json.get_string("secret");
    CHECK(!secret.empty());
    CHECK(!f.api.get("/services/data/v1/webhooks", f.admin_token).json.at("records")[0].contains("secret"));

    // A workflow rule that posts new high-value opportunities.
    json::Value rule = json::Value::object();
    rule.set("name", "Big deals");
    rule.set("object", "Opportunity");
    rule.set("criteria", "Amount >= 100000");
    rule.set("onUpdate", false);
    json::Value actions = json::Value::array();
    json::Value action = json::Value::object();
    action.set("type", "Webhook");
    action.set("webhookId", hook_id);
    actions.push_back(action);
    rule.set("actions", actions);
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/workflowRules", rule, f.admin_token).status, 201);
    f.api.create(f.admin_token, "Opportunity",
                 f.obj({{"Name", "Small"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}, {"Amount", 10}}));
    f.api.create(f.admin_token, "Opportunity",
                 f.obj({{"Name", "Whale"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}, {"Amount", 250000}}));

    REQUIRE(wait_for([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !received.empty();
    }));
    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE_EQ(received.size(), size_t{1});
        const http::Request& req = received[0];
        CHECK_EQ(req.path, std::string("/hook"));
        CHECK_EQ(req.headers.get("X-TropiCRM-Signature"), services::WebhookService::signature(secret, req.body));
        const json::Value payload = json::parse(req.body);
        CHECK_EQ(payload.get_string("orgId"), f.org_id);
        CHECK_EQ(payload.at("data").at("record").get_string("Name"), std::string("Whale"));
        CHECK_EQ(payload.at("data").get_string("event"), std::string("created"));
    }
    REQUIRE(wait_for([&] {
        return f.api.get("/services/data/v1/webhooks/deliveries", f.admin_token).json.at("records").size() == 1;
    }));
    auto deliveries = f.api.get("/services/data/v1/webhooks/deliveries", f.admin_token);
    CHECK_EQ(deliveries.json.at("records")[0].get_int("status"), int64_t{200});

    CHECK_EQ(f.api.post("/services/data/v1/webhooks", f.obj({{"name", "x"}, {"url", "ftp://nope.test/"}}), f.admin_token).status,
             400);
    CHECK_EQ(f.api.del("/services/data/v1/webhooks/" + hook_id, f.admin_token).status, 204);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    receiver.stop();
}

TEST(api_admin, platform_key_required) {
    Config config = testing::test_config();
    config.set("platform.admin_key_sha256", crypto::to_hex(crypto::sha256("platform-operator-key-for-tests")));
    testing::TestApp api(config);
    const std::string admin = testing::address("root", "tenant");
    const std::string org_id = api.signup("Tenant", admin, "Tenant2026x");
    const std::string token = api.login(admin, "Tenant2026x");

    CHECK_EQ(api.get("/services/admin/v1/orgs", "").status, 403);
    CHECK_EQ(api.get("/services/admin/v1/orgs", token).status, 403);
    auto listed = api.call("GET", "/services/admin/v1/orgs", {}, {}, {{"X-Platform-Admin-Key", "platform-operator-key-for-tests"}});
    REQUIRE_EQ(listed.status, 200);
    CHECK_EQ(listed.json.at("records").size(), size_t{1});
    CHECK_EQ(listed.json.at("records")[0].get_string("id"), org_id);

    // Disabling an org locks its users out.
    auto disabled = api.call("PATCH", "/services/admin/v1/orgs/" + org_id, R"({"active": false})", {},
                             {{"X-Platform-Admin-Key", "platform-operator-key-for-tests"}});
    CHECK_EQ(disabled.status, 200);
    CHECK_EQ(api.get("/services/data/v1/sobjects", token).status, 401);
    CHECK_EQ(form_login(api, admin, "Tenant2026x").status, 400);
    api.call("PATCH", "/services/admin/v1/orgs/" + org_id, R"({"active": true})", {},
             {{"X-Platform-Admin-Key", "platform-operator-key-for-tests"}});
    CHECK_EQ(api.get("/services/data/v1/sobjects", token).status, 200);

    // Org admins can use support tooling for their own org.
    auto users = api.get("/services/admin/v1/orgs/" + org_id + "/users", token);
    REQUIRE_EQ(users.status, 200);
    CHECK_EQ(users.json.at("records").size(), size_t{1});
}

TEST(api_tooling, profiles_roles_and_settings) {
    Fixture f;
    auto profiles = f.api.get("/services/data/v1/tooling/profiles", f.admin_token);
    REQUIRE_EQ(profiles.status, 200);
    CHECK_EQ(profiles.json.at("records").size(), size_t{5});
    auto cloned = f.api.post("/services/data/v1/tooling/profiles", f.obj({{"name", "Contractor"}, {"cloneFrom", "Read Only"}}),
                             f.admin_token);
    REQUIRE_EQ(cloned.status, 201);
    const std::string pid = cloned.json.get_string("id");
    auto patched = f.api.patch("/services/data/v1/tooling/profiles/" + pid,
                               json::parse(R"({"objectPermissions": {"Case": {"read": true, "create": true}}})"),
                               f.admin_token);
    REQUIRE_EQ(patched.status, 200);
    CHECK_EQ(patched.json.at("objectPermissions").at("Case").get_bool("create"), true);
    CHECK_EQ(patched.json.at("objectPermissions").at("Account").get_bool("read"), true);
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/profiles/00e000000000001AAA",
                         json::parse(R"({"systemPermissions": {"ModifyAllData": false}})"), f.admin_token)
                 .status,
             400);

    auto role = f.api.post("/services/data/v1/tooling/roles", f.obj({{"name", "East"}}), f.admin_token);
    REQUIRE_EQ(role.status, 201);
    const std::string rid = role.json.get_string("id");
    CHECK_EQ(f.api.post("/services/data/v1/tooling/roles", f.obj({{"name", "Bad"}, {"parentRoleId", "00E000000000999AAA"}}),
                        f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/roles/" + rid, f.obj({{"parentRoleId", rid}}), f.admin_token).status, 400);
    CHECK_EQ(f.api.del("/services/data/v1/tooling/roles/" + rid, f.admin_token).status, 204);

    auto settings = f.api.patch("/services/data/v1/tooling/settings", f.obj({{"timezone", "Asia/Tokyo"}}), f.admin_token);
    REQUIRE_EQ(settings.status, 200);
    CHECK_EQ(settings.json.get_string("timezone"), std::string("Asia/Tokyo"));
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/settings", f.obj({{"timezone", "Moon/Base"}}), f.admin_token).status, 400);
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/settings", f.obj({{"currencyRates", f.obj({{"EUR", -1}})}}), f.admin_token)
                 .status,
             400);

    // Standard users can't change setup.
    const std::string user = testing::address("std");
    f.api.create_user(f.admin_token, user, "Std", "Standard User", "Standard123");
    const std::string token = f.api.login(user, "Standard123");
    CHECK_EQ(f.api.post("/services/data/v1/tooling/objects", f.obj({{"name", "Nope__c"}}), token).status, 403);
    CHECK_EQ(f.api.get("/services/data/v1/tooling/settings", token).status, 403);
    CHECK_EQ(f.api.get("/services/data/v1/webhooks", token).status, 403);
}
