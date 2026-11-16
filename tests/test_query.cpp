// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "harness.hpp"
#include "query/ast.hpp"
#include "query/executor.hpp"

using namespace tcrm;
using testing::Fixture;

namespace {

/// Accounts and opportunities shared by the query tests.
struct QueryData : Fixture {
    std::string globex, initech, umbrella;

    QueryData() {
        globex = api.create(admin_token, "Account",
                            obj({{"Name", "Globex"}, {"Industry", "Technology"}, {"AnnualRevenue", 5000000},
                                 {"NumberOfEmployees", 120}, {"BillingCity", "Atlanta"}}));
        initech = api.create(admin_token, "Account",
                             obj({{"Name", "Initech"}, {"Industry", "Technology"}, {"AnnualRevenue", 750000},
                                  {"BillingCity", "Austin"}}));
        umbrella = api.create(admin_token, "Account",
                              obj({{"Name", "Umbrella Health"}, {"Industry", "Healthcare"}, {"BillingCity", "Raccoon"}}));
        opp("Globex Renewal", globex, "Closed Won", 120000, "2026-02-10");
        opp("Globex Upsell", globex, "Negotiation/Review", 40000, "2026-12-01");
        opp("Initech Pilot", initech, "Prospecting", 15000, "2027-01-15");
        opp("Umbrella Lab", umbrella, "Closed Lost", 90000, "2026-03-03");
        api.create(admin_token, "Contact",
                   obj({{"FirstName", "Hank"}, {"LastName", "Scorpio"}, {"AccountId", globex},
                        {"Email", testing::address("hank", "globex")}}));
        api.create(admin_token, "Contact", obj({{"FirstName", "Bill"}, {"LastName", "Lumbergh"}, {"AccountId", initech}}));
        api.create(admin_token, "Contact", obj({{"LastName", "Orphan"}}));
    }

    void opp(const std::string& name, const std::string& account, const std::string& stage, double amount,
             const std::string& close) {
        api.create(admin_token, "Opportunity",
                   obj({{"Name", name}, {"AccountId", account}, {"StageName", stage}, {"Amount", amount},
                        {"CloseDate", close}}));
    }

    testing::TestApp::Resp q(const std::string& tql) { return api.query(tql, admin_token); }

    std::vector<std::string> names(const std::string& tql, const std::string& field = "Name") {
        testing::TestApp::Resp r = q(tql);
        std::vector<std::string> out;
        if (r.status != 200) {
            testing::record_failure(__FILE__, __LINE__, "query failed: " + r.body);
            return out;
        }
        for (const json::Value& rec : r.json.at("records").as_array()) out.push_back(rec.get_string(field));
        return out;
    }
};

using Names = std::vector<std::string>;

}  // namespace

TEST(query_parser, parses_clauses) {
    const query::Query q = query::parse(
        "SELECT Id, Name, Account.Owner.Name, COUNT(Id) cnt FROM Opportunity WHERE (Amount > 100 AND NOT IsClosed = "
        "true) OR StageName IN ('Prospecting', 'Qualification') GROUP BY Name ORDER BY Name DESC NULLS LAST LIMIT 10 "
        "OFFSET 5");
    CHECK_EQ(q.from, std::string("Opportunity"));
    CHECK_EQ(q.select.size(), size_t{4});
    CHECK_EQ(q.select[2].path.parts.size(), size_t{3});
    CHECK(q.select[3].kind == query::SelectItem::Kind::Aggregate);
    CHECK_EQ(q.select[3].alias, std::string("cnt"));
    REQUIRE(q.where.has_value());
    CHECK(q.where->kind == query::Condition::Kind::Or);
    CHECK_EQ(q.group_by.size(), size_t{1});
    REQUIRE_EQ(q.order_by.size(), size_t{1});
    CHECK(q.order_by[0].descending);
    CHECK(q.order_by[0].nulls == query::OrderItem::Nulls::Last);
    CHECK_EQ(q.limit, int64_t{10});
    CHECK_EQ(q.offset, int64_t{5});
    CHECK(q.is_aggregate());
}

TEST(query_parser, literals) {
    const query::Query q = query::parse(
        "SELECT Id FROM Event WHERE StartDateTime > 2026-01-01T09:00:00Z AND ActivityDate = 2026-01-02 AND "
        "CreatedDate = LAST_N_DAYS:30 AND Subject = 'It\\'s here' AND Amount >= -12.5");
    REQUIRE(q.where.has_value());
    const auto& c = q.where->children;
    REQUIRE_EQ(c.size(), size_t{5});
    CHECK(c[0].values[0].is_datetime_literal);
    CHECK(c[1].values[0].is_date_literal);
    CHECK(c[2].values[0].kind == query::Operand::Kind::DateRange);
    CHECK_EQ(c[2].values[0].literal, std::string("LAST_N_DAYS:30"));
    CHECK_EQ(c[3].values[0].value.as_string(), std::string("It's here"));
    CHECK_NEAR(c[4].values[0].value.as_double(), -12.5, 1e-12);
}

TEST(query_parser, rejects_bad_syntax) {
    const char* bad[] = {"SELECT FROM Account", "SELECT Id Account", "SELECT Id FROM", "SELECT Id FROM Account WHERE",
                         "SELECT Id FROM Account WHERE Name = ", "SELECT Id FROM Account LIMIT -1",
                         "SELECT Id FROM Account WHERE Name IN 'x'", "SELECT Id FROM Account ORDER Name",
                         "SELECT Id FROM Account WHERE Name = 'unterminated", "SELECT Id FROM Account trailing",
                         "SELECT SUM() FROM Account"};
    for (const char* text : bad) {
        bool threw = false;
        try {
            query::parse(text);
        } catch (const ApiError& e) {
            threw = e.code() == "MALFORMED_QUERY";
        }
        if (!threw) testing::record_failure(__FILE__, __LINE__, std::string("accepted: ") + text);
    }
}

TEST(query_parser, quote_literal_round_trips) {
    const std::string tricky = "O'Brien \\ \"Quoted\"\nLine";
    const query::Query q = query::parse("SELECT Id FROM Account WHERE Name = " + query::quote_literal(tricky));
    CHECK_EQ(q.where->values[0].value.as_string(), tricky);
}

TEST(query_exec, filters_and_ordering) {
    QueryData d;
    CHECK(d.names("SELECT Name FROM Account WHERE Industry = 'Technology' ORDER BY Name") == Names({"Globex", "Initech"}));
    CHECK(d.names("SELECT Name FROM Account WHERE industry = 'technology' ORDER BY Name DESC") ==
          Names({"Initech", "Globex"}));
    CHECK(d.names("SELECT Name FROM Account WHERE AnnualRevenue > 1000000") == Names({"Globex"}));
    CHECK(d.names("SELECT Name FROM Account WHERE AnnualRevenue = null") == Names({"Umbrella Health"}));
    CHECK(d.names("SELECT Name FROM Account WHERE AnnualRevenue != null ORDER BY AnnualRevenue") ==
          Names({"Initech", "Globex"}));
    CHECK(d.names("SELECT Name FROM Account WHERE BillingCity IN ('Austin', 'Raccoon') ORDER BY Name") ==
          Names({"Initech", "Umbrella Health"}));
    CHECK(d.names("SELECT Name FROM Account WHERE BillingCity NOT IN ('Austin') ORDER BY Name") ==
          Names({"Globex", "Umbrella Health"}));
    CHECK(d.names("SELECT Name FROM Account WHERE Name LIKE 'g%' OR Name LIKE '%HEALTH'  ORDER BY Name") ==
          Names({"Globex", "Umbrella Health"}));
    CHECK(d.names("SELECT Name FROM Account WHERE Name LIKE 'Init_ch'") == Names({"Initech"}));
    CHECK(d.names("SELECT Name FROM Account WHERE NOT (Industry = 'Technology') ") == Names({"Umbrella Health"}));
    CHECK(d.names("SELECT Name FROM Account ORDER BY AnnualRevenue DESC NULLS FIRST") ==
          Names({"Umbrella Health", "Globex", "Initech"}));
    CHECK(d.names("SELECT Name FROM Account ORDER BY AnnualRevenue") == Names({"Umbrella Health", "Initech", "Globex"}));
}

TEST(query_exec, limit_and_offset) {
    QueryData d;
    CHECK(d.names("SELECT Name FROM Account ORDER BY Name LIMIT 2") == Names({"Globex", "Initech"}));
    CHECK(d.names("SELECT Name FROM Account ORDER BY Name LIMIT 2 OFFSET 1") == Names({"Initech", "Umbrella Health"}));
    CHECK(d.names("SELECT Name FROM Account ORDER BY Name LIMIT 0").empty());
    const auto r = d.q("SELECT Name FROM Account OFFSET 2001");
    CHECK_EQ(r.status, 400);
}

TEST(query_exec, dates_and_date_literals) {
    QueryData d;
    CHECK(d.names("SELECT Name FROM Opportunity WHERE CloseDate < 2026-04-01 ORDER BY CloseDate") ==
          Names({"Globex Renewal", "Umbrella Lab"}));
    CHECK(d.names("SELECT Name FROM Opportunity WHERE CloseDate >= 2027-01-01") == Names({"Initech Pilot"}));
    const auto today = d.q("SELECT Name FROM Account WHERE CreatedDate = TODAY");
    REQUIRE_EQ(today.status, 200);
    CHECK_EQ(today.json.get_int("totalSize"), int64_t{3});
    CHECK_EQ(d.q("SELECT Name FROM Account WHERE CreatedDate = LAST_N_DAYS:7").json.get_int("totalSize"), int64_t{3});
    CHECK_EQ(d.q("SELECT Name FROM Account WHERE CreatedDate < YESTERDAY").json.get_int("totalSize"), int64_t{0});
    CHECK_EQ(d.q("SELECT Name FROM Account WHERE CreatedDate > 2020-01-01T00:00:00Z").json.get_int("totalSize"),
             int64_t{3});
    CHECK_EQ(d.q("SELECT Name FROM Account WHERE Name = TODAY").status, 400);
}

TEST(query_exec, type_checking) {
    QueryData d;
    auto r = d.q("SELECT Name FROM Account WHERE AnnualRevenue = '5'");
    CHECK_EQ(r.status, 400);
    CHECK(r.body.find("must be of type double") != std::string::npos);
    CHECK_EQ(d.q("SELECT Name FROM Account WHERE Name = 5").status, 400);
    CHECK_EQ(d.q("SELECT Name FROM Account WHERE Name LIKE 5").status, 400);
    CHECK_EQ(d.q("SELECT Nope FROM Account").status, 400);
    CHECK_EQ(d.q("SELECT Name FROM Nope").status, 400);
    CHECK_EQ(d.q("SELECT Name FROM Account WHERE Industry INCLUDES ('x')").status, 400);
}

TEST(query_exec, relationships) {
    QueryData d;
    auto r = d.q("SELECT Name, Account.Name, Account.Owner.Name FROM Opportunity WHERE Account.BillingCity = 'Austin'");
    REQUIRE_EQ(r.status, 200);
    REQUIRE_EQ(r.json.get_int("totalSize"), int64_t{1});
    const json::Value& rec = r.json.at("records")[0];
    CHECK_EQ(rec.get_string("Name"), std::string("Initech Pilot"));
    CHECK_EQ(rec.at("Account").get_string("Name"), std::string("Initech"));
    CHECK_EQ(rec.at("Account").at("Owner").get_string("Name"), std::string("Test Admin"));
    CHECK_EQ(rec.at("Account").at("attributes").get_string("type"), std::string("Account"));

    // A null lookup yields a null relationship.
    r = d.q("SELECT LastName, Account.Name FROM Contact WHERE LastName = 'Orphan'");
    REQUIRE_EQ(r.status, 200);
    CHECK(r.json.at("records")[0].at("Account").is_null());

    // Order by a parent field.
    CHECK(d.names("SELECT Name FROM Opportunity ORDER BY Account.Name, Name") ==
          Names({"Globex Renewal", "Globex Upsell", "Initech Pilot", "Umbrella Lab"}));
    CHECK_EQ(d.q("SELECT Name, Nope.Name FROM Opportunity").status, 400);
}

TEST(query_exec, child_subqueries) {
    QueryData d;
    auto r = d.q("SELECT Name, (SELECT Name FROM Opportunities WHERE IsClosed = false ORDER BY Name), "
                 "(SELECT LastName FROM Contacts) FROM Account ORDER BY Name");
    REQUIRE_EQ(r.status, 200);
    const json::Value& globex = r.json.at("records")[0];
    CHECK_EQ(globex.get_string("Name"), std::string("Globex"));
    CHECK_EQ(globex.at("Opportunities").get_int("totalSize"), int64_t{1});
    CHECK_EQ(globex.at("Opportunities").at("records")[0].get_string("Name"), std::string("Globex Upsell"));
    CHECK_EQ(globex.at("Contacts").at("records")[0].get_string("LastName"), std::string("Scorpio"));
    const json::Value& umbrella = r.json.at("records")[2];
    CHECK(umbrella.at("Opportunities").is_null());
    CHECK(umbrella.at("Contacts").is_null());
    CHECK_EQ(d.q("SELECT Name, (SELECT Id FROM Nothings) FROM Account").status, 400);
}

TEST(query_exec, aggregates) {
    QueryData d;
    auto r = d.q("SELECT COUNT() FROM Opportunity");
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.get_int("totalSize"), int64_t{4});
    CHECK(r.json.at("records").empty());

    r = d.q("SELECT IsWon, COUNT(Id) n, SUM(Amount) total, MAX(Amount), MIN(CloseDate) FROM Opportunity GROUP BY "
            "IsWon ORDER BY IsWon");
    REQUIRE_EQ(r.status, 200);
    REQUIRE_EQ(r.json.get_int("totalSize"), int64_t{2});
    const json::Value& lost = r.json.at("records")[0];
    CHECK_EQ(lost.get_bool("IsWon", true), false);
    CHECK_EQ(lost.get_int("n"), int64_t{3});
    CHECK_NEAR(lost.get_double("total"), 145000.0, 1e-6);
    CHECK_NEAR(lost.get_double("expr0"), 90000.0, 1e-6);
    CHECK_EQ(lost.get_string("expr1"), std::string("2026-03-03"));

    r = d.q("SELECT Account.Industry, SUM(Amount) FROM Opportunity GROUP BY Account.Industry HAVING SUM(Amount) > "
            "100000 ORDER BY SUM(Amount) DESC");
    REQUIRE_EQ(r.status, 200);
    REQUIRE_EQ(r.json.get_int("totalSize"), int64_t{1});
    CHECK_EQ(r.json.at("records")[0].get_string("Industry"), std::string("Technology"));
    CHECK_NEAR(r.json.at("records")[0].get_double("expr0"), 175000.0, 1e-6);

    r = d.q("SELECT COUNT_DISTINCT(AccountId) accounts, AVG(Amount) avg FROM Opportunity");
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.at("records")[0].get_int("accounts"), int64_t{3});
    CHECK_NEAR(r.json.at("records")[0].get_double("avg"), 66250.0, 1e-6);

    CHECK_EQ(d.q("SELECT Name, COUNT(Id) FROM Opportunity").status, 400);
    CHECK_EQ(d.q("SELECT SUM(Name) FROM Opportunity").status, 400);
}

TEST(query_exec, polymorphic_and_users) {
    QueryData d;
    d.api.create(d.admin_token, "Task",
                 d.obj({{"Subject", "Call Globex"}, {"WhatId", d.globex}, {"ActivityDate", "2026-11-20"}}));
    auto r = d.q("SELECT Subject, What.Name, Owner.Name FROM Task");
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.at("records")[0].at("What").get_string("Name"), std::string("Globex"));
    CHECK_EQ(d.q("SELECT Subject, What.Industry FROM Task").status, 400);

    r = d.q("SELECT Name, Username, IsActive, ProfileName FROM User");
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.get_int("totalSize"), int64_t{1});
    CHECK_EQ(r.json.at("records")[0].get_string("ProfileName"), std::string("System Administrator"));
}

TEST(query_exec, cursor_pagination) {
    Fixture f;
    for (int i = 0; i < 230; ++i) {
        f.api.create(f.admin_token, "Lead",
                     f.obj({{"LastName", "Lead " + std::to_string(i)}, {"Company", "Batch Co"}}));
    }
    auto first = f.api.call("GET", "/services/data/v1/query?q=" + url::percent_encode("SELECT Id FROM Lead"), {},
                            f.admin_token, {{"Sforce-Query-Options", "batchSize=200"}});
    REQUIRE_EQ(first.status, 200);
    CHECK_EQ(first.json.get_int("totalSize"), int64_t{230});
    CHECK_EQ(first.json.get_bool("done", true), false);
    CHECK_EQ(first.json.at("records").size(), size_t{200});
    const std::string next = first.json.get_string("nextRecordsUrl");
    REQUIRE(!next.empty());
    auto second = f.api.get(next, f.admin_token);
    REQUIRE_EQ(second.status, 200);
    CHECK_EQ(second.json.at("records").size(), size_t{30});
    CHECK_EQ(second.json.get_bool("done", false), true);
    // A cursor can be read once.
    CHECK_EQ(f.api.get(next, f.admin_token).status, 400);
}

TEST(query_exec, deleted_rows_and_query_all) {
    QueryData d;
    REQUIRE_EQ(d.api.del("/services/data/v1/sobjects/Account/" + d.umbrella, d.admin_token).status, 204);
    CHECK_EQ(d.q("SELECT Id FROM Account").json.get_int("totalSize"), int64_t{2});
    auto all = d.api.get("/services/data/v1/queryAll?q=" + url::percent_encode("SELECT Id, IsDeleted FROM Account"),
                         d.admin_token);
    CHECK_EQ(all.json.get_int("totalSize"), int64_t{3});
    CHECK_EQ(d.q("SELECT Id FROM Account ALL ROWS").json.get_int("totalSize"), int64_t{3});
}

TEST(query_exec, like_matching) {
    CHECK(query::like_match("Hello World", "hello%"));
    CHECK(query::like_match("Hello World", "%WORLD"));
    CHECK(query::like_match("Hello World", "%lo W%"));
    CHECK(query::like_match("abc", "a_c"));
    CHECK(!query::like_match("abc", "a_"));
    CHECK(query::like_match("100%", "100\\%"));
    CHECK(!query::like_match("1000", "100\\%"));
    CHECK(query::like_match("", "%"));
    CHECK(!query::like_match("", "_"));
}
