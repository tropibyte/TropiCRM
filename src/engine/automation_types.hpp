// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <vector>

#include "json/json.hpp"

namespace tcrm::engine {

/// Blocks a save when `condition` evaluates to true.
struct ValidationRule {
    std::string id;
    std::string name;
    std::string object;
    std::string condition;
    std::string error_message;
    std::string error_field;  // optional: report the error against this field
    bool active = true;
};

struct WorkflowAction {
    enum class Type { FieldUpdate, CreateTask, Webhook, EmailAlert };
    Type type = Type::FieldUpdate;
    std::string field;          // FieldUpdate: field to set
    std::string value_formula;  // FieldUpdate: new value (a formula)
    std::string subject;        // CreateTask / EmailAlert
    int due_in_days = 0;        // CreateTask
    std::string webhook_id;     // Webhook
    std::string recipient = "owner";  // CreateTask / EmailAlert: owner, creator or a user id
};

/// Runs `actions` after a save when `criteria` is true. With
/// `only_when_newly_true`, edits trigger only if the criteria was false
/// before the edit.
struct WorkflowRule {
    std::string id;
    std::string name;
    std::string object;
    std::string criteria;
    bool on_create = true;
    bool on_update = true;
    bool only_when_newly_true = false;
    std::vector<WorkflowAction> actions;
    bool active = true;
};

/// Assigns new Leads or Cases that match `criteria` to the next user in
/// `user_ids`, round robin.
struct AssignmentRule {
    std::string id;
    std::string name;
    std::string object;
    std::string criteria;
    std::vector<std::string> user_ids;
    int sort_order = 0;
    bool active = true;
};

struct AutomationConfig {
    std::vector<ValidationRule> validation_rules;
    std::vector<WorkflowRule> workflow_rules;
    std::vector<AssignmentRule> assignment_rules;

    json::Value to_json() const;
    static AutomationConfig from_json(const json::Value& v);

    static json::Value validation_rule_to_json(const ValidationRule& r);
    static ValidationRule validation_rule_from_json(const json::Value& v);
    static json::Value workflow_rule_to_json(const WorkflowRule& r);
    static WorkflowRule workflow_rule_from_json(const json::Value& v);
    static json::Value assignment_rule_to_json(const AssignmentRule& r);
    static AssignmentRule assignment_rule_from_json(const json::Value& v);
};

}  // namespace tcrm::engine
