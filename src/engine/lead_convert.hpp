// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>

#include "engine/record_service.hpp"

namespace tcrm::engine {

struct LeadConvertRequest {
    std::string lead_id;
    std::string account_id;  // existing account to attach to; empty = match or create
    std::string contact_id;  // existing contact to merge into; empty = create
    bool create_opportunity = true;
    std::string opportunity_name;
    std::string converted_status = "Closed - Converted";
    std::string owner_id;  // owner for the new records; empty = the lead's owner
};

struct LeadConvertResult {
    std::string lead_id;
    std::string account_id;
    std::string contact_id;
    std::string opportunity_id;
    bool account_created = false;
};

/// Converts a lead into an account, a contact and (optionally) an
/// opportunity, then marks the lead converted.
LeadConvertResult convert_lead(RecordService& records, const Principal& who, const LeadConvertRequest& request);

}  // namespace tcrm::engine
