// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "engine/access.hpp"
#include "engine/org.hpp"
#include "json/json.hpp"
#include "model/record.hpp"

namespace tcrm::engine {

/// API paths.
inline constexpr const char* kApiBase = "/services/data/v1";

/// {"type": "Account", "url": "/services/data/v1/sobjects/Account/001..."}
json::Value record_attributes(const std::string& object, const std::string& id);

/// The read-only User record for an org user.
Record user_record(const auth::User& u, const Org& org);

/// Finds any record by id, using the key prefix to pick the object.
/// Users are included. Returns a copy. The caller holds the org lock.
std::optional<Record> load_record(const Org& org, const std::string& id, const meta::ObjectDef** object_out = nullptr);

/// The object a record id belongs to, or nullptr.
const meta::ObjectDef* object_for_id(const Org& org, const std::string& id);

/// Serializes a record for the REST API, dropping fields the principal
/// can't read. `fields` limits the output; nullptr means every field.
json::Value record_to_api_json(const meta::ObjectDef& obj, const Record& r, const AccessChecker& access,
                               const std::vector<std::string>* fields = nullptr);

/// The value of the object's name field (Name, CaseNumber, Subject...).
std::string record_name(const meta::ObjectDef& obj, const Record& r);

}  // namespace tcrm::engine
