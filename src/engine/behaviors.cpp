// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <ctime>

#include "engine/records.hpp"
#include "engine/record_service.hpp"
#include "model/errors.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::engine {
namespace {

struct StageInfo {
    const char* name;
    int probability;
    const char* forecast;
    bool closed;
    bool won;
};

constexpr StageInfo kStages[] = {
    {"Prospecting", 10, "Pipeline", false, false},
    {"Qualification", 10, "Pipeline", false, false},
    {"Needs Analysis", 20, "Pipeline", false, false},
    {"Value Proposition", 50, "Pipeline", false, false},
    {"Id. Decision Makers", 60, "Pipeline", false, false},
    {"Perception Analysis", 70, "Best Case", false, false},
    {"Proposal/Price Quote", 75, "Best Case", false, false},
    {"Negotiation/Review", 90, "Commit", false, false},
    {"Closed Won", 100, "Closed", true, true},
    {"Closed Lost", 0, "Omitted", true, false},
};

bool requested(const json::Value& fields, const char* name) {
    if (!fields.is_object()) return false;
    for (const auto& [k, v] : fields.as_object()) {
        if (str::iequals(k, name)) return true;
    }
    return false;
}

void opportunity(Record* r, const Record* before, const json::Value& fields) {
    const std::string stage = r->text("StageName");
    const bool stage_changed = before == nullptr || before->text("StageName") != stage;
    int probability = 0;
    std::string forecast;
    bool closed = false, won = false;
    if (!stage_defaults(stage, &probability, &forecast, &closed, &won)) return;
    if (stage_changed && !requested(fields, "Probability")) {
        r->set("Probability", FieldValue::number(probability));
    }
    r->set("IsClosed", FieldValue::boolean(closed));
    r->set("IsWon", FieldValue::boolean(won));
    r->set("ForecastCategoryName", FieldValue::string(forecast));
}

void case_record(const Org& org, Record* r, const Record* before) {
    const std::string status = r->text("Status");
    const bool closed = status == "Closed";
    const bool was_closed = before != nullptr && before->text("Status") == "Closed";
    r->set("IsClosed", FieldValue::boolean(closed));
    if (closed && !was_closed) r->set("ClosedDate", FieldValue::datetime(timeutil::now_ms()));
    if (!closed && was_closed) r->set("ClosedDate", FieldValue::null());
    if (status == "Escalated") r->set("IsEscalated", FieldValue::boolean(true));

    const std::string priority = r->text("Priority");
    const bool priority_changed = before == nullptr || before->text("Priority") != priority;
    if (priority_changed && !closed) {
        auto it = org.settings.hours.sla_hours.find(priority);
        if (it != org.settings.hours.sla_hours.end()) {
            const FieldValue& created = before ? before->get("CreatedDate") : FieldValue();
            const int64_t start =
                created.kind() == ValueKind::DateTime ? created.as_datetime() : timeutil::now_ms();
            r->set("SlaDueDate", FieldValue::datetime(add_business_hours(start, it->second, org.settings.hours)));
        }
    }
}

void event(Record* r) {
    const FieldValue& start = r->get("StartDateTime");
    const FieldValue& end = r->get("EndDateTime");
    if (start.kind() != ValueKind::DateTime || end.kind() != ValueKind::DateTime) return;
    if (end.as_datetime() < start.as_datetime()) {
        throw errors::bad_request("FIELD_INTEGRITY_EXCEPTION", "End must be after Start", {"EndDateTime"});
    }
    r->set("DurationInMinutes",
           FieldValue::number(static_cast<double>((end.as_datetime() - start.as_datetime()) / timeutil::kMsPerMinute)));
}

void line_item(const Org& org, Record* r) {
    const FieldValue& product = r->get("Product2Id");
    if (product.kind() != ValueKind::String) return;
    std::optional<Record> p = load_record(org, product.as_string());
    if (p) r->set("Name", FieldValue::string(p->text("Name")));
}

}  // namespace

bool stage_defaults(const std::string& stage, int* probability, std::string* forecast, bool* closed, bool* won) {
    for (const StageInfo& s : kStages) {
        if (stage == s.name) {
            *probability = s.probability;
            *forecast = s.forecast;
            *closed = s.closed;
            *won = s.won;
            return true;
        }
    }
    return false;
}

void apply_standard_behavior(const Org& org, const meta::ObjectDef& obj, Record* r, const Record* before,
                             const json::Value& requested_fields) {
    if (obj.name == "Opportunity") {
        opportunity(r, before, requested_fields);
    } else if (obj.name == "Case") {
        case_record(org, r, before);
    } else if (obj.name == "Task") {
        r->set("IsClosed", FieldValue::boolean(r->text("Status") == "Completed"));
    } else if (obj.name == "Event") {
        event(r);
    } else if (obj.name == "OpportunityLineItem") {
        line_item(org, r);
    } else if (obj.name == "Lead") {
        if (before != nullptr && before->get("IsConverted").kind() == ValueKind::Bool &&
            before->get("IsConverted").as_bool()) {
            throw errors::bad_request("CANNOT_UPDATE_CONVERTED_LEAD", "cannot reference converted lead");
        }
    }
}

int64_t add_business_hours(int64_t start_ms, int hours, const BusinessHours& bh) {
    std::time_t t = static_cast<std::time_t>(start_ms / 1000);
    int remaining = hours * 60;
    while (remaining > 0) {
        const std::tm* local = std::localtime(&t);
        const bool workday = (bh.workdays >> local->tm_wday) & 1u;
        const int minute = local->tm_hour * 60 + local->tm_min;
        if (workday && minute >= bh.start_minute && minute < bh.end_minute) --remaining;
        t += 60;
    }
    return static_cast<int64_t>(t) * 1000;
}

double convert_currency(const OrgSettings& settings, double amount, const std::string& from, const std::string& to) {
    if (from == to) return amount;
    auto rate_of = [&settings](const std::string& code) {
        if (code == settings.currency) return 1.0;
        auto it = settings.currency_rates.find(code);
        return it == settings.currency_rates.end() ? 0.0 : it->second;
    };
    // Rates are units of the currency per one unit of the corporate currency.
    const double in_corporate = amount / rate_of(from);
    return in_corporate * rate_of(to);
}

}  // namespace tcrm::engine
