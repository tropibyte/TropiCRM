// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "api/rate_limiter.hpp"
#include "harness.hpp"

using namespace tcrm;
using testing::Fixture;

TEST(rate_limit, token_bucket) {
    api::RateLimiter limiter(2, 60);  // burst of 2, one token per second
    int64_t retry = 0;
    CHECK(limiter.allow("a", 1000));
    CHECK(limiter.allow("a", 1000));
    CHECK(!limiter.allow("a", 1000, &retry));
    CHECK(retry > 900 && retry <= 1000);
    CHECK(limiter.allow("b", 1000));  // independent key
    CHECK(!limiter.allow("a", 1500));
    CHECK(limiter.allow("a", 2100));
    CHECK_EQ(limiter.tracked_keys(), size_t{2});
    limiter.prune(100000);
    CHECK_EQ(limiter.tracked_keys(), size_t{0});
}

TEST(rate_limit, client_address) {
    http::Request req;
    req.remote_addr = "10.1.2.3:51515";
    CHECK_EQ(api::client_ip(req), std::string("10.1.2.3"));
    req.headers.set("X-Forwarded-For", " 203.0.113.9 , 10.0.0.1");
    CHECK_EQ(api::client_ip(req), std::string("203.0.113.9"));
}

TEST(rate_limit, login_endpoint_is_throttled) {
    Config config = testing::test_config();
    config.set("limits.public_requests_per_minute", "6");  // burst of 3
    testing::TestApp api(config);
    const std::string user = testing::address("throttle");
    api.signup("Throttle Co", user, "Throttle2026");
    const std::string body = "grant_type=password&username=" + url::percent_encode(user) + "&password=wrong-pass1";
    const std::vector<std::pair<std::string, std::string>> form = {{"Content-Type", "application/x-www-form-urlencoded"}};
    CHECK_EQ(api.call("POST", "/services/oauth2/token", body, {}, form).status, 400);
    CHECK_EQ(api.call("POST", "/services/oauth2/token", body, {}, form).status, 400);
    auto limited = api.call("POST", "/services/oauth2/token", body, {}, form);
    CHECK_EQ(limited.status, 429);
    CHECK(!limited.headers.get("Retry-After").empty());
    // Authenticated API traffic isn't affected by the public limiter.
    CHECK_EQ(api.get("/healthz", "").status, 200);
}

TEST(forecasts, roll_up_through_the_role_hierarchy) {
    Fixture f;
    auto vp_role = f.api.post("/services/data/v1/tooling/roles", f.obj({{"name", "VP"}}), f.admin_token);
    auto rep_role = f.api.post("/services/data/v1/tooling/roles",
                               f.obj({{"name", "AE"}, {"parentRoleId", vp_role.json.get_string("id")}}), f.admin_token);
    const std::string vp_name = testing::address("fvp");
    const std::string rep_name = testing::address("fae");
    f.api.create_user(f.admin_token, vp_name, "Vp", "Standard User", "Forecast123", vp_role.json.get_string("id"));
    f.api.create_user(f.admin_token, rep_name, "Ae", "Standard User", "Forecast123", rep_role.json.get_string("id"));
    const std::string vp = f.api.login(vp_name, "Forecast123");
    const std::string rep = f.api.login(rep_name, "Forecast123");
    auto opp = [&](const std::string& token, const std::string& name, const std::string& stage, double amount,
                   const std::string& close) {
        f.api.create(token, "Opportunity",
                     f.obj({{"Name", name}, {"StageName", stage}, {"Amount", amount}, {"CloseDate", close}}));
    };
    opp(rep, "Rep commit", "Negotiation/Review", 1000, "2026-05-10");
    opp(rep, "Rep won", "Closed Won", 2000, "2026-05-20");
    opp(vp, "VP pipeline", "Prospecting", 500, "2026-06-01");
    opp(vp, "Out of range", "Prospecting", 999, "2027-03-01");

    auto r = f.api.get("/services/data/v1/forecasts?start=2026-01-01&end=2027-01-01", vp);
    REQUIRE_EQ(r.status, 200);
    const json::Value& total = r.json.at("total");
    CHECK_EQ(total.get_int("opportunities"), int64_t{3});
    CHECK_NEAR(total.get_double("commit"), 1000, 1e-9);
    CHECK_NEAR(total.get_double("closed"), 2000, 1e-9);
    CHECK_NEAR(total.get_double("pipeline"), 500, 1e-9);
    CHECK_NEAR(total.get_double("commitTotal"), 3000, 1e-9);
    REQUIRE_EQ(r.json.at("periods").size(), size_t{2});
    CHECK_EQ(r.json.at("periods")[0].get_string("period"), std::string("2026-05"));

    auto mine = f.api.get("/services/data/v1/forecasts?start=2026-01-01&end=2027-01-01", rep);
    REQUIRE_EQ(mine.status, 200);
    CHECK_EQ(mine.json.at("total").get_int("opportunities"), int64_t{2});
    CHECK_EQ(f.api.get("/services/data/v1/forecasts?start=2026-02-01&end=2026-01-01", vp).status, 400);
    CHECK_EQ(f.api.get("/services/data/v1/forecasts?start=garbage", vp).status, 400);
    CHECK_EQ(f.api.get("/services/data/v1/forecasts", vp).status, 200);
}

TEST(dashboards, run_components) {
    Fixture f;
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "D1"}, {"Industry", "Energy"}}));
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "D2"}, {"Industry", "Energy"}}));
    auto report = f.api.post("/services/data/v1/analytics/reports",
                             json::parse(R"J({"name":"By industry","object":"Account","groupings":["Industry"],"aggregate":"COUNT(Id)"})J"),
                             f.admin_token);
    REQUIRE_EQ(report.status, 201);
    json::Value dash = json::Value::object();
    dash.set("name", "Sales overview");
    json::Value comps = json::Value::array();
    json::Value comp = json::Value::object();
    comp.set("title", "Accounts by industry");
    comp.set("reportId", report.json.get_string("id"));
    comp.set("display", "bar");
    comps.push_back(comp);
    dash.set("components", comps);
    auto created = f.api.post("/services/data/v1/analytics/dashboards", dash, f.admin_token);
    REQUIRE_EQ(created.status, 201);
    const std::string id = created.json.get_string("id");
    auto run = f.api.post("/services/data/v1/analytics/dashboards/" + id + "/run", json::Value::object(), f.admin_token);
    REQUIRE_EQ(run.status, 200);
    const json::Value& result = run.json.at("components")[0].at("result");
    REQUIRE_EQ(result.at("rows").size(), size_t{1});
    CHECK_EQ(result.at("rows")[0][1].as_int(), int64_t{2});
    CHECK_EQ(f.api.get("/services/data/v1/analytics/dashboards", f.admin_token).json.at("records").size(), size_t{1});

    comp.set("display", "hologram");
    json::Value bad = dash;
    json::Value bad_comps = json::Value::array();
    bad_comps.push_back(comp);
    bad.set("components", bad_comps);
    CHECK_EQ(f.api.post("/services/data/v1/analytics/dashboards", bad, f.admin_token).status, 400);
    comp.set("display", "table");
    comp.set("reportId", "00O000000000000AAA");
    bad_comps = json::Value::array();
    bad_comps.push_back(comp);
    bad.set("components", bad_comps);
    CHECK_EQ(f.api.post("/services/data/v1/analytics/dashboards", bad, f.admin_token).status, 400);

    // Deleting the report leaves the dashboard with an error component.
    REQUIRE_EQ(f.api.del("/services/data/v1/analytics/reports/" + report.json.get_string("id"), f.admin_token).status, 204);
    run = f.api.post("/services/data/v1/analytics/dashboards/" + id + "/run", json::Value::object(), f.admin_token);
    CHECK(run.json.at("components")[0].contains("error"));
    CHECK_EQ(f.api.del("/services/data/v1/analytics/dashboards/" + id, f.admin_token).status, 204);
}

TEST(api_basics, more_query_shapes) {
    Fixture f;
    const std::string acct = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Shape"}}));
    for (int i = 0; i < 5; ++i) {
        f.api.create(f.admin_token, "Case",
                     f.obj({{"Subject", "Case " + std::to_string(i)}, {"AccountId", acct},
                            {"Priority", i % 2 == 0 ? "High" : "Low"}}));
    }
    auto r = f.api.query("SELECT Priority, COUNT(Id) FROM Case GROUP BY Priority HAVING COUNT(Id) >= 3", f.admin_token);
    REQUIRE_EQ(r.status, 200);
    REQUIRE_EQ(r.json.get_int("totalSize"), int64_t{1});
    CHECK_EQ(r.json.at("records")[0].get_string("Priority"), std::string("High"));
    r = f.api.query("SELECT CaseNumber, Account.Name FROM Case WHERE Priority = 'Low' ORDER BY CaseNumber DESC", f.admin_token);
    REQUIRE_EQ(r.json.get_int("totalSize"), int64_t{2});
    CHECK_EQ(r.json.at("records")[0].get_string("CaseNumber"), std::string("00000004"));
    r = f.api.query("SELECT Name, (SELECT Subject FROM Cases WHERE Priority = 'High' ORDER BY Subject DESC LIMIT 2) FROM Account",
                    f.admin_token);
    REQUIRE_EQ(r.status, 200);
    const json::Value& cases = r.json.at("records")[0].at("Cases");
    CHECK_EQ(cases.get_int("totalSize"), int64_t{2});
    CHECK_EQ(cases.at("records")[0].get_string("Subject"), std::string("Case 4"));
    r = f.api.query("SELECT Id FROM Case WHERE CaseNumber IN ('00000001', '00000003') OR Subject LIKE '%4'", f.admin_token);
    CHECK_EQ(r.json.get_int("totalSize"), int64_t{3});
    r = f.api.query("SELECT COUNT() FROM Case WHERE Account.Name = 'Shape' AND IsClosed = false", f.admin_token);
    CHECK_EQ(r.json.get_int("totalSize"), int64_t{5});
}

TEST(setup, edit_fields_through_tooling) {
    Fixture f;
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/objects/Account/fields",
                          json::parse(R"({"name":"Region__c","type":"picklist","picklistValues":["East","West"],
                                         "restrictedPicklist":true})"),
                          f.admin_token)
                   .status,
               201);
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/objects/Account/fields",
                          json::parse(R"({"name":"Code__c","type":"string","length":20})"), f.admin_token)
                   .status,
               201);
    auto r = f.api.patch("/services/data/v1/tooling/objects/Account/fields/Region__c",
                         json::parse(R"({"label":"Sales Region","picklistValues":["East","West","Central"]})"),
                         f.admin_token);
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.get_string("label"), std::string("Sales Region"));
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Mid"}, {"Region__c", "Central"}}));

    r = f.api.patch("/services/data/v1/tooling/objects/Account/fields/Code__c", json::parse(R"({"unique":true,"length":10})"),
                    f.admin_token);
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.get_bool("unique", false), true);
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Coded"}, {"Code__c", "C-1"}}));
    auto dup = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", "Coded 2"}, {"Code__c", "C-1"}}), f.admin_token);
    CHECK_EQ(dup.status, 400);
    auto too_long = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", "Long"}, {"Code__c", "0123456789A"}}),
                               f.admin_token);
    CHECK_EQ(too_long.status, 400);

    CHECK_EQ(f.api.patch("/services/data/v1/tooling/objects/Account/fields/Name", json::parse(R"({"required":false})"),
                         f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/objects/Account/fields/Name", json::parse(R"({"label":"Company"})"),
                         f.admin_token)
                 .status,
             200);
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/objects/Account/fields/CreatedDate", json::parse(R"({"label":"x"})"),
                         f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/objects/Account/fields/Code__c", json::parse(R"({"length":0})"),
                         f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/objects/Account/fields/Code__c", json::parse(R"({"type":"double"})"),
                         f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.patch("/services/data/v1/tooling/objects/Account/fields/Nope__c", json::parse(R"({"label":"x"})"),
                         f.admin_token)
                 .status,
             404);
}

TEST(setup, recently_viewed) {
    Fixture f;
    const std::string a = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Viewed first"}}));
    const std::string b = f.api.create(f.admin_token, "Contact", f.obj({{"LastName", "Viewed second"}}));
    f.api.get("/services/data/v1/sobjects/Account/" + a, f.admin_token);
    f.api.get("/services/data/v1/sobjects/Contact/" + b, f.admin_token);
    f.api.get("/services/data/v1/sobjects/Account/" + a, f.admin_token);
    auto r = f.api.get("/services/data/v1/recent", f.admin_token);
    REQUIRE_EQ(r.status, 200);
    REQUIRE_EQ(r.json.size(), size_t{2});
    CHECK_EQ(r.json[0].get_string("Id"), a);
    CHECK_EQ(r.json[1].get_string("Name"), std::string("Viewed second"));
    f.api.del("/services/data/v1/sobjects/Account/" + a, f.admin_token);
    CHECK_EQ(f.api.get("/services/data/v1/recent", f.admin_token).json.size(), size_t{1});
}

TEST(setup, health_check) {
    Fixture f;
    auto r = f.api.get("/services/data/v1/tooling/healthCheck", f.admin_token);
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.at("checks").size(), size_t{9});
    CHECK(r.json.get_int("score") > 0);
    bool hashing = false;
    for (const json::Value& c : r.json.at("checks").as_array()) {
        if (c.get_string("id") == "password_hashing") hashing = c.get_string("status") == "Fail";
    }
    CHECK(hashing);  // the test config uses 1000 iterations
    const std::string user = testing::address("hc");
    f.api.create_user(f.admin_token, user, "Hc", "Standard User", "Health1234");
    CHECK_EQ(f.api.get("/services/data/v1/tooling/healthCheck", f.api.login(user, "Health1234")).status, 403);
}
