// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "meta/codec.hpp"

#include <algorithm>
#include <cmath>
#include <regex>
#include <string>

#include "model/errors.hpp"
#include "util/id.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::meta {
namespace {

ApiError type_error(const FieldDef& f, const std::string& expected) {
    return errors::bad_request("INVALID_FIELD", f.name + ": expected " + expected, {f.name});
}

void check_length(const FieldDef& f, const std::string& s) {
    if (static_cast<int>(s.size()) > f.length) {
        throw errors::bad_request("STRING_TOO_LONG",
                                  f.label + ": data value too large (max length=" + std::to_string(f.length) + ")",
                                  {f.name});
    }
}

FieldValue picklist_value(const FieldDef& f, const std::string& s) {
    if (s.empty()) return FieldValue::null();
    if (f.restricted_picklist) {
        const PicklistValue* p = f.find_picklist_value(s);
        if (p == nullptr || !p->active) {
            throw errors::bad_request("INVALID_OR_NULL_FOR_RESTRICTED_PICKLIST",
                                      f.label + ": bad value for restricted picklist field: " + s, {f.name});
        }
    }
    check_length(f, s);
    return FieldValue::string(s);
}

FieldValue multipicklist_value(const FieldDef& f, const std::vector<std::string>& items) {
    std::vector<std::string> clean;
    for (const std::string& item : items) {
        std::string t(str::trim(item));
        if (t.empty()) continue;
        if (f.restricted_picklist) {
            const PicklistValue* p = f.find_picklist_value(t);
            if (p == nullptr || !p->active) {
                throw errors::bad_request("INVALID_OR_NULL_FOR_RESTRICTED_PICKLIST",
                                          f.label + ": bad value for restricted picklist field: " + t, {f.name});
            }
        }
        if (std::find(clean.begin(), clean.end(), t) == clean.end()) clean.push_back(std::move(t));
    }
    if (clean.empty()) return FieldValue::null();
    return FieldValue::string(str::join(clean, ";"));
}

FieldValue reference_value(const FieldDef& f, const std::string& s) {
    if (s.empty()) return FieldValue::null();
    std::string id18;
    if (!ids::normalize(s, &id18)) {
        throw errors::bad_request("MALFORMED_ID", f.label + ": id value of incorrect type: " + s, {f.name});
    }
    return FieldValue::string(id18);
}

FieldValue number_value(const FieldDef& f, double d) {
    return FieldValue::number(check_precision(f, d));
}

}  // namespace

bool is_valid_email(std::string_view s) {
    static const std::regex kEmail(R"(^([a-zA-Z0-9_\.\-\+])+\@(([a-zA-Z0-9\-])+\.)+([a-zA-Z0-9]{2,6})+$)");
    return std::regex_match(s.begin(), s.end(), kEmail);
}

double check_precision(const FieldDef& f, double v) {
    if (!std::isfinite(v)) throw type_error(f, "a finite number");
    const double factor = std::pow(10.0, f.scale);
    const double rounded = std::round(v * factor) / factor;
    const double limit = std::pow(10.0, f.precision - f.scale);
    if (std::fabs(rounded) >= limit) {
        throw errors::bad_request("NUMBER_OUTSIDE_VALID_RANGE",
                                  f.label + ": value outside of valid range on numeric field", {f.name});
    }
    return rounded;
}

FieldValue value_from_json(const FieldDef& f, const json::Value& v) {
    if (v.is_null()) return FieldValue::null();
    switch (f.value_type()) {
        case FieldType::Text:
        case FieldType::TextArea:
        case FieldType::LongTextArea:
        case FieldType::Phone:
        case FieldType::Url: {
            if (!v.is_string()) throw type_error(f, "a string");
            const std::string& s = v.as_string();
            check_length(f, s);
            return s.empty() ? FieldValue::null() : FieldValue::string(s);
        }
        case FieldType::Email: {
            if (!v.is_string()) throw type_error(f, "a string");
            const std::string s(str::trim(v.as_string()));
            if (s.empty()) return FieldValue::null();
            check_length(f, s);
            if (!is_valid_email(s)) {
                throw errors::bad_request("INVALID_EMAIL_ADDRESS", f.label + ": invalid email address: " + s, {f.name});
            }
            return FieldValue::string(s);
        }
        case FieldType::Number:
        case FieldType::Currency:
        case FieldType::Percent: {
            if (v.is_number()) return number_value(f, v.as_double());
            if (v.is_string()) {
                double d = 0;
                if (!str::parse_double(str::trim(v.as_string()), &d)) throw type_error(f, "a number");
                return number_value(f, d);
            }
            throw type_error(f, "a number");
        }
        case FieldType::Checkbox:
            if (v.is_bool()) return FieldValue::boolean(v.as_bool());
            if (v.is_string() && (v.as_string() == "true" || v.as_string() == "false")) {
                return FieldValue::boolean(v.as_string() == "true");
            }
            throw type_error(f, "true or false");
        case FieldType::Date: {
            if (!v.is_string()) throw type_error(f, "a date (YYYY-MM-DD)");
            int64_t days = 0;
            if (!timeutil::parse_date(v.as_string(), &days)) throw type_error(f, "a date (YYYY-MM-DD)");
            return FieldValue::date(days);
        }
        case FieldType::DateTime: {
            if (!v.is_string()) throw type_error(f, "an ISO 8601 date-time");
            int64_t ms = 0;
            if (!timeutil::parse_datetime(v.as_string(), &ms)) throw type_error(f, "an ISO 8601 date-time");
            return FieldValue::datetime(ms);
        }
        case FieldType::Picklist:
            if (!v.is_string()) throw type_error(f, "a string");
            return picklist_value(f, v.as_string());
        case FieldType::MultiPicklist: {
            if (v.is_string()) return multipicklist_value(f, str::split(v.as_string(), ';'));
            if (v.is_array()) {
                std::vector<std::string> items;
                for (const json::Value& item : v.as_array()) {
                    if (!item.is_string()) throw type_error(f, "an array of strings");
                    items.push_back(item.as_string());
                }
                return multipicklist_value(f, items);
            }
            throw type_error(f, "a string or array of strings");
        }
        case FieldType::Id:
        case FieldType::Lookup:
        case FieldType::MasterDetail:
            if (!v.is_string()) throw type_error(f, "an id");
            return reference_value(f, std::string(str::trim(v.as_string())));
        default:
            throw type_error(f, "a supported value");
    }
}

FieldValue value_from_text(const FieldDef& f, std::string_view text) {
    if (text.empty()) return FieldValue::null();
    switch (f.value_type()) {
        case FieldType::Number:
        case FieldType::Currency:
        case FieldType::Percent: {
            std::string t(str::trim(text));
            if (!t.empty() && t.back() == '%') t.pop_back();
            try {
                return number_value(f, std::stod(t));
            } catch (const std::invalid_argument&) {
                throw type_error(f, "a number");
            } catch (const std::out_of_range&) {
                throw type_error(f, "a number");
            }
        }
        case FieldType::Checkbox: {
            const std::string t = str::to_lower(str::trim(text));
            if (t == "true" || t == "1" || t == "yes") return FieldValue::boolean(true);
            if (t == "false" || t == "0" || t == "no") return FieldValue::boolean(false);
            throw type_error(f, "true or false");
        }
        default:
            return value_from_json(f, json::Value(std::string(text)));
    }
}

json::Value value_to_json(const FieldDef& f, const FieldValue& v) {
    switch (v.kind()) {
        case ValueKind::Null: return json::Value();
        case ValueKind::Bool: return json::Value(v.as_bool());
        case ValueKind::Int: return json::Value(v.as_int());
        case ValueKind::Double: {
            const double d = v.as_double();
            if (f.scale == 0 && d == std::floor(d) && std::fabs(d) < 9e15) return json::Value(static_cast<int64_t>(d));
            return json::Value(d);
        }
        case ValueKind::String: return json::Value(v.as_string());
        case ValueKind::Date: return json::Value(timeutil::format_date(v.as_date()));
        case ValueKind::DateTime: return json::Value(timeutil::format_datetime(v.as_datetime()));
    }
    return json::Value();
}

}  // namespace tcrm::meta
