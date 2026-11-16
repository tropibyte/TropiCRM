// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <vector>

#include "engine/access.hpp"
#include "json/json.hpp"

namespace tcrm::services {

/// An email template with merge fields such as {!Name}, {!Account.Name} or
/// {!Owner.Email}. Merge fields may give a fallback: {!Title, "there"}.
struct EmailTemplate {
    std::string id;
    std::string name;
    std::string object;
    std::string subject;
    std::string body;
    std::string owner_id;
    bool is_public = true;
    int64_t created_at = 0;

    json::Value to_json() const;
    static EmailTemplate from_json(const json::Value& v);
};

std::vector<EmailTemplate> load_templates(const engine::Org& org);
void store_templates(engine::Org& org, const std::vector<EmailTemplate>& templates);

/// Checks object and merge-field syntax. Throws ApiError.
void validate_template(const engine::Org& org, EmailTemplate* t);

/// The merge fields a template uses, e.g. {"Name", "Account.Name"}.
std::vector<std::string> merge_fields(const std::string& text);

struct Rendered {
    std::string subject;
    std::string body;
};

/// Fills a template's merge fields from a record. The caller holds the org
/// lock and has checked that `who` can read the record.
Rendered render_template(const engine::Org& org, const engine::Principal& who, const EmailTemplate& t,
                         const meta::ObjectDef& obj, const Record& r);

}  // namespace tcrm::services
