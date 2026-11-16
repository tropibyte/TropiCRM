// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/lead_convert.hpp"

#include <mutex>
#include <shared_mutex>

#include "engine/records.hpp"
#include "model/errors.hpp"
#include "query/executor.hpp"
#include "util/id.hpp"
#include "util/log.hpp"
#include "util/time.hpp"

namespace tcrm::engine {
namespace {

void copy_if_set(const Record& from, const char* from_field, json::Value* to, const char* to_field) {
    const FieldValue& v = from.get(from_field);
    if (v.is_null()) return;
    switch (v.kind()) {
        case ValueKind::Bool: to->set(to_field, v.as_bool()); break;
        case ValueKind::Int:
        case ValueKind::Double: to->set(to_field, v.as_double()); break;
        default: to->set(to_field, v.to_text()); break;
    }
}

/// Looks for an existing account with the lead's company name. Runs in
/// system mode so that a rep converting a lead attaches it to the
/// company's account even if someone else owns it.
std::string find_matching_account(const Org& org, const Principal& who, const std::string& company) {
    const Principal system = Principal::system_for(who.org, who.user_id);
    const query::Query q = query::parse("SELECT Id FROM Account WHERE Name = '" + company + "' LIMIT 1");
    query::Executor ex(org, system);
    const std::vector<std::string> found = ex.matching_ids(q, false);
    return found.empty() ? std::string() : found[0];
}

}  // namespace

LeadConvertResult convert_lead(RecordService& records, const Principal& who, const LeadConvertRequest& request) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());

    std::string lead_id;
    if (!ids::normalize(request.lead_id, &lead_id)) throw errors::bad_request("MALFORMED_ID", "Malformed lead id");
    const meta::ObjectDef* lead_obj = org.schema.object("Lead");
    const Record* lead_ptr = org.db.table("Lead").get(lead_id);
    if (lead_ptr == nullptr || lead_ptr->is_deleted()) throw errors::not_found("No such lead: " + request.lead_id);
    AccessChecker access(org, who);
    if (!access.can_edit(*lead_obj, *lead_ptr)) throw errors::forbidden();
    const Record lead = *lead_ptr;
    if (lead.get("IsConverted").kind() == ValueKind::Bool && lead.get("IsConverted").as_bool()) {
        throw errors::bad_request("CANNOT_UPDATE_CONVERTED_LEAD", "This lead was already converted");
    }

    const std::string owner = request.owner_id.empty() ? lead.owner_id() : request.owner_id;
    SaveOptions opts;
    opts.allow_duplicates = true;
    LeadConvertResult result;
    result.lead_id = lead_id;

    // Account: the one given, an existing match, or a new one.
    const meta::ObjectDef* account_obj = org.schema.object("Account");
    if (!request.account_id.empty()) {
        std::string aid;
        if (!ids::normalize(request.account_id, &aid)) throw errors::bad_request("MALFORMED_ID", "Malformed account id");
        const Record* acct = org.db.table("Account").get(aid);
        if (acct == nullptr || acct->is_deleted() || !access.can_read(*account_obj, *acct)) {
            throw errors::bad_request("INVALID_CROSS_REFERENCE_KEY", "invalid account id");
        }
        result.account_id = aid;
    } else {
        const std::string company = lead.text("Company");
        result.account_id = find_matching_account(org, who, company);
        if (result.account_id.empty()) {
            json::Value acct = json::Value::object();
            acct.set("Name", company);
            copy_if_set(lead, "Industry", &acct, "Industry");
            copy_if_set(lead, "AnnualRevenue", &acct, "AnnualRevenue");
            copy_if_set(lead, "NumberOfEmployees", &acct, "NumberOfEmployees");
            copy_if_set(lead, "Phone", &acct, "Phone");
            copy_if_set(lead, "Website", &acct, "Website");
            copy_if_set(lead, "Street", &acct, "BillingStreet");
            copy_if_set(lead, "City", &acct, "BillingCity");
            copy_if_set(lead, "State", &acct, "BillingState");
            copy_if_set(lead, "PostalCode", &acct, "BillingPostalCode");
            copy_if_set(lead, "Country", &acct, "BillingCountry");
            copy_if_set(lead, "CurrencyIsoCode", &acct, "CurrencyIsoCode");
            if (!owner.empty()) acct.set("OwnerId", owner);
            result.account_id = records.create_locked(org, who, *account_obj, acct, opts).id;
            result.account_created = true;
        }
    }

    // Contact.
    const meta::ObjectDef* contact_obj = org.schema.object("Contact");
    if (!request.contact_id.empty()) {
        std::string cid;
        if (!ids::normalize(request.contact_id, &cid)) throw errors::bad_request("MALFORMED_ID", "Malformed contact id");
        json::Value link = json::Value::object();
        link.set("AccountId", result.account_id);
        result.contact_id = records.update_locked(org, who, *contact_obj, cid, link, opts).id;
    } else {
        json::Value contact = json::Value::object();
        copy_if_set(lead, "Salutation", &contact, "Salutation");
        copy_if_set(lead, "FirstName", &contact, "FirstName");
        copy_if_set(lead, "LastName", &contact, "LastName");
        copy_if_set(lead, "Title", &contact, "Title");
        copy_if_set(lead, "Email", &contact, "Email");
        copy_if_set(lead, "Phone", &contact, "Phone");
        copy_if_set(lead, "MobilePhone", &contact, "MobilePhone");
        copy_if_set(lead, "LeadSource", &contact, "LeadSource");
        copy_if_set(lead, "Street", &contact, "MailingStreet");
        copy_if_set(lead, "City", &contact, "MailingCity");
        copy_if_set(lead, "State", &contact, "MailingState");
        copy_if_set(lead, "PostalCode", &contact, "MailingPostalCode");
        copy_if_set(lead, "Country", &contact, "MailingCountry");
        copy_if_set(lead, "Description", &contact, "Description");
        contact.set("AccountId", result.account_id);
        if (!owner.empty()) contact.set("OwnerId", owner);
        result.contact_id = records.create_locked(org, who, *contact_obj, contact, opts).id;
    }

    // Opportunity.
    if (request.create_opportunity) {
        const meta::ObjectDef* opp_obj = org.schema.object("Opportunity");
        json::Value opp = json::Value::object();
        opp.set("Name", request.opportunity_name.empty() ? lead.text("Company") + "-" : request.opportunity_name);
        opp.set("AccountId", result.account_id);
        opp.set("StageName", "Prospecting");
        opp.set("CloseDate", timeutil::format_date(org.tz().local_date(timeutil::now_ms()) + 30));
        copy_if_set(lead, "LeadSource", &opp, "LeadSource");
        copy_if_set(lead, "CampaignId", &opp, "CampaignId");
        copy_if_set(lead, "CurrencyIsoCode", &opp, "CurrencyIsoCode");
        if (!owner.empty()) opp.set("OwnerId", owner);
        result.opportunity_id = records.create_locked(org, who, *opp_obj, opp, opts).id;
    }

    // Finally mark the lead converted. These fields are read-only through
    // the API, so write them directly.
    Record converted = lead;
    const int64_t now = timeutil::now_ms();
    converted.set("IsConverted", FieldValue::boolean(true));
    converted.set("ConvertedDate", FieldValue::date(org.tz().local_date(now)));
    converted.set("ConvertedAccountId", FieldValue::string(result.account_id));
    converted.set("ConvertedContactId", FieldValue::string(result.contact_id));
    if (!result.opportunity_id.empty()) {
        converted.set("ConvertedOpportunityId", FieldValue::string(result.opportunity_id));
    }
    converted.set("Status", FieldValue::string(request.converted_status));
    converted.set(sysfield::kLastModifiedDate, FieldValue::datetime(now));
    converted.set(sysfield::kLastModifiedById, FieldValue::string(who.user_id));
    converted.set(sysfield::kSystemModstamp, FieldValue::datetime(now));
    ++converted.version;
    org.db.put(converted);
    if (records.hooks()) records.hooks()->saved(org, *lead_obj, &lead, converted);
    log::info("leads", org.id(), ": converted ", lead_id, " -> account ", result.account_id);
    return result;
}

}  // namespace tcrm::engine
