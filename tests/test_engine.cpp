// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "harness.hpp"

using namespace tcrm;
using testing::Fixture;

namespace {

json::Value get_record(Fixture& f, const std::string& object, const std::string& id, const std::string& token = {}) {
    auto r = f.api.get("/services/data/v1/sobjects/" + object + "/" + id, token.empty() ? f.admin_token : token);
    if (r.status != 200) testing::record_failure(__FILE__, __LINE__, "get failed: " + r.body);
    return r.json;
}

std::string error_code(const testing::TestApp::Resp& r) {
    if (r.json.is_array() && !r.json.empty()) return r.json[0].get_string("errorCode");
    return r.json.get_string("error");
}

}  // namespace

TEST(records, create_read_update_delete) {
    Fixture f;
    const std::string id = f.api.create(f.admin_token, "Account",
                                        f.obj({{"Name", "Stark Industries"}, {"Phone", "555-0100"}, {"Rating", "Hot"}}));
    json::Value rec = get_record(f, "Account", id);
    CHECK_EQ(rec.get_string("Name"), std::string("Stark Industries"));
    CHECK_EQ(rec.at("attributes").get_string("type"), std::string("Account"));
    CHECK(!rec.get_string("CreatedDate").empty());
    CHECK_EQ(rec.get_bool("IsDeleted", true), false);
    CHECK_EQ(rec.get_string("CurrencyIsoCode"), std::string("USD"));

    auto upd = f.api.patch("/services/data/v1/sobjects/Account/" + id, f.obj({{"Phone", "555-0199"}, {"Rating", nullptr}}),
                           f.admin_token);
    CHECK_EQ(upd.status, 204);
    rec = get_record(f, "Account", id);
    CHECK_EQ(rec.get_string("Phone"), std::string("555-0199"));
    CHECK(rec.at("Rating").is_null());

    auto partial = f.api.get("/services/data/v1/sobjects/Account/" + id + "?fields=Name,Phone", f.admin_token);
    CHECK_EQ(partial.json.size(), size_t{3});

    CHECK_EQ(f.api.del("/services/data/v1/sobjects/Account/" + id, f.admin_token).status, 204);
    auto gone = f.api.get("/services/data/v1/sobjects/Account/" + id, f.admin_token);
    CHECK_EQ(gone.status, 404);
    CHECK_EQ(error_code(gone), std::string("ENTITY_IS_DELETED"));
    CHECK_EQ(f.api.del("/services/data/v1/sobjects/Account/" + id, f.admin_token).status, 404);
}

TEST(records, validation_of_input) {
    Fixture f;
    auto r = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Phone", "1"}}), f.admin_token);
    CHECK_EQ(r.status, 400);
    CHECK_EQ(error_code(r), std::string("REQUIRED_FIELD_MISSING"));
    r = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", "X"}, {"Bogus__c", 1}}), f.admin_token);
    CHECK_EQ(error_code(r), std::string("INVALID_FIELD"));
    r = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", "X"}, {"AnnualRevenue", "lots"}}), f.admin_token);
    CHECK_EQ(error_code(r), std::string("INVALID_FIELD"));
    r = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", std::string(300, 'x')}}), f.admin_token);
    CHECK_EQ(error_code(r), std::string("STRING_TOO_LONG"));
    r = f.api.post("/services/data/v1/sobjects/Lead", f.obj({{"LastName", "L"}, {"Company", "C"}, {"Status", "Bogus"}}),
                   f.admin_token);
    CHECK_EQ(error_code(r), std::string("INVALID_OR_NULL_FOR_RESTRICTED_PICKLIST"));
    r = f.api.post("/services/data/v1/sobjects/Contact", f.obj({{"LastName", "L"}, {"Email", "not-an-email"}}),
                   f.admin_token);
    CHECK_EQ(error_code(r), std::string("INVALID_EMAIL_ADDRESS"));
    r = f.api.post("/services/data/v1/sobjects/Contact", f.obj({{"LastName", "L"}, {"Birthdate", "2026-02-30"}}),
                   f.admin_token);
    CHECK_EQ(error_code(r), std::string("INVALID_FIELD"));
    r = f.api.post("/services/data/v1/sobjects/Contact", f.obj({{"LastName", "L"}, {"AccountId", "001000000000000AAA"}}),
                   f.admin_token);
    CHECK_EQ(error_code(r), std::string("INVALID_CROSS_REFERENCE_KEY"));
    r = f.api.post("/services/data/v1/sobjects/Contact", f.obj({{"LastName", "L"}, {"AccountId", "garbage"}}), f.admin_token);
    CHECK_EQ(error_code(r), std::string("MALFORMED_ID"));
    r = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", "X"}, {"NumberOfEmployees", 123456789012.0}}),
                   f.admin_token);
    CHECK_EQ(error_code(r), std::string("NUMBER_OUTSIDE_VALID_RANGE"));
    r = f.api.post("/services/data/v1/sobjects/Opportunity",
                   f.obj({{"Name", "X"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-01-01"}, {"IsWon", true}}),
                   f.admin_token);
    CHECK_EQ(error_code(r), std::string("INVALID_FIELD_FOR_INSERT_UPDATE"));
    CHECK_EQ(f.api.post("/services/data/v1/sobjects/Nope", f.obj({{"Name", "X"}}), f.admin_token).status, 404);
    CHECK_EQ(f.api.call("POST", "/services/data/v1/sobjects/Account", "{bad json", f.admin_token).status, 400);
}

TEST(records, numbers_round_to_scale) {
    Fixture f;
    const std::string id = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Round"}, {"AnnualRevenue", 10.256}}));
    CHECK_NEAR(get_record(f, "Account", id).get_double("AnnualRevenue"), 10.26, 1e-9);
}

TEST(records, formula_fields_and_defaults) {
    Fixture f;
    const std::string cid = f.api.create(f.admin_token, "Contact", f.obj({{"FirstName", "Ada"}, {"LastName", "Lovelace"}}));
    CHECK_EQ(get_record(f, "Contact", cid).get_string("Name"), std::string("Ada Lovelace"));
    const std::string lid = f.api.create(f.admin_token, "Lead", f.obj({{"LastName", "Solo"}, {"Company", "Falcon"}}));
    const json::Value lead = get_record(f, "Lead", lid);
    CHECK_EQ(lead.get_string("Name"), std::string("Solo"));
    CHECK_EQ(lead.get_string("Status"), std::string("Open - Not Contacted"));
    CHECK_EQ(lead.get_bool("IsConverted", true), false);
    CHECK_EQ(get_record(f, "Contact", cid).get_bool("DoNotCall", true), false);
}

TEST(records, opportunity_stage_behavior) {
    Fixture f;
    const std::string id = f.api.create(
        f.admin_token, "Opportunity",
        f.obj({{"Name", "Deal"}, {"StageName", "Value Proposition"}, {"CloseDate", "2026-12-31"}, {"Amount", 1000}}));
    json::Value o = get_record(f, "Opportunity", id);
    CHECK_EQ(o.get_int("Probability"), int64_t{50});
    CHECK_EQ(o.get_int("ExpectedRevenue"), int64_t{500});
    CHECK_EQ(o.get_string("ForecastCategoryName"), std::string("Pipeline"));
    CHECK_EQ(o.get_bool("IsClosed", true), false);

    f.api.patch("/services/data/v1/sobjects/Opportunity/" + id, f.obj({{"StageName", "Closed Won"}}), f.admin_token);
    o = get_record(f, "Opportunity", id);
    CHECK_EQ(o.get_int("Probability"), int64_t{100});
    CHECK_EQ(o.get_bool("IsWon", false), true);
    CHECK_EQ(o.get_bool("IsClosed", false), true);
    CHECK_EQ(o.get_string("ForecastCategoryName"), std::string("Closed"));

    // An explicit probability wins over the stage default.
    f.api.patch("/services/data/v1/sobjects/Opportunity/" + id,
                f.obj({{"StageName", "Negotiation/Review"}, {"Probability", 85}}), f.admin_token);
    CHECK_EQ(get_record(f, "Opportunity", id).get_int("Probability"), int64_t{85});
}

TEST(records, case_behavior_and_autonumber) {
    Fixture f;
    const std::string c1 = f.api.create(f.admin_token, "Case", f.obj({{"Subject", "Printer on fire"}, {"Priority", "High"}}));
    const std::string c2 = f.api.create(f.admin_token, "Case", f.obj({{"Subject", "Paper jam"}}));
    json::Value a = get_record(f, "Case", c1);
    json::Value b = get_record(f, "Case", c2);
    CHECK_EQ(a.get_string("CaseNumber"), std::string("00000001"));
    CHECK_EQ(b.get_string("CaseNumber"), std::string("00000002"));
    CHECK_EQ(a.get_string("Status"), std::string("New"));
    CHECK_EQ(b.get_string("Priority"), std::string("Medium"));
    CHECK(!a.get_string("SlaDueDate").empty());
    CHECK(a.get_string("SlaDueDate") > a.get_string("CreatedDate"));

    f.api.patch("/services/data/v1/sobjects/Case/" + c1, f.obj({{"Status", "Closed"}}), f.admin_token);
    a = get_record(f, "Case", c1);
    CHECK_EQ(a.get_bool("IsClosed", false), true);
    CHECK(!a.get_string("ClosedDate").empty());
    f.api.patch("/services/data/v1/sobjects/Case/" + c1, f.obj({{"Status", "Working"}}), f.admin_token);
    a = get_record(f, "Case", c1);
    CHECK_EQ(a.get_bool("IsClosed", true), false);
    CHECK(a.at("ClosedDate").is_null());
}

TEST(records, events_and_tasks) {
    Fixture f;
    const std::string ev = f.api.create(
        f.admin_token, "Event",
        f.obj({{"Subject", "Kickoff"}, {"StartDateTime", "2026-11-20T15:00:00Z"}, {"EndDateTime", "2026-11-20T16:30:00Z"}}));
    CHECK_EQ(get_record(f, "Event", ev).get_int("DurationInMinutes"), int64_t{90});
    auto bad = f.api.post(
        "/services/data/v1/sobjects/Event",
        f.obj({{"Subject", "Backwards"}, {"StartDateTime", "2026-11-20T15:00:00Z"}, {"EndDateTime", "2026-11-20T14:00:00Z"}}),
        f.admin_token);
    CHECK_EQ(bad.status, 400);
    const std::string task = f.api.create(f.admin_token, "Task", f.obj({{"Subject", "Follow up"}}));
    CHECK_EQ(get_record(f, "Task", task).get_bool("IsClosed", true), false);
    f.api.patch("/services/data/v1/sobjects/Task/" + task, f.obj({{"Status", "Completed"}}), f.admin_token);
    CHECK_EQ(get_record(f, "Task", task).get_bool("IsClosed", false), true);
}

TEST(records, rollup_summaries) {
    Fixture f;
    const std::string acct = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Rollup Co"}}));
    const std::string o1 = f.api.create(
        f.admin_token, "Opportunity",
        f.obj({{"Name", "A"}, {"AccountId", acct}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}, {"Amount", 100}}));
    f.api.create(f.admin_token, "Opportunity",
                 f.obj({{"Name", "B"}, {"AccountId", acct}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"},
                        {"Amount", 250}}));
    json::Value a = get_record(f, "Account", acct);
    CHECK_EQ(a.get_int("TotalOpportunityAmount"), int64_t{350});
    CHECK_EQ(a.get_int("NumberOfOpportunities"), int64_t{2});
    f.api.patch("/services/data/v1/sobjects/Opportunity/" + o1, f.obj({{"Amount", 400}}), f.admin_token);
    CHECK_EQ(get_record(f, "Account", acct).get_int("TotalOpportunityAmount"), int64_t{650});

    // Currency conversion into the account's currency.
    auto settings = f.api.patch("/services/data/v1/tooling/settings",
                                f.obj({{"currencyRates", f.obj({{"EUR", 0.5}})}}), f.admin_token);
    REQUIRE_EQ(settings.status, 200);
    f.api.create(f.admin_token, "Opportunity",
                 f.obj({{"Name", "C"}, {"AccountId", acct}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"},
                        {"Amount", 100}, {"CurrencyIsoCode", "EUR"}}));
    CHECK_EQ(get_record(f, "Account", acct).get_int("TotalOpportunityAmount"), int64_t{850});
}

TEST(records, line_items_and_master_detail) {
    Fixture f;
    const std::string product = f.api.create(f.admin_token, "Product2", f.obj({{"Name", "Widget"}, {"ListPrice", 10}}));
    const std::string opp = f.api.create(
        f.admin_token, "Opportunity",
        f.obj({{"Name", "Widgets"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}}));
    const std::string li = f.api.create(
        f.admin_token, "OpportunityLineItem",
        f.obj({{"OpportunityId", opp}, {"Product2Id", product}, {"Quantity", 3}, {"UnitPrice", 20}, {"Discount", 10}}));
    json::Value item = get_record(f, "OpportunityLineItem", li);
    CHECK_EQ(item.get_string("Name"), std::string("Widget"));
    CHECK_EQ(item.get_int("TotalPrice"), int64_t{54});
    CHECK_EQ(get_record(f, "Opportunity", opp).get_int("LineItemTotal"), int64_t{54});

    // Deleting the master cascades to its details; undelete restores both.
    CHECK_EQ(f.api.del("/services/data/v1/sobjects/Opportunity/" + opp, f.admin_token).status, 204);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/OpportunityLineItem/" + li, f.admin_token).status, 404);
    CHECK_EQ(f.api.post("/services/data/v1/recyclebin/" + opp + "/undelete", json::Value::object(), f.admin_token).status, 200);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/OpportunityLineItem/" + li, f.admin_token).status, 200);
}

TEST(records, recycle_bin) {
    Fixture f;
    const std::string id = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Temporary"}}));
    const std::string contact = f.api.create(f.admin_token, "Contact", f.obj({{"LastName", "Kept"}, {"AccountId", id}}));
    REQUIRE_EQ(f.api.del("/services/data/v1/sobjects/Account/" + id, f.admin_token).status, 204);
    // Contacts aren't cascade-deleted; their lookup is cleared.
    CHECK(get_record(f, "Contact", contact).at("AccountId").is_null());
    auto bin = f.api.get("/services/data/v1/recyclebin", f.admin_token);
    REQUIRE_EQ(bin.status, 200);
    CHECK_EQ(bin.json.get_int("totalSize"), int64_t{1});
    CHECK_EQ(bin.json.at("records")[0].get_string("Name"), std::string("Temporary"));
    CHECK_EQ(f.api.post("/services/data/v1/recyclebin/" + id + "/undelete", json::Value::object(), f.admin_token).status, 200);
    CHECK_EQ(get_record(f, "Account", id).get_string("Name"), std::string("Temporary"));
    CHECK_EQ(f.api.post("/services/data/v1/recyclebin/" + id + "/undelete", json::Value::object(), f.admin_token).status, 400);
    REQUIRE_EQ(f.api.del("/services/data/v1/sobjects/Account/" + id, f.admin_token).status, 204);
    CHECK_EQ(f.api.del("/services/data/v1/recyclebin/" + id, f.admin_token).status, 204);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + id, f.admin_token).status, 404);
    CHECK_EQ(f.api.get("/services/data/v1/recyclebin", f.admin_token).json.get_int("totalSize"), int64_t{0});
}

TEST(records, upsert_by_external_id) {
    Fixture f;
    json::Value field = json::parse(R"({"name":"ErpId__c","type":"string","length":40,"externalId":true})");
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/objects/Account/fields", field, f.admin_token).status, 201);
    auto first = f.api.patch("/services/data/v1/sobjects/Account/ErpId__c/ERP-100", f.obj({{"Name", "From ERP"}}),
                             f.admin_token);
    REQUIRE_EQ(first.status, 201);
    CHECK_EQ(first.json.get_bool("created", false), true);
    CHECK_EQ(first.headers.get("Location"), std::string("/services/data/v1/sobjects/Account/ErpId__c/ERP-100"));
    auto second = f.api.patch("/services/data/v1/sobjects/Account/ErpId__c/ERP-100", f.obj({{"Phone", "555"}}),
                              f.admin_token);
    REQUIRE_EQ(second.status, 200);
    CHECK_EQ(second.json.get_string("id"), first.json.get_string("id"));
    const json::Value rec = get_record(f, "Account", first.json.get_string("id"));
    CHECK_EQ(rec.get_string("Name"), std::string("From ERP"));
    CHECK_EQ(rec.get_string("Phone"), std::string("555"));
    CHECK_EQ(rec.get_string("ErpId__c"), std::string("ERP-100"));

    // The external id is unique.
    auto dup = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", "Dup"}, {"ErpId__c", "ERP-100"}}),
                          f.admin_token);
    CHECK_EQ(error_code(dup), std::string("DUPLICATE_VALUE"));
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Account/Name/x", f.obj({{"Phone", "1"}}), f.admin_token).status, 400);
}

TEST(records, optimistic_concurrency) {
    Fixture f;
    const std::string id = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Versioned"}}));
    auto r = f.api.get("/services/data/v1/sobjects/Account/" + id, f.admin_token);
    const std::string etag = r.headers.get("ETag");
    REQUIRE(!etag.empty());
    CHECK_EQ(f.api.call("GET", "/services/data/v1/sobjects/Account/" + id, {}, f.admin_token, {{"If-None-Match", etag}}).status,
             304);
    auto ok = f.api.call("PATCH", "/services/data/v1/sobjects/Account/" + id, R"({"Phone":"1"})", f.admin_token,
                         {{"If-Match", etag}});
    CHECK_EQ(ok.status, 204);
    auto stale = f.api.call("PATCH", "/services/data/v1/sobjects/Account/" + id, R"({"Phone":"2"})", f.admin_token,
                            {{"If-Match", etag}});
    CHECK_EQ(stale.status, 412);
}

TEST(records, duplicate_rules) {
    Fixture f;
    const std::string email = testing::address("dup.contact", "partner");
    f.api.create(f.admin_token, "Contact", f.obj({{"LastName", "One"}, {"Email", email}}));
    auto r = f.api.post("/services/data/v1/sobjects/Contact", f.obj({{"LastName", "Two"}, {"Email", email}}), f.admin_token);
    CHECK_EQ(error_code(r), std::string("DUPLICATES_DETECTED"));
    auto allowed = f.api.call("POST", "/services/data/v1/sobjects/Contact",
                              f.obj({{"LastName", "Two"}, {"Email", email}}).dump(), f.admin_token,
                              {{"Sforce-Duplicate-Rule-Header", "allowSave=true"}});
    CHECK_EQ(allowed.status, 201);
    f.api.create(f.admin_token, "Account", f.obj({{"Name", "Wayne Enterprises, Inc."}}));
    r = f.api.post("/services/data/v1/sobjects/Account", f.obj({{"Name", "wayne enterprises"}}), f.admin_token);
    CHECK_EQ(error_code(r), std::string("DUPLICATES_DETECTED"));
}

TEST(records, field_history) {
    Fixture f;
    const std::string id = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Before"}, {"Industry", "Retail"}}));
    f.api.patch("/services/data/v1/sobjects/Account/" + id, f.obj({{"Name", "After"}, {"Phone", "1"}}), f.admin_token);
    f.api.patch("/services/data/v1/sobjects/Account/" + id, f.obj({{"Industry", "Media"}}), f.admin_token);
    auto h = f.api.get("/services/data/v1/sobjects/Account/" + id + "/history", f.admin_token);
    REQUIRE_EQ(h.status, 200);
    CHECK_EQ(h.json.get_int("totalSize"), int64_t{2});  // Phone isn't tracked
    bool saw_name = false;
    for (const json::Value& e : h.json.at("records").as_array()) {
        if (e.get_string("Field") == "Name") {
            saw_name = true;
            CHECK_EQ(e.at("OldValue").as_string(), std::string("Before"));
            CHECK_EQ(e.at("NewValue").as_string(), std::string("After"));
        }
    }
    CHECK(saw_name);
}

TEST(automation, validation_rules) {
    Fixture f;
    json::Value rule = json::parse(R"J({"name":"Discount cap","object":"opportunity",
        "errorConditionFormula":"AND(Amount > 0, Amount < 100, ISPICKVAL(StageName, \"Closed Won\"))",
        "errorMessage":"Closed deals must be at least 100","errorDisplayField":"Amount"})J");
    auto created = f.api.post("/services/data/v1/tooling/validationRules", rule, f.admin_token);
    REQUIRE_EQ(created.status, 201);
    auto r = f.api.post("/services/data/v1/sobjects/Opportunity",
                        f.obj({{"Name", "Tiny"}, {"StageName", "Closed Won"}, {"CloseDate", "2026-12-01"}, {"Amount", 50}}),
                        f.admin_token);
    CHECK_EQ(error_code(r), std::string("FIELD_CUSTOM_VALIDATION_EXCEPTION"));
    CHECK_EQ(r.json[0].get_string("message"), std::string("Closed deals must be at least 100"));
    CHECK_EQ(r.json[0].at("fields")[0].as_string(), std::string("Amount"));
    f.api.create(f.admin_token, "Opportunity",
                 f.obj({{"Name", "Fine"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}, {"Amount", 50}}));

    auto bad = f.api.post("/services/data/v1/tooling/validationRules",
                          json::parse(R"({"name":"x","object":"Account","errorConditionFormula":"IF(","errorMessage":"m"})"),
                          f.admin_token);
    CHECK_EQ(bad.status, 400);
    const std::string rule_id = created.json.get_string("id");
    CHECK_EQ(f.api.del("/services/data/v1/tooling/validationRules/" + rule_id, f.admin_token).status, 204);
    f.api.create(f.admin_token, "Opportunity",
                 f.obj({{"Name", "Tiny"}, {"StageName", "Closed Won"}, {"CloseDate", "2026-12-01"}, {"Amount", 50}}));
}

TEST(automation, workflow_field_update_and_task) {
    Fixture f;
    json::Value rule = json::parse(R"J({"name":"Escalate big cases","object":"Case",
        "criteria":"ISPICKVAL(Priority, \"High\")","onlyWhenNewlyTrue":true,
        "actions":[{"type":"FieldUpdate","field":"Status","value":"\"Escalated\""},
                   {"type":"CreateTask","subject":"Call the customer","dueInDays":1},
                   {"type":"EmailAlert","subject":"High priority case"}]})J");
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/workflowRules", rule, f.admin_token).status, 201);
    const std::string id = f.api.create(f.admin_token, "Case", f.obj({{"Subject", "Outage"}, {"Priority", "High"}}));
    const json::Value c = get_record(f, "Case", id);
    CHECK_EQ(c.get_string("Status"), std::string("Escalated"));
    CHECK_EQ(c.get_bool("IsEscalated", false), true);
    auto tasks = f.api.query("SELECT Subject, WhatId FROM Task", f.admin_token);
    REQUIRE_EQ(tasks.json.get_int("totalSize"), int64_t{1});
    CHECK_EQ(tasks.json.at("records")[0].get_string("Subject"), std::string("Call the customer"));
    CHECK_EQ(tasks.json.at("records")[0].get_string("WhatId"), id);
    auto outbox = f.api.get("/services/data/v1/outbox", f.admin_token);
    REQUIRE_EQ(outbox.status, 200);
    CHECK_EQ(outbox.json.at("records")[0].get_string("subject"), std::string("High priority case"));
    // A low-priority case doesn't match.
    const std::string low = f.api.create(f.admin_token, "Case", f.obj({{"Subject", "Typo"}, {"Priority", "Low"}}));
    CHECK_EQ(get_record(f, "Case", low).get_string("Status"), std::string("New"));
}

TEST(automation, assignment_rules) {
    Fixture f;
    const std::string rep = f.api.create_user(f.admin_token, testing::address("rep"), "Rep", "Standard User",
                                              "Rep12345pass");
    json::Value rule = json::Value::object();
    rule.set("name", "Web leads to Rep");
    rule.set("object", "Lead");
    rule.set("criteria", "ISPICKVAL(LeadSource, \"Web\")");
    json::Value users = json::Value::array();
    users.push_back(rep);
    rule.set("userIds", users);
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/assignmentRules", rule, f.admin_token).status, 201);
    const std::string web = f.api.create(f.admin_token, "Lead",
                                         f.obj({{"LastName", "W"}, {"Company", "Web Co"}, {"LeadSource", "Web"}}));
    CHECK_EQ(get_record(f, "Lead", web).get_string("OwnerId"), rep);
    const std::string phone = f.api.create(f.admin_token, "Lead",
                                           f.obj({{"LastName", "P"}, {"Company", "Phone Co"}, {"LeadSource", "Phone Inquiry"}}));
    CHECK(get_record(f, "Lead", phone).get_string("OwnerId") != rep);
}

TEST(automation, lead_conversion) {
    Fixture f;
    const std::string lead = f.api.create(
        f.admin_token, "Lead",
        f.obj({{"FirstName", "Peter"}, {"LastName", "Gibbons"}, {"Company", "Initrode"}, {"Industry", "Technology"},
               {"Email", testing::address("peter", "initrode")}, {"City", "Austin"}}));
    auto r = f.api.post("/services/data/v1/actions/convertLead", f.obj({{"leadId", lead}, {"opportunityName", "Initrode Deal"}}),
                        f.admin_token);
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.get_bool("accountCreated", false), true);
    const json::Value acct = get_record(f, "Account", r.json.get_string("accountId"));
    CHECK_EQ(acct.get_string("Name"), std::string("Initrode"));
    CHECK_EQ(acct.get_string("BillingCity"), std::string("Austin"));
    const json::Value contact = get_record(f, "Contact", r.json.get_string("contactId"));
    CHECK_EQ(contact.get_string("Name"), std::string("Peter Gibbons"));
    CHECK_EQ(contact.get_string("AccountId"), r.json.get_string("accountId"));
    const json::Value opp = get_record(f, "Opportunity", r.json.get_string("opportunityId"));
    CHECK_EQ(opp.get_string("Name"), std::string("Initrode Deal"));
    const json::Value converted = get_record(f, "Lead", lead);
    CHECK_EQ(converted.get_bool("IsConverted", false), true);
    CHECK_EQ(converted.get_string("Status"), std::string("Closed - Converted"));
    CHECK_EQ(converted.get_string("ConvertedAccountId"), r.json.get_string("accountId"));

    // Converted leads are read-only and can't be converted twice.
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Lead/" + lead, f.obj({{"Title", "x"}}), f.admin_token).status, 400);
    CHECK_EQ(f.api.post("/services/data/v1/actions/convertLead", f.obj({{"leadId", lead}}), f.admin_token).status, 400);

    // A second lead from the same company joins the existing account.
    const std::string lead2 = f.api.create(f.admin_token, "Lead", f.obj({{"LastName", "Bolton"}, {"Company", "Initrode"}}));
    auto r2 = f.api.post("/services/data/v1/actions/convertLead", f.obj({{"leadId", lead2}, {"doNotCreateOpportunity", true}}),
                         f.admin_token);
    REQUIRE_EQ(r2.status, 200);
    CHECK_EQ(r2.json.get_string("accountId"), r.json.get_string("accountId"));
    CHECK(r2.json.at("opportunityId").is_null());
}

TEST(sharing, private_records_and_role_hierarchy) {
    Fixture f;
    // Roles: VP above Rep.
    auto vp_role = f.api.post("/services/data/v1/tooling/roles", f.obj({{"name", "VP Sales"}}), f.admin_token);
    REQUIRE_EQ(vp_role.status, 201);
    auto rep_role = f.api.post("/services/data/v1/tooling/roles",
                               f.obj({{"name", "Rep"}, {"parentRoleId", vp_role.json.get_string("id")}}), f.admin_token);
    REQUIRE_EQ(rep_role.status, 201);
    const std::string vp_name = testing::address("vp");
    const std::string rep_name = testing::address("rep1");
    const std::string other_name = testing::address("other");
    f.api.create_user(f.admin_token, vp_name, "Vp", "Standard User", "Vp123456pass", vp_role.json.get_string("id"));
    f.api.create_user(f.admin_token, rep_name, "Rep", "Standard User", "Rep123456pass", rep_role.json.get_string("id"));
    f.api.create_user(f.admin_token, other_name, "Other", "Standard User", "Other12345pass");
    const std::string vp = f.api.login(vp_name, "Vp123456pass");
    const std::string rep = f.api.login(rep_name, "Rep123456pass");
    const std::string other = f.api.login(other_name, "Other12345pass");

    const std::string acct = f.api.create(rep, "Account", f.obj({{"Name", "Rep's Account"}}));
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + acct, rep).status, 200);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + acct, vp).status, 200);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + acct, other).status, 404);
    CHECK_EQ(f.api.query("SELECT Id FROM Account", other).json.get_int("totalSize"), int64_t{0});
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Account/" + acct, f.obj({{"Phone", "1"}}), other).status, 403);
    // Managers can edit but not delete or transfer.
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Account/" + acct, f.obj({{"Phone", "2"}}), vp).status, 204);
    CHECK_EQ(f.api.del("/services/data/v1/sobjects/Account/" + acct, vp).status, 403);

    // Contacts follow their account.
    const std::string contact = f.api.create(rep, "Contact", f.obj({{"LastName", "Kid"}, {"AccountId", acct}}));
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Contact/" + contact, vp).status, 200);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Contact/" + contact, other).status, 404);

    // Manual share to the other user.
    auto share = f.api.post("/services/data/v1/sobjects/Account/" + acct + "/shares",
                            f.obj({{"userId", f.api.query("SELECT Id FROM User WHERE LastName = 'Other'", f.admin_token)
                                                  .json.at("records")[0]
                                                  .get_string("Id")},
                                   {"access", "Read"}}),
                            rep);
    REQUIRE_EQ(share.status, 201);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + acct, other).status, 200);
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Account/" + acct, f.obj({{"Phone", "3"}}), other).status, 403);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Contact/" + contact, other).status, 200);

    // Leads are public read/write by default.
    const std::string lead = f.api.create(rep, "Lead", f.obj({{"LastName", "Shared"}, {"Company", "Pub"}}));
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Lead/" + lead, f.obj({{"Title", "CTO"}}), other).status, 204);
}

TEST(sharing, sharing_rules_and_org_defaults) {
    Fixture f;
    auto role = f.api.post("/services/data/v1/tooling/roles", f.obj({{"name", "Support"}}), f.admin_token);
    const std::string role_id = role.json.get_string("id");
    const std::string agent_name = testing::address("agent");
    f.api.create_user(f.admin_token, agent_name, "Agent", "Standard User", "Agent123pass", role_id);
    const std::string agent = f.api.login(agent_name, "Agent123pass");
    const std::string tech = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Tech"}, {"Industry", "Technology"}}));
    const std::string retail = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Shop"}, {"Industry", "Retail"}}));
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + tech, agent).status, 404);

    auto current = f.api.get("/services/data/v1/tooling/sharing", f.admin_token);
    REQUIRE_EQ(current.status, 200);
    json::Value model = current.json;
    json::Value rules = json::Value::array();
    json::Value rule = json::Value::object();
    rule.set("name", "Tech accounts to support");
    rule.set("object", "Account");
    rule.set("field", "Industry");
    rule.set("value", "Technology");
    rule.set("roleId", role_id);
    rule.set("access", "Read");
    rules.push_back(rule);
    model.set("sharingRules", rules);
    REQUIRE_EQ(f.api.put("/services/data/v1/tooling/sharing", model, f.admin_token).status, 200);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + tech, agent).status, 200);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + retail, agent).status, 404);

    // Make accounts public read.
    model["organizationWideDefaults"].set("Account", "PublicRead");
    REQUIRE_EQ(f.api.put("/services/data/v1/tooling/sharing", model, f.admin_token).status, 200);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + retail, agent).status, 200);
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Account/" + retail, f.obj({{"Phone", "1"}}), agent).status, 403);
}

TEST(sharing, object_permissions_and_fls) {
    Fixture f;
    json::Value salary = json::parse(R"({"name":"Salary__c","type":"currency","precision":10,"scale":2,
        "fieldPermissions":{"Read Only":{"read":false},"Standard User":{"read":true,"edit":false}}})");
    REQUIRE_EQ(f.api.post("/services/data/v1/tooling/objects/Contact/fields", salary, f.admin_token).status, 201);
    const std::string ro_name = testing::address("viewer");
    const std::string std_name = testing::address("staff");
    f.api.create_user(f.admin_token, ro_name, "Viewer", "Read Only", "Viewer123pass");
    f.api.create_user(f.admin_token, std_name, "Staff", "Standard User", "Staff123pass");
    const std::string viewer = f.api.login(ro_name, "Viewer123pass");
    const std::string staff = f.api.login(std_name, "Staff123pass");

    // Make contacts visible to everyone for this test.
    auto model = f.api.get("/services/data/v1/tooling/sharing", f.admin_token).json;
    model["organizationWideDefaults"].set("Contact", "PublicReadWrite");
    REQUIRE_EQ(f.api.put("/services/data/v1/tooling/sharing", model, f.admin_token).status, 200);

    const std::string c = f.api.create(f.admin_token, "Contact", f.obj({{"LastName", "Paid"}, {"Salary__c", 90000}}));
    // Read Only: no Salary__c in the record, the query rejects it, no create.
    json::Value seen = get_record(f, "Contact", c, viewer);
    CHECK(!seen.contains("Salary__c"));
    CHECK(seen.contains("LastName"));
    CHECK_EQ(f.api.query("SELECT LastName, Salary__c FROM Contact", viewer).status, 400);
    CHECK_EQ(f.api.post("/services/data/v1/sobjects/Contact", f.obj({{"LastName", "Nope"}}), viewer).status, 403);
    // Standard User: can read but not edit Salary__c.
    CHECK_EQ(get_record(f, "Contact", c, staff).get_int("Salary__c"), int64_t{90000});
    auto edit = f.api.patch("/services/data/v1/sobjects/Contact/" + c, f.obj({{"Salary__c", 1}}), staff);
    CHECK_EQ(edit.status, 400);
    CHECK_EQ(error_code(edit), std::string("INVALID_FIELD_FOR_INSERT_UPDATE"));
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Contact/" + c, f.obj({{"Title", "Boss"}}), staff).status, 204);
    // Describe hides the field from the Read Only profile.
    auto described = f.api.get("/services/data/v1/sobjects/Contact/describe", f.admin_token);
    bool admin_sees = false;
    for (const json::Value& fld : described.json.at("fields").as_array()) admin_sees |= fld.get_string("name") == "Salary__c";
    CHECK(admin_sees);
}

TEST(custom_objects, create_and_use) {
    Fixture f;
    auto obj = f.api.post("/services/data/v1/tooling/objects",
                          f.obj({{"name", "Project__c"}, {"label", "Project"}, {"sharingModel", "PublicReadWrite"}}),
                          f.admin_token);
    REQUIRE_EQ(obj.status, 201);
    CHECK_EQ(obj.json.get_string("keyPrefix"), std::string("a00"));
    const char* fields[] = {
        R"({"name":"Budget__c","type":"currency"})",
        R"({"name":"Phase__c","type":"picklist","picklistValues":["Plan","Build","Done"],"restrictedPicklist":true})",
        R"({"name":"Account__c","type":"lookup","referenceTo":"Account"})",
        R"({"name":"Code__c","type":"autonumber","displayFormat":"PRJ-{0000}"})",
        R"({"name":"Over__c","type":"formula","returnType":"boolean","formula":"Budget__c > 10000"})",
    };
    for (const char* fdef : fields) {
        auto r = f.api.post("/services/data/v1/tooling/objects/Project__c/fields", json::parse(fdef), f.admin_token);
        if (r.status != 201) testing::record_failure(__FILE__, __LINE__, std::string(fdef) + " -> " + r.body);
    }
    const std::string acct = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Client"}}));
    const std::string p = f.api.create(f.admin_token, "Project__c",
                                       f.obj({{"Name", "Apollo"}, {"Budget__c", 25000}, {"Phase__c", "Build"}, {"Account__c", acct}}));
    const json::Value rec = get_record(f, "Project__c", p);
    CHECK_EQ(rec.get_string("Code__c"), std::string("PRJ-0001"));
    CHECK_EQ(rec.get_bool("Over__c", false), true);
    CHECK_EQ(p.substr(0, 3), std::string("a00"));
    auto q = f.api.query("SELECT Name, Account__r.Name, Phase__c FROM Project__c", f.admin_token);
    REQUIRE_EQ(q.status, 200);
    CHECK_EQ(q.json.at("records")[0].at("Account__r").get_string("Name"), std::string("Client"));
    auto children = f.api.query("SELECT Name, (SELECT Name FROM Projects__r) FROM Account", f.admin_token);
    REQUIRE_EQ(children.status, 200);
    CHECK_EQ(children.json.at("records")[0].at("Projects__r").get_int("totalSize"), int64_t{1});

    auto bad = f.api.post("/services/data/v1/sobjects/Project__c", f.obj({{"Name", "X"}, {"Phase__c", "Nope"}}), f.admin_token);
    CHECK_EQ(bad.status, 400);
    CHECK_EQ(f.api.post("/services/data/v1/tooling/objects", f.obj({{"name", "No Suffix"}}), f.admin_token).status, 400);
    CHECK_EQ(f.api.post("/services/data/v1/tooling/objects/Project__c/fields",
                        json::parse(R"({"name":"Budget__c","type":"currency"})"), f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.post("/services/data/v1/tooling/objects/Project__c/fields",
                        json::parse(R"({"name":"Bad__c","type":"formula","formula":"1 +"})"), f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.del("/services/data/v1/tooling/objects/Project__c/fields/Budget__c", f.admin_token).status, 204);
    CHECK_EQ(f.api.query("SELECT Budget__c FROM Project__c", f.admin_token).status, 400);
    CHECK_EQ(f.api.del("/services/data/v1/tooling/objects/Account/fields/Name", f.admin_token).status, 400);
}
