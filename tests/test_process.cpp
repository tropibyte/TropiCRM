// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "harness.hpp"
#include "services/templates.hpp"

using namespace tcrm;
using testing::Fixture;

namespace {

std::string error_code(const testing::TestApp::Resp& r) {
    if (r.json.is_array() && !r.json.empty()) return r.json[0].get_string("errorCode");
    return r.json.get_string("error");
}

std::string user_id_of(Fixture& f, const std::string& last_name) {
    auto r = f.api.query("SELECT Id FROM User WHERE LastName = '" + last_name + "'", f.admin_token);
    return r.json.at("records")[0].get_string("Id");
}

}  // namespace

TEST(approvals, manager_approves_and_outcome_applies) {
    Fixture f;
    const std::string mgr_name = testing::address("manager");
    const std::string rep_name = testing::address("seller");
    const std::string mgr_id = f.api.create_user(f.admin_token, mgr_name, "Manager", "Standard User", "Manager123");
    json::Value rep_body = json::Value::object();
    rep_body.set("username", rep_name);
    rep_body.set("lastName", "Seller");
    rep_body.set("password", "Seller12345");
    rep_body.set("managerId", mgr_id);
    REQUIRE_EQ(f.api.post("/services/data/v1/users", rep_body, f.admin_token).status, 201);
    const std::string mgr = f.api.login(mgr_name, "Manager123");
    const std::string rep = f.api.login(rep_name, "Seller12345");

    json::Value process = json::parse(R"J({"name":"Big discount","object":"Opportunity",
        "entryCriteria":"Amount > 1000","steps":[{"name":"Manager","approver":"manager"}],
        "approvedField":"NextStep","approvedValue":"\"Approved by manager\"",
        "rejectedField":"StageName","rejectedValue":"\"Closed Lost\""})J");
    auto created = f.api.post("/services/data/v1/tooling/approvalProcesses", process, f.admin_token);
    REQUIRE_EQ(created.status, 201);

    const std::string small = f.api.create(
        rep, "Opportunity", f.obj({{"Name", "Small"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}, {"Amount", 10}}));
    auto none = f.api.post("/services/data/v1/process/approvals", f.obj({{"recordId", small}}), rep);
    CHECK_EQ(error_code(none), std::string("NO_APPLICABLE_PROCESS"));

    const std::string big = f.api.create(
        rep, "Opportunity", f.obj({{"Name", "Big"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}, {"Amount", 5000}}));
    auto submitted = f.api.post("/services/data/v1/process/approvals", f.obj({{"recordId", big}, {"comments", "please"}}), rep);
    REQUIRE_EQ(submitted.status, 201);
    CHECK_EQ(submitted.json.get_string("currentApproverId"), mgr_id);
    CHECK_EQ(submitted.json.get_string("status"), std::string("Pending"));
    const std::string req_id = submitted.json.get_string("id");

    // The record is locked while pending.
    auto locked = f.api.patch("/services/data/v1/sobjects/Opportunity/" + big, f.obj({{"Amount", 4000}}), rep);
    CHECK_EQ(error_code(locked), std::string("ENTITY_IS_LOCKED"));
    CHECK_EQ(f.api.post("/services/data/v1/process/approvals", f.obj({{"recordId", big}}), rep).status, 400);

    auto queue = f.api.get("/services/data/v1/process/approvals", mgr);
    CHECK_EQ(queue.json.get_int("totalSize"), int64_t{1});
    CHECK_EQ(f.api.get("/services/data/v1/process/approvals?view=submitted", rep).json.get_int("totalSize"), int64_t{1});
    CHECK_EQ(f.api.post("/services/data/v1/process/approvals/" + req_id, f.obj({{"action", "Approve"}}), rep).status, 403);

    auto approved = f.api.post("/services/data/v1/process/approvals/" + req_id,
                               f.obj({{"action", "Approve"}, {"comments", "ok"}}), mgr);
    REQUIRE_EQ(approved.status, 200);
    CHECK_EQ(approved.json.get_string("status"), std::string("Approved"));
    CHECK_EQ(approved.json.at("history").size(), size_t{2});
    auto opp = f.api.get("/services/data/v1/sobjects/Opportunity/" + big, rep);
    CHECK_EQ(opp.json.get_string("NextStep"), std::string("Approved by manager"));
    CHECK_EQ(f.api.patch("/services/data/v1/sobjects/Opportunity/" + big, f.obj({{"Amount", 4000}}), rep).status, 204);
    CHECK_EQ(f.api.post("/services/data/v1/process/approvals/" + req_id, f.obj({{"action", "Reject"}}), mgr).status, 400);

    // Rejection path, then recall.
    const std::string big2 = f.api.create(
        rep, "Opportunity", f.obj({{"Name", "Big 2"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}, {"Amount", 9000}}));
    const std::string r2 = f.api.post("/services/data/v1/process/approvals", f.obj({{"recordId", big2}}), rep).json.get_string("id");
    REQUIRE_EQ(f.api.post("/services/data/v1/process/approvals/" + r2, f.obj({{"action", "Reject"}}), mgr).status, 200);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Opportunity/" + big2, rep).json.get_string("StageName"),
             std::string("Closed Lost"));
    const std::string big3 = f.api.create(
        rep, "Opportunity", f.obj({{"Name", "Big 3"}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}, {"Amount", 9000}}));
    const std::string r3 = f.api.post("/services/data/v1/process/approvals", f.obj({{"recordId", big3}}), rep).json.get_string("id");
    CHECK_EQ(f.api.post("/services/data/v1/process/approvals/" + r3, f.obj({{"action", "Recall"}}), rep).json.get_string("status"),
             std::string("Recalled"));
    CHECK_EQ(f.api.post("/services/data/v1/process/approvals/" + r3, f.obj({{"action", "Dance"}}), rep).status, 400);

    CHECK_EQ(f.api.post("/services/data/v1/tooling/approvalProcesses",
                        json::parse(R"({"name":"x","object":"Opportunity","steps":[]})"), f.admin_token)
                 .status,
             400);
    const auto email = f.api.app.outbox().recent(f.org_id, 20);
    bool asked = false;
    for (const auto& m : email) asked |= m.to_user_id == mgr_id && m.subject.find("Approval requested") == 0;
    CHECK(asked);
}

TEST(templates, render_and_send) {
    Fixture f;
    const std::string acct = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Vandelay Industries"}}));
    const std::string contact = f.api.create(
        f.admin_token, "Contact", f.obj({{"FirstName", "Art"}, {"LastName", "Vandelay"}, {"AccountId", acct}}));
    json::Value tmpl = json::parse(R"({"name":"Welcome","object":"Contact",
        "subject":"Welcome, {!FirstName}!",
        "body":"Hi {!FirstName} from {!Account.Name}. Your title: {!Title, \"not set\"}. {!Unclosed"})");
    auto created = f.api.post("/services/data/v1/emailTemplates", tmpl, f.admin_token);
    REQUIRE_EQ(created.status, 201);
    const std::string id = created.json.get_string("id");
    auto rendered = f.api.post("/services/data/v1/emailTemplates/" + id + "/render", f.obj({{"recordId", contact}}),
                               f.admin_token);
    REQUIRE_EQ(rendered.status, 200);
    CHECK_EQ(rendered.json.get_string("subject"), std::string("Welcome, Art!"));
    CHECK_EQ(rendered.json.get_string("body"),
             std::string("Hi Art from Vandelay Industries. Your title: not set. {!Unclosed"));
    auto sent = f.api.post("/services/data/v1/emailTemplates/" + id + "/send", f.obj({{"recordId", contact}}), f.admin_token);
    CHECK_EQ(sent.json.get_bool("sent", false), true);
    CHECK_EQ(f.api.app.outbox().recent(f.org_id, 1)[0].subject, std::string("Welcome, Art!"));
    CHECK_EQ(f.api.post("/services/data/v1/emailTemplates/" + id + "/render", f.obj({{"recordId", acct}}), f.admin_token).status,
             400);
    CHECK_EQ(f.api.post("/services/data/v1/emailTemplates",
                        json::parse(R"({"name":"Bad","object":"Contact","subject":"{!Nope}","body":""})"), f.admin_token)
                 .status,
             400);
    CHECK_EQ(f.api.get("/services/data/v1/emailTemplates", f.admin_token).json.at("records").size(), size_t{1});
    CHECK_EQ(f.api.del("/services/data/v1/emailTemplates/" + id, f.admin_token).status, 204);
    const auto fields = services::merge_fields("{!A} and {!B.C, \"x\"} {!");
    REQUIRE_EQ(fields.size(), size_t{2});
    CHECK_EQ(fields[1], std::string("B.C"));
}

TEST(merge, accounts_and_children) {
    Fixture f;
    const std::string master = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Kramerica"}, {"Phone", "1"}}));
    const std::string dup = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Kramerica Ind"}, {"Phone", "2"}}));
    const std::string c1 = f.api.create(f.admin_token, "Contact", f.obj({{"LastName", "Kramer"}, {"AccountId", dup}}));
    const std::string o1 = f.api.create(
        f.admin_token, "Opportunity",
        f.obj({{"Name", "Bladder ship"}, {"AccountId", dup}, {"StageName", "Prospecting"}, {"CloseDate", "2026-12-01"}}));
    json::Value body = json::Value::object();
    body.set("object", "Account");
    body.set("masterId", master);
    json::Value dups = json::Value::array();
    dups.push_back(dup);
    body.set("duplicateIds", dups);
    body.set("fields", f.obj({{"Phone", "2"}}));
    auto r = f.api.post("/services/data/v1/actions/merge", body, f.admin_token);
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.get_int("updatedRelatedIds"), int64_t{2});
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + master, f.admin_token).json.get_string("Phone"), std::string("2"));
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Contact/" + c1, f.admin_token).json.get_string("AccountId"), master);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Opportunity/" + o1, f.admin_token).json.get_string("AccountId"), master);
    CHECK_EQ(f.api.get("/services/data/v1/sobjects/Account/" + dup, f.admin_token).status, 404);

    body.set("object", "Opportunity");
    CHECK_EQ(f.api.post("/services/data/v1/actions/merge", body, f.admin_token).status, 400);
    body.set("object", "Account");
    json::Value self = json::Value::array();
    self.push_back(master);
    body.set("duplicateIds", self);
    CHECK_EQ(f.api.post("/services/data/v1/actions/merge", body, f.admin_token).status, 400);
}

TEST(transfer, mass_transfer_requires_admin) {
    Fixture f;
    const std::string a = testing::address("leaving");
    const std::string b = testing::address("staying");
    f.api.create_user(f.admin_token, a, "Leaving", "Standard User", "Leaving123");
    f.api.create_user(f.admin_token, b, "Staying", "Standard User", "Staying123");
    const std::string leaving = f.api.login(a, "Leaving123");
    const std::string staying_token = f.api.login(b, "Staying123");
    for (int i = 0; i < 3; ++i) f.api.create(leaving, "Account", f.obj({{"Name", "Book " + std::to_string(i)}}));
    const std::string from = user_id_of(f, "Leaving");
    const std::string to = user_id_of(f, "Staying");
    json::Value body = json::Value::object();
    body.set("object", "Account");
    body.set("fromUserId", from);
    body.set("toUserId", to);
    CHECK_EQ(f.api.post("/services/data/v1/actions/transferOwnership", body, staying_token).status, 403);
    auto r = f.api.post("/services/data/v1/actions/transferOwnership", body, f.admin_token);
    REQUIRE_EQ(r.status, 200);
    CHECK_EQ(r.json.get_int("transferred"), int64_t{3});
    CHECK_EQ(f.api.query("SELECT Id FROM Account", staying_token).json.get_int("totalSize"), int64_t{3});
    CHECK_EQ(f.api.query("SELECT Id FROM Account", leaving).json.get_int("totalSize"), int64_t{0});
    auto trail = f.api.get("/services/data/v1/setupAuditTrail?section=Data%20Management", f.admin_token);
    REQUIRE_EQ(trail.status, 200);
    CHECK_EQ(trail.json.get_int("totalSize"), int64_t{1});
}

TEST(activities, timeline_for_a_record) {
    Fixture f;
    const std::string acct = f.api.create(f.admin_token, "Account", f.obj({{"Name", "Timeline"}}));
    f.api.create(f.admin_token, "Task", f.obj({{"Subject", "Earlier call"}, {"WhatId", acct}, {"ActivityDate", "2026-01-05"}}));
    f.api.create(f.admin_token, "Event",
                 f.obj({{"Subject", "Later meeting"}, {"WhatId", acct}, {"StartDateTime", "2026-02-01T10:00:00Z"},
                        {"EndDateTime", "2026-02-01T11:00:00Z"}}));
    f.api.create(f.admin_token, "Task", f.obj({{"Subject", "Unrelated"}}));
    auto r = f.api.get("/services/data/v1/sobjects/Account/" + acct + "/activities", f.admin_token);
    REQUIRE_EQ(r.status, 200);
    REQUIRE_EQ(r.json.get_int("totalSize"), int64_t{2});
    CHECK_EQ(r.json.at("records")[0].get_string("Subject"), std::string("Later meeting"));
    CHECK_EQ(r.json.at("records")[1].get_string("Subject"), std::string("Earlier call"));
}

TEST(audit, setup_changes_are_recorded) {
    Fixture f;
    f.api.post("/services/data/v1/tooling/objects/Account/fields", json::parse(R"({"name":"Score__c","type":"double"})"),
               f.admin_token);
    f.api.create_user(f.admin_token, testing::address("audited"), "Audited", "Standard User", "Audited123");
    f.api.patch("/services/data/v1/tooling/settings", f.obj({{"timezone", "Europe/Berlin"}}), f.admin_token);
    auto trail = f.api.get("/services/data/v1/setupAuditTrail", f.admin_token);
    REQUIRE_EQ(trail.status, 200);
    CHECK_EQ(trail.json.get_int("totalSize"), int64_t{3});
    CHECK_EQ(trail.json.at("records")[0].get_string("section"), std::string("Company Profile"));
    CHECK_EQ(trail.json.at("records")[2].get_string("display"), std::string("Account.Score__c"));
    const std::string user = testing::address("audited");
    const std::string token = f.api.login(user, "Audited123");
    CHECK_EQ(f.api.get("/services/data/v1/setupAuditTrail", token).status, 403);
}

TEST(web_forms, web_to_lead_and_case) {
    Fixture f;
    auto lead = f.api.call("POST", "/services/web2lead?oid=" + f.org_id,
                           "first_name=x&LastName=Newman&Company=USPS&Email=" + url::percent_encode(testing::address("newman", "usps")),
                           {}, {{"Content-Type", "application/x-www-form-urlencoded"}});
    // Unknown form fields are rejected like any other unknown field.
    CHECK_EQ(lead.status, 400);
    lead = f.api.call("POST", "/services/web2lead?oid=" + f.org_id, "LastName=Newman&Company=USPS", {},
                      {{"Content-Type", "application/x-www-form-urlencoded"}});
    REQUIRE_EQ(lead.status, 201);
    auto rec = f.api.get("/services/data/v1/sobjects/Lead/" + lead.json.get_string("id"), f.admin_token);
    CHECK_EQ(rec.json.get_string("LeadSource"), std::string("Web"));
    CHECK_EQ(rec.json.get_string("Company"), std::string("USPS"));

    json::Value c = json::Value::object();
    c.set("oid", f.org_id);
    c.set("Subject", "Help");
    c.set("name", "Elaine");
    auto kase = f.api.post("/services/web2case", c);
    REQUIRE_EQ(kase.status, 201);
    auto case_rec = f.api.get("/services/data/v1/sobjects/Case/" + kase.json.get_string("id"), f.admin_token);
    CHECK_EQ(case_rec.json.get_string("SuppliedName"), std::string("Elaine"));
    CHECK_EQ(case_rec.json.get_string("Origin"), std::string("Web"));

    CHECK_EQ(f.api.call("POST", "/services/web2lead?oid=00D000000000000AAA", "LastName=x&Company=y", {},
                        {{"Content-Type", "application/x-www-form-urlencoded"}})
                 .status,
             404);
    REQUIRE_EQ(f.api.patch("/services/data/v1/tooling/settings", f.obj({{"webForms", false}}), f.admin_token).status, 200);
    CHECK_EQ(f.api.post("/services/web2case", c).status, 403);
}

TEST(metrics, prometheus_endpoint) {
    Fixture f;
    f.api.get("/services/data/v1/sobjects", f.admin_token);
    f.api.get("/services/data/v1/nope", f.admin_token);
    auto r = f.api.get("/metrics", "");
    REQUIRE_EQ(r.status, 200);
    CHECK(r.body.find("tropicrm_http_responses_total{class=\"2xx\"}") != std::string::npos);
    CHECK(r.body.find("tropicrm_org_users{org_id=\"" + f.org_id) != std::string::npos);
    CHECK(r.headers.get("Content-Type").find("text/plain") == 0);
}
