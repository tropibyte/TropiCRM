// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <initializer_list>

#include "meta/schema.hpp"

namespace tcrm::meta {
namespace {

FieldDef make(const std::string& name, const std::string& label, FieldType type) {
    FieldDef f;
    f.name = name;
    f.label = label;
    f.type = type;
    return f;
}

FieldDef text(const std::string& name, const std::string& label, int length = 255) {
    FieldDef f = make(name, label, FieldType::Text);
    f.length = length;
    return f;
}

FieldDef textarea(const std::string& name, const std::string& label, int length = 255) {
    FieldDef f = make(name, label, FieldType::TextArea);
    f.length = length;
    return f;
}

FieldDef longtext(const std::string& name, const std::string& label, int length = 32000) {
    FieldDef f = make(name, label, FieldType::LongTextArea);
    f.length = length;
    return f;
}

FieldDef required(FieldDef f) {
    f.required = true;
    return f;
}

FieldDef searchable(FieldDef f) {
    f.searchable = true;
    return f;
}

FieldDef indexed(FieldDef f) {
    f.indexed = true;
    return f;
}

FieldDef read_only(FieldDef f) {
    f.read_only = true;
    return f;
}

FieldDef tracked(FieldDef f) {
    f.history_tracked = true;
    return f;
}

FieldDef number(const std::string& name, const std::string& label, int precision = 18, int scale = 0) {
    FieldDef f = make(name, label, FieldType::Number);
    f.precision = precision;
    f.scale = scale;
    return f;
}

FieldDef currency(const std::string& name, const std::string& label) {
    FieldDef f = make(name, label, FieldType::Currency);
    f.precision = 18;
    f.scale = 2;
    return f;
}

FieldDef percent(const std::string& name, const std::string& label) {
    FieldDef f = make(name, label, FieldType::Percent);
    f.precision = 5;
    f.scale = 2;
    return f;
}

FieldDef checkbox(const std::string& name, const std::string& label, bool default_on = false) {
    FieldDef f = make(name, label, FieldType::Checkbox);
    f.default_value = default_on ? "true" : "false";
    return f;
}

FieldDef picklist(const std::string& name, const std::string& label, std::initializer_list<const char*> values,
                  const char* default_value = nullptr, bool restricted = false) {
    FieldDef f = make(name, label, FieldType::Picklist);
    for (const char* v : values) {
        PicklistValue p;
        p.value = v;
        p.label = v;
        p.is_default = default_value != nullptr && p.value == default_value;
        f.picklist.push_back(std::move(p));
    }
    f.restricted_picklist = restricted;
    return f;
}

FieldDef lookup(const std::string& name, const std::string& label, const std::string& target,
                const std::string& relationship, DeleteRule rule = DeleteRule::SetNull) {
    FieldDef f = make(name, label, FieldType::Lookup);
    f.reference_to = target;
    f.relationship_name = relationship;
    f.delete_rule = rule;
    f.indexed = true;
    f.length = 18;
    return f;
}

FieldDef master_detail(const std::string& name, const std::string& label, const std::string& target,
                       const std::string& relationship) {
    FieldDef f = lookup(name, label, target, relationship, DeleteRule::Cascade);
    f.type = FieldType::MasterDetail;
    f.required = true;
    return f;
}

FieldDef formula(const std::string& name, const std::string& label, FieldType returns, const std::string& expr) {
    FieldDef f = make(name, label, FieldType::Formula);
    f.formula_return = returns;
    f.formula = expr;
    return f;
}

FieldDef rollup(const std::string& name, const std::string& label, const std::string& child,
                const std::string& via, const std::string& op, const std::string& field) {
    FieldDef f = make(name, label, FieldType::RollupSummary);
    f.rollup.child_object = child;
    f.rollup.relationship_field = via;
    f.rollup.op = op;
    f.rollup.field = field;
    return f;
}

void add_address(ObjectDef* o, const std::string& prefix) {
    o->add_field(textarea(prefix + "Street", prefix + " Street"));
    o->add_field(text(prefix + "City", prefix + " City", 40));
    o->add_field(text(prefix + "State", prefix + " State/Province", 80));
    o->add_field(text(prefix + "PostalCode", prefix + " Zip/Postal Code", 20));
    o->add_field(text(prefix + "Country", prefix + " Country", 80));
}

FieldDef currency_code() {
    return picklist("CurrencyIsoCode", "Currency", {"USD", "EUR", "GBP", "JPY", "CAD", "AUD", "INR"}, "USD", true);
}

ObjectDef make_object(const std::string& name, const std::string& label, const std::string& plural,
                      const std::string& prefix, bool with_owner = true) {
    ObjectDef o;
    o.name = name;
    o.label = label;
    o.plural_label = plural;
    o.key_prefix = prefix;
    add_system_fields(&o, with_owner);
    return o;
}

const char* const kLeadSources[] = {"Web", "Phone Inquiry", "Partner Referral", "Purchased List", "Trade Show",
                                    "Word of mouth", "Other"};

FieldDef lead_source() {
    FieldDef f = picklist("LeadSource", "Lead Source", {});
    for (const char* s : kLeadSources) f.picklist.push_back({s, s, false, true});
    return f;
}

FieldDef industry() {
    return picklist("Industry", "Industry",
                    {"Agriculture", "Banking", "Construction", "Education", "Energy", "Finance", "Healthcare",
                     "Manufacturing", "Media", "Retail", "Technology", "Telecommunications", "Transportation",
                     "Other"});
}

ObjectDef account() {
    ObjectDef o = make_object("Account", "Account", "Accounts", "001");
    o.track_history = true;
    o.add_field(tracked(searchable(required(text("Name", "Account Name")))));
    o.fields.back().name_field = true;
    o.add_field(text("AccountNumber", "Account Number", 40));
    o.add_field(tracked(picklist("Type", "Account Type",
                                 {"Prospect", "Customer - Direct", "Customer - Channel", "Partner", "Other"})));
    o.add_field(tracked(industry()));
    o.add_field(tracked(currency("AnnualRevenue", "Annual Revenue")));
    o.add_field(number("NumberOfEmployees", "Employees", 8, 0));
    o.add_field(picklist("Rating", "Account Rating", {"Hot", "Warm", "Cold"}));
    o.add_field(picklist("Ownership", "Ownership", {"Public", "Private", "Subsidiary", "Other"}));
    o.add_field(searchable(make("Phone", "Account Phone", FieldType::Phone)));
    o.add_field(make("Fax", "Account Fax", FieldType::Phone));
    o.add_field(make("Website", "Website", FieldType::Url));
    o.add_field(lookup("ParentId", "Parent Account", "Account", "Parent"));
    add_address(&o, "Billing");
    add_address(&o, "Shipping");
    o.add_field(longtext("Description", "Account Description"));
    o.add_field(currency_code());
    o.add_field(rollup("TotalOpportunityAmount", "Total Opportunity Amount", "Opportunity", "AccountId", "SUM",
                       "Amount"));
    o.add_field(rollup("NumberOfOpportunities", "Number of Opportunities", "Opportunity", "AccountId", "COUNT", ""));
    return o;
}

ObjectDef contact() {
    ObjectDef o = make_object("Contact", "Contact", "Contacts", "003");
    o.track_history = true;
    o.add_field(picklist("Salutation", "Salutation", {"Mr.", "Ms.", "Mrs.", "Mx.", "Dr.", "Prof."}));
    o.add_field(searchable(text("FirstName", "First Name", 40)));
    o.add_field(tracked(searchable(required(text("LastName", "Last Name", 80)))));
    o.add_field(formula("Name", "Full Name", FieldType::Text, "TRIM(BLANKVALUE(FirstName, \"\") & \" \" & LastName)"));
    o.fields.back().name_field = true;
    o.add_field(tracked(lookup("AccountId", "Account Name", "Account", "Account")));
    o.add_field(text("Title", "Title", 128));
    o.add_field(text("Department", "Department", 80));
    o.add_field(tracked(indexed(searchable(make("Email", "Email", FieldType::Email)))));
    o.fields.back().length = 80;
    o.add_field(searchable(make("Phone", "Business Phone", FieldType::Phone)));
    o.add_field(make("MobilePhone", "Mobile Phone", FieldType::Phone));
    o.add_field(make("Birthdate", "Birthdate", FieldType::Date));
    o.add_field(lead_source());
    o.add_field(lookup("ReportsToId", "Reports To", "Contact", "ReportsTo"));
    add_address(&o, "Mailing");
    o.add_field(checkbox("DoNotCall", "Do Not Call"));
    o.add_field(checkbox("HasOptedOutOfEmail", "Email Opt Out"));
    o.add_field(longtext("Description", "Contact Description"));
    o.add_field(currency_code());
    return o;
}

ObjectDef lead() {
    ObjectDef o = make_object("Lead", "Lead", "Leads", "00Q");
    o.track_history = true;
    o.add_field(picklist("Salutation", "Salutation", {"Mr.", "Ms.", "Mrs.", "Mx.", "Dr.", "Prof."}));
    o.add_field(searchable(text("FirstName", "First Name", 40)));
    o.add_field(searchable(required(text("LastName", "Last Name", 80))));
    o.add_field(formula("Name", "Full Name", FieldType::Text, "TRIM(BLANKVALUE(FirstName, \"\") & \" \" & LastName)"));
    o.fields.back().name_field = true;
    o.add_field(searchable(required(text("Company", "Company"))));
    o.add_field(text("Title", "Title", 128));
    o.add_field(indexed(searchable(make("Email", "Email", FieldType::Email))));
    o.fields.back().length = 80;
    o.add_field(searchable(make("Phone", "Phone", FieldType::Phone)));
    o.add_field(make("MobilePhone", "Mobile Phone", FieldType::Phone));
    o.add_field(make("Website", "Website", FieldType::Url));
    o.add_field(tracked(picklist("Status", "Lead Status",
                                 {"Open - Not Contacted", "Working - Contacted", "Closed - Converted",
                                  "Closed - Not Converted"},
                                 "Open - Not Contacted", true)));
    o.add_field(lead_source());
    o.add_field(industry());
    o.add_field(picklist("Rating", "Rating", {"Hot", "Warm", "Cold"}));
    o.add_field(currency("AnnualRevenue", "Annual Revenue"));
    o.add_field(number("NumberOfEmployees", "No. of Employees", 8, 0));
    add_address(&o, "");
    o.add_field(lookup("CampaignId", "Campaign", "Campaign", "Campaign"));
    o.add_field(read_only(checkbox("IsConverted", "Converted")));
    o.add_field(read_only(make("ConvertedDate", "Converted Date", FieldType::Date)));
    o.add_field(read_only(lookup("ConvertedAccountId", "Converted Account", "Account", "ConvertedAccount")));
    o.add_field(read_only(lookup("ConvertedContactId", "Converted Contact", "Contact", "ConvertedContact")));
    o.add_field(read_only(lookup("ConvertedOpportunityId", "Converted Opportunity", "Opportunity",
                                 "ConvertedOpportunity")));
    o.add_field(longtext("Description", "Description"));
    o.add_field(currency_code());
    return o;
}

ObjectDef opportunity() {
    ObjectDef o = make_object("Opportunity", "Opportunity", "Opportunities", "006");
    o.track_history = true;
    o.add_field(tracked(searchable(required(text("Name", "Opportunity Name", 120)))));
    o.fields.back().name_field = true;
    o.add_field(tracked(lookup("AccountId", "Account Name", "Account", "Account")));
    o.add_field(tracked(currency("Amount", "Amount")));
    o.add_field(tracked(required(make("CloseDate", "Close Date", FieldType::Date))));
    o.add_field(tracked(required(picklist(
        "StageName", "Stage",
        {"Prospecting", "Qualification", "Needs Analysis", "Value Proposition", "Id. Decision Makers",
         "Perception Analysis", "Proposal/Price Quote", "Negotiation/Review", "Closed Won", "Closed Lost"},
        "Prospecting", true))));
    o.add_field(tracked(percent("Probability", "Probability (%)")));
    o.add_field(formula("ExpectedRevenue", "Expected Amount", FieldType::Currency,
                        "BLANKVALUE(Amount, 0) * BLANKVALUE(Probability, 0) / 100"));
    o.add_field(picklist("Type", "Opportunity Type",
                         {"New Customer", "Existing Customer - Upgrade", "Existing Customer - Replacement",
                          "Existing Customer - Downgrade"}));
    o.add_field(lead_source());
    o.add_field(text("NextStep", "Next Step"));
    o.add_field(read_only(checkbox("IsClosed", "Closed")));
    o.add_field(read_only(checkbox("IsWon", "Won")));
    o.add_field(read_only(picklist("ForecastCategoryName", "Forecast Category",
                                   {"Pipeline", "Best Case", "Commit", "Closed", "Omitted"})));
    o.add_field(lookup("CampaignId", "Primary Campaign Source", "Campaign", "Campaign"));
    o.add_field(longtext("Description", "Description"));
    o.add_field(currency_code());
    o.add_field(rollup("LineItemTotal", "Line Item Total", "OpportunityLineItem", "OpportunityId", "SUM",
                       "TotalPrice"));
    return o;
}

ObjectDef line_item() {
    ObjectDef o = make_object("OpportunityLineItem", "Opportunity Product", "Opportunity Products", "00k", false);
    o.name_field = "Name";
    o.add_field(read_only(text("Name", "Name")));
    o.fields.back().name_field = true;
    o.add_field(master_detail("OpportunityId", "Opportunity", "Opportunity", "Opportunity"));
    o.add_field(required(lookup("Product2Id", "Product", "Product2", "Product2")));
    o.add_field(required(number("Quantity", "Quantity", 12, 2)));
    o.add_field(required(currency("UnitPrice", "Sales Price")));
    o.add_field(percent("Discount", "Discount"));
    o.add_field(formula("TotalPrice", "Total Price", FieldType::Currency,
                        "Quantity * UnitPrice * (1 - BLANKVALUE(Discount, 0) / 100)"));
    o.add_field(textarea("Description", "Line Description"));
    o.searchable = false;
    return o;
}

ObjectDef product() {
    ObjectDef o = make_object("Product2", "Product", "Products", "01t");
    o.add_field(searchable(required(text("Name", "Product Name"))));
    o.fields.back().name_field = true;
    o.add_field(indexed(text("ProductCode", "Product Code")));
    o.add_field(picklist("Family", "Product Family", {"Hardware", "Software", "Services", "Support"}));
    o.add_field(checkbox("IsActive", "Active", true));
    o.add_field(currency("ListPrice", "List Price"));
    o.add_field(textarea("Description", "Product Description", 4000));
    return o;
}

ObjectDef case_object() {
    ObjectDef o = make_object("Case", "Case", "Cases", "500");
    o.track_history = true;
    o.name_field = "CaseNumber";
    FieldDef num = make("CaseNumber", "Case Number", FieldType::AutoNumber);
    num.autonumber_format = "{00000000}";
    num.name_field = true;
    num.indexed = true;
    num.searchable = true;
    o.add_field(std::move(num));
    o.add_field(searchable(text("Subject", "Subject")));
    o.add_field(longtext("Description", "Description"));
    o.add_field(tracked(picklist("Status", "Status", {"New", "Working", "Escalated", "Closed"}, "New", true)));
    o.add_field(tracked(picklist("Priority", "Priority", {"High", "Medium", "Low"}, "Medium")));
    o.add_field(picklist("Origin", "Case Origin", {"Phone", "Email", "Web"}));
    o.add_field(picklist("Type", "Case Type", {"Question", "Problem", "Feature Request"}));
    o.add_field(picklist("Reason", "Case Reason",
                         {"Installation", "Equipment Complexity", "Performance", "Breakdown", "Feedback", "Other"}));
    o.add_field(lookup("AccountId", "Account Name", "Account", "Account"));
    o.add_field(lookup("ContactId", "Contact Name", "Contact", "Contact"));
    o.add_field(read_only(checkbox("IsClosed", "Closed")));
    o.add_field(read_only(make("ClosedDate", "Closed Date", FieldType::DateTime)));
    o.add_field(checkbox("IsEscalated", "Escalated"));
    o.add_field(read_only(make("SlaDueDate", "SLA Due", FieldType::DateTime)));
    o.add_field(make("SuppliedEmail", "Web Email", FieldType::Email));
    o.add_field(text("SuppliedName", "Web Name", 80));
    return o;
}

ObjectDef task() {
    ObjectDef o = make_object("Task", "Task", "Tasks", "00T");
    o.name_field = "Subject";
    o.add_field(searchable(required(text("Subject", "Subject"))));
    o.fields.back().name_field = true;
    o.add_field(make("ActivityDate", "Due Date", FieldType::Date));
    o.add_field(picklist("Status", "Status",
                         {"Not Started", "In Progress", "Completed", "Waiting on someone else", "Deferred"},
                         "Not Started", true));
    o.add_field(picklist("Priority", "Priority", {"High", "Normal", "Low"}, "Normal"));
    o.add_field(picklist("Type", "Type", {"Call", "Email", "Meeting", "Other"}));
    o.add_field(lookup("WhoId", "Name", "Contact,Lead", "Who"));
    o.add_field(lookup("WhatId", "Related To", "Account,Opportunity,Case,Campaign", "What"));
    o.add_field(read_only(checkbox("IsClosed", "Closed")));
    o.add_field(number("CallDurationInSeconds", "Call Duration", 8, 0));
    o.add_field(make("ReminderDateTime", "Reminder Date/Time", FieldType::DateTime));
    o.add_field(longtext("Description", "Comments"));
    return o;
}

ObjectDef event() {
    ObjectDef o = make_object("Event", "Event", "Events", "00U");
    o.name_field = "Subject";
    o.add_field(searchable(required(text("Subject", "Subject"))));
    o.fields.back().name_field = true;
    o.add_field(required(make("StartDateTime", "Start", FieldType::DateTime)));
    o.add_field(required(make("EndDateTime", "End", FieldType::DateTime)));
    o.add_field(text("Location", "Location"));
    o.add_field(checkbox("IsAllDayEvent", "All-Day Event"));
    o.add_field(lookup("WhoId", "Name", "Contact,Lead", "Who"));
    o.add_field(lookup("WhatId", "Related To", "Account,Opportunity,Case,Campaign", "What"));
    o.add_field(read_only(number("DurationInMinutes", "Duration", 8, 0)));
    o.add_field(longtext("Description", "Description"));
    return o;
}

ObjectDef campaign() {
    ObjectDef o = make_object("Campaign", "Campaign", "Campaigns", "701");
    o.add_field(searchable(required(text("Name", "Campaign Name", 80))));
    o.fields.back().name_field = true;
    o.add_field(picklist("Type", "Type", {"Conference", "Webinar", "Email", "Advertisement", "Other"}));
    o.add_field(picklist("Status", "Status", {"Planned", "In Progress", "Completed", "Aborted"}, "Planned"));
    o.add_field(make("StartDate", "Start Date", FieldType::Date));
    o.add_field(make("EndDate", "End Date", FieldType::Date));
    o.add_field(currency("BudgetedCost", "Budgeted Cost"));
    o.add_field(currency("ActualCost", "Actual Cost"));
    o.add_field(currency("ExpectedRevenue", "Expected Revenue"));
    o.add_field(checkbox("IsActive", "Active"));
    o.add_field(longtext("Description", "Description"));
    return o;
}

ObjectDef note() {
    ObjectDef o = make_object("Note", "Note", "Notes", "002");
    o.name_field = "Title";
    o.add_field(searchable(required(text("Title", "Title", 80))));
    o.fields.back().name_field = true;
    o.add_field(searchable(longtext("Body", "Body")));
    o.add_field(required(lookup("ParentId", "Parent", "*", "Parent")));
    o.add_field(checkbox("IsPrivate", "Private"));
    return o;
}

ObjectDef attachment() {
    ObjectDef o = make_object("Attachment", "Attachment", "Attachments", "00P");
    o.searchable = false;
    o.add_field(required(text("Name", "File Name")));
    o.fields.back().name_field = true;
    o.add_field(required(lookup("ParentId", "Parent", "*", "Parent")));
    o.add_field(text("ContentType", "Content Type", 120));
    o.add_field(read_only(number("BodyLength", "Body Length", 18, 0)));
    o.add_field(read_only(text("ContentHash", "SHA-256", 64)));
    o.add_field(textarea("Description", "Description", 500));
    o.add_field(checkbox("IsPrivate", "Private"));
    return o;
}

ObjectDef user() {
    // A read-only projection of the org's users so queries can reference
    // Owner.Name and "SELECT ... FROM User". Backed by the user store.
    ObjectDef o;
    o.name = "User";
    o.label = "User";
    o.plural_label = "Users";
    o.key_prefix = "005";
    o.searchable = false;
    FieldDef id = make("Id", "User ID", FieldType::Id);
    id.system = true;
    o.add_field(std::move(id));
    o.add_field(read_only(text("Username", "Username", 80)));
    o.add_field(read_only(text("Name", "Full Name", 121)));
    o.fields.back().name_field = true;
    o.add_field(read_only(text("FirstName", "First Name", 40)));
    o.add_field(read_only(text("LastName", "Last Name", 80)));
    o.add_field(read_only(make("Email", "Email", FieldType::Email)));
    o.add_field(read_only(text("Title", "Title", 80)));
    o.add_field(read_only(text("Department", "Department", 80)));
    o.add_field(read_only(checkbox("IsActive", "Active")));
    o.add_field(read_only(text("ProfileName", "Profile", 80)));
    o.add_field(read_only(text("UserRoleId", "Role", 18)));
    o.add_field(read_only(lookup("ManagerId", "Manager", "User", "Manager")));
    o.add_field(read_only(text("TimeZoneSidKey", "Time Zone", 40)));
    return o;
}

}  // namespace

void add_system_fields(ObjectDef* o, bool with_owner) {
    FieldDef id = make("Id", "Record ID", FieldType::Id);
    id.system = true;
    id.length = 18;
    o->add_field(std::move(id));
    if (with_owner) {
        FieldDef owner = lookup("OwnerId", "Owner ID", "User", "Owner");
        owner.history_tracked = true;
        o->add_field(std::move(owner));
    }
    FieldDef created = make("CreatedDate", "Created Date", FieldType::DateTime);
    created.system = true;
    o->add_field(std::move(created));
    FieldDef created_by = lookup("CreatedById", "Created By ID", "User", "CreatedBy");
    created_by.system = true;
    o->add_field(std::move(created_by));
    FieldDef modified = make("LastModifiedDate", "Last Modified Date", FieldType::DateTime);
    modified.system = true;
    o->add_field(std::move(modified));
    FieldDef modified_by = lookup("LastModifiedById", "Last Modified By ID", "User", "LastModifiedBy");
    modified_by.system = true;
    o->add_field(std::move(modified_by));
    FieldDef stamp = make("SystemModstamp", "System Modstamp", FieldType::DateTime);
    stamp.system = true;
    o->add_field(std::move(stamp));
    FieldDef deleted = make("IsDeleted", "Deleted", FieldType::Checkbox);
    deleted.system = true;
    o->add_field(std::move(deleted));
}

std::vector<ObjectDef> standard_objects() {
    std::vector<ObjectDef> out;
    out.push_back(account());
    out.push_back(contact());
    out.push_back(lead());
    out.push_back(opportunity());
    out.push_back(line_item());
    out.push_back(product());
    out.push_back(case_object());
    out.push_back(task());
    out.push_back(event());
    out.push_back(campaign());
    out.push_back(note());
    out.push_back(attachment());
    out.push_back(user());
    return out;
}

}  // namespace tcrm::meta
