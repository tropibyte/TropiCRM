// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "api/context.hpp"
#include "engine/formula_context.hpp"
#include "formula/formula.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

void require_customize(const engine::Principal& who) {
    require(who.profile.customize_application || who.profile.modify_all_data, "customize the application");
}

void check_formula(const std::string& source, const std::string& field) {
    try {
        formula::Formula::compile(source);
    } catch (const formula::FormulaError& e) {
        throw errors::invalid_field(field, std::string("Formula syntax error: ") + e.what());
    }
}

/// Generic list/create/delete endpoints for rule collections.
template <typename Rule, typename ToJson, typename FromJson, typename Validate>
void rule_routes(http::Router& r, ApiContext& ctx, const std::string& path, const std::string& prefix,
                 std::vector<Rule> engine::AutomationConfig::*member, ToJson to_json, FromJson from_json,
                 Validate validate) {
    r.get(path, authed(ctx, [member, to_json](http::Request& req) {
              const engine::Principal& who = principal(req);
              require_customize(who);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value records = json::Value::array();
              for (const Rule& rule : who.org->automation.*member) records.push_back(to_json(rule));
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));
    r.post(path, authed(ctx, [member, to_json, from_json, validate, prefix](http::Request& req) {
               const engine::Principal& who = principal(req);
               require_customize(who);
               Rule rule = from_json(body_object(req));
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               validate(org, &rule);
               rule.id = new_config_id(org, prefix);
               (org.automation.*member).push_back(rule);
               org.save_automation();
               return json_response(201, to_json(rule));
           }));
    r.del(path + "/{id}", authed(ctx, [member](http::Request& req) {
              const engine::Principal& who = principal(req);
              require_customize(who);
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              auto& rules = org.automation.*member;
              const size_t before = rules.size();
              rules.erase(std::remove_if(rules.begin(), rules.end(),
                                         [&](const Rule& x) { return x.id == req.path_param("id"); }),
                          rules.end());
              if (rules.size() == before) throw errors::not_found();
              org.save_automation();
              return http::Response::no_content();
          }));
}

const meta::ObjectDef& require_object(const engine::Org& org, const std::string& name) {
    const meta::ObjectDef* obj = org.schema.object(name);
    if (obj == nullptr) throw errors::invalid_field("object", "Unknown object " + name);
    return *obj;
}

}  // namespace

void register_tooling_routes(http::Router& r, ApiContext& ctx) {
    const std::string base = "/services/data/v1/tooling";

    r.get(base + "/objects", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.view_setup || who.profile.customize_application, "view setup");
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value records = json::Value::array();
              for (const meta::ObjectDef* o : who.org->schema.objects()) {
                  json::Value v = json::Value::object();
                  v.set("name", o->name);
                  v.set("label", o->label);
                  v.set("custom", o->custom);
                  v.set("keyPrefix", o->key_prefix);
                  v.set("sharingModel", security::org_default_name(who.org->sharing.default_for(o->name)));
                  int custom_fields = 0;
                  for (const meta::FieldDef& f : o->fields) custom_fields += f.custom ? 1 : 0;
                  v.set("customFieldCount", custom_fields);
                  records.push_back(std::move(v));
              }
              json::Value out = json::Value::object();
              out.set("records", std::move(records));
              return json_response(200, out);
          }));

    r.post(base + "/objects", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               require_customize(who);
               const json::Value body = body_object(req);
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               const meta::ObjectDef& obj =
                   org.schema.create_custom_object(body.get_string("name"), body.get_string("label"),
                                                   body.get_string("pluralLabel"), body.get_string("description"));
               security::OrgDefault def = security::OrgDefault::Private;
               if (body.contains("sharingModel") && !security::parse_org_default(body.get_string("sharingModel"), &def)) {
                   throw errors::invalid_field("sharingModel", "Unknown sharing model");
               }
               org.sharing.defaults[obj.name] = def;
               storage::Table& t = org.db.table(obj.name);
               for (const meta::FieldDef& f : obj.fields) {
                   if (f.indexed) t.ensure_index(f.name, f.unique);
               }
               org.save_schema();
               org.save_sharing();
               audit(ctx, who, "Customize", "Created custom object", obj.name);
               json::Value v = json::Value::object();
               v.set("name", obj.name);
               v.set("keyPrefix", obj.key_prefix);
               v.set("success", true);
               return json_response(201, v);
           }));

    r.post(base + "/objects/{object}/fields", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               require_customize(who);
               const json::Value body = body_object(req);
               meta::FieldDef f = meta::Schema::field_from_json(body);
               if (f.type == meta::FieldType::Formula) check_formula(f.formula, "formula");
               if (!f.default_value.empty()) check_formula(f.default_value, "defaultValue");
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               const meta::FieldDef& added = org.schema.add_custom_field(req.path_param("object"), std::move(f));
               const meta::ObjectDef* obj = org.schema.object(req.path_param("object"));
               if (added.indexed || added.unique || added.external_id) {
                   org.db.table(obj->name).ensure_index(added.name, added.unique || added.external_id);
               }
               // Field-level security for the new field: visible and editable
               // for every profile unless the request says otherwise.
               if (const json::Value* fls = body.find("fieldPermissions"); fls && fls->is_object()) {
                   for (const auto& [profile_name, perms] : fls->as_object()) {
                       const security::Profile* p = org.profiles.find_by_name(profile_name);
                       if (p == nullptr) continue;
                       security::Profile copy = *p;
                       security::FieldPermission fp;
                       fp.read = perms.get_bool("read", true);
                       fp.edit = fp.read && perms.get_bool("edit", true);
                       copy.fields[obj->name + "." + added.name] = fp;
                       org.profiles.put(std::move(copy));
                   }
                   org.save_profiles();
               }
               org.save_schema();
               audit(ctx, who, "Customize", "Created custom field", obj->name + "." + added.name);
               json::Value v = meta::Schema::field_to_json(added);
               v.set("success", true);
               return json_response(201, v);
           }));

    r.patch(base + "/objects/{object}/fields/{field}", authed(ctx, [&ctx](http::Request& req) {
                const engine::Principal& who = principal(req);
                require_customize(who);
                const json::Value body = body_object(req);
                if (body.contains("defaultValue")) check_formula(body.get_string("defaultValue"), "defaultValue");
                engine::Org& org = *who.org;
                std::unique_lock<std::shared_mutex> lock(org.mutex());
                const meta::ObjectDef& obj = require_object(org, req.path_param("object"));
                const meta::FieldDef& f = org.schema.update_field(obj.name, req.path_param("field"), body);
                if (f.indexed || f.unique || f.external_id) {
                    storage::Table& t = org.db.table(obj.name);
                    t.drop_index(f.name);
                    t.ensure_index(f.name, f.unique || f.external_id);
                }
                org.save_schema();
                audit(ctx, who, "Customize", "Changed field", obj.name + "." + f.name);
                json::Value v = meta::Schema::field_to_json(f);
                v.set("success", true);
                return json_response(200, v);
            }));

    r.del(base + "/objects/{object}/fields/{field}", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              require_customize(who);
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              const meta::ObjectDef& obj = require_object(org, req.path_param("object"));
              const meta::FieldDef* f = obj.field(req.path_param("field"));
              if (f == nullptr) throw errors::not_found();
              const std::string name = f->name;
              const std::string object = obj.name;
              org.schema.remove_custom_field(object, name);
              org.db.table(object).drop_index(name);
              org.save_schema();
              audit(ctx, who, "Customize", "Deleted custom field", object + "." + name);
              return http::Response::no_content();
          }));

    rule_routes<engine::ValidationRule>(
        r, ctx, base + "/validationRules", "03d", &engine::AutomationConfig::validation_rules,
        engine::AutomationConfig::validation_rule_to_json, engine::AutomationConfig::validation_rule_from_json,
        [](const engine::Org& org, engine::ValidationRule* rule) {
            rule->object = require_object(org, rule->object).name;
            if (rule->name.empty()) throw errors::invalid_field("name", "Rule name is required");
            if (rule->error_message.empty()) throw errors::invalid_field("errorMessage", "An error message is required");
            check_formula(rule->condition, "errorConditionFormula");
        });

    rule_routes<engine::WorkflowRule>(
        r, ctx, base + "/workflowRules", "01Q", &engine::AutomationConfig::workflow_rules,
        engine::AutomationConfig::workflow_rule_to_json, engine::AutomationConfig::workflow_rule_from_json,
        [](const engine::Org& org, engine::WorkflowRule* rule) {
            const meta::ObjectDef& obj = require_object(org, rule->object);
            rule->object = obj.name;
            if (rule->name.empty()) throw errors::invalid_field("name", "Rule name is required");
            check_formula(rule->criteria, "criteria");
            for (const engine::WorkflowAction& a : rule->actions) {
                if (a.type == engine::WorkflowAction::Type::FieldUpdate) {
                    const meta::FieldDef* f = obj.field(a.field);
                    if (f == nullptr || !f->is_createable()) {
                        throw errors::invalid_field("actions", "Can't update field " + a.field);
                    }
                    check_formula(a.value_formula, "actions.value");
                }
            }
        });

    rule_routes<engine::AssignmentRule>(
        r, ctx, base + "/assignmentRules", "01Q", &engine::AutomationConfig::assignment_rules,
        engine::AutomationConfig::assignment_rule_to_json, engine::AutomationConfig::assignment_rule_from_json,
        [](const engine::Org& org, engine::AssignmentRule* rule) {
            rule->object = require_object(org, rule->object).name;
            if (rule->object != "Lead" && rule->object != "Case") {
                throw errors::invalid_field("object", "Assignment rules apply to Leads and Cases");
            }
            check_formula(rule->criteria, "criteria");
            for (const std::string& uid : rule->user_ids) {
                if (org.users.find(uid) == nullptr) throw errors::invalid_field("userIds", "No such user " + uid);
            }
        });

    // Profiles: object permissions and field-level security.
    r.get(base + "/profiles", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.view_setup || who.profile.customize_application || who.profile.manage_users,
                      "view profiles");
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value out = json::Value::object();
              out.set("records", who.org->profiles.to_json());
              return json_response(200, out);
          }));

    r.patch(base + "/profiles/{id}", authed(ctx, [&ctx](http::Request& req) {
                const engine::Principal& who = principal(req);
                require_customize(who);
                const json::Value body = body_object(req);
                engine::Org& org = *who.org;
                std::unique_lock<std::shared_mutex> lock(org.mutex());
                const security::Profile* existing = org.profiles.find(req.path_param("id"));
                if (existing == nullptr) throw errors::not_found();
                json::Value merged = existing->to_json();
                for (const auto& [k, v] : body.as_object()) {
                    if (k == "id") continue;
                    if (v.is_object() && merged.at(k).is_object()) {
                        json::Value& target = merged[k];
                        for (const auto& [ik, iv] : v.as_object()) target.set(ik, iv);
                    } else {
                        merged.set(k, v);
                    }
                }
                security::Profile updated = security::Profile::from_json(merged);
                updated.id = existing->id;
                if (updated.name == "System Administrator" && !updated.modify_all_data) {
                    throw errors::bad_request("CANNOT_MODIFY", "The System Administrator profile must keep Modify All Data");
                }
                org.profiles.put(updated);
                org.save_profiles();
                audit(ctx, who, "Manage Users", "Changed profile", updated.name);
                return json_response(200, updated.to_json());
            }));

    r.post(base + "/profiles", authed(ctx, [](http::Request& req) {
               const engine::Principal& who = principal(req);
               require_customize(who);
               const json::Value body = body_object(req);
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               const security::Profile* source = org.profiles.find_by_name(body.get_string("cloneFrom", "Standard User"));
               if (source == nullptr) throw errors::invalid_field("cloneFrom", "No such profile");
               security::Profile p = *source;
               p.id = new_config_id(org, "00e");
               p.name = body.get_string("name");
               if (p.name.empty() || org.profiles.find_by_name(p.name) != nullptr) {
                   throw errors::invalid_field("name", "Profile names must be unique and non-empty");
               }
               p.description = body.get_string("description");
               org.profiles.put(p);
               org.save_profiles();
               return json_response(201, p.to_json());
           }));

    // Roles.
    r.get(base + "/roles", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value out = json::Value::object();
              out.set("records", who.org->roles.to_json());
              return json_response(200, out);
          }));

    r.post(base + "/roles", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               require(who.profile.manage_users || who.profile.customize_application, "manage roles");
               const json::Value body = body_object(req);
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               security::Role role;
               role.id = new_config_id(org, "00E");
               role.name = body.get_string("name");
               role.parent_id = body.get_string("parentRoleId");
               if (role.name.empty()) throw errors::invalid_field("name", "Role name is required");
               org.roles.put(role);
               org.save_roles();
               audit(ctx, who, "Manage Users", "Created role", role.name);
               json::Value v = json::Value::object();
               v.set("id", role.id);
               v.set("name", role.name);
               v.set("parentRoleId", role.parent_id);
               return json_response(201, v);
           }));

    r.patch(base + "/roles/{id}", authed(ctx, [](http::Request& req) {
                const engine::Principal& who = principal(req);
                require(who.profile.manage_users || who.profile.customize_application, "manage roles");
                const json::Value body = body_object(req);
                engine::Org& org = *who.org;
                std::unique_lock<std::shared_mutex> lock(org.mutex());
                const security::Role* existing = org.roles.find(req.path_param("id"));
                if (existing == nullptr) throw errors::not_found();
                security::Role role = *existing;
                if (body.contains("name")) role.name = body.get_string("name");
                if (body.contains("parentRoleId")) role.parent_id = body.get_string("parentRoleId");
                org.roles.put(role);
                org.save_roles();
                return http::Response::no_content();
            }));

    r.del(base + "/roles/{id}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.manage_users || who.profile.customize_application, "manage roles");
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              for (const auth::User& u : org.users.all()) {
                  if (u.role_id == req.path_param("id")) {
                      throw errors::bad_request("DELETE_FAILED", "Users are still assigned to this role");
                  }
              }
              org.roles.remove(req.path_param("id"));
              org.save_roles();
              return http::Response::no_content();
          }));

    // Sharing settings.
    r.get(base + "/sharing", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.view_setup || who.profile.customize_application, "view sharing settings");
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              return json_response(200, who.org->sharing.to_json());
          }));

    r.put(base + "/sharing", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              require_customize(who);
              const json::Value body = body_object(req);
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              security::SharingModel model = security::SharingModel::from_json(body);
              for (security::SharingRule& rule : model.rules) {
                  const meta::ObjectDef& obj = require_object(org, rule.object);
                  rule.object = obj.name;
                  if (obj.field(rule.field) == nullptr) throw errors::invalid_field("field", "Unknown field " + rule.field);
                  if (org.roles.find(rule.role_id) == nullptr) throw errors::invalid_field("roleId", "Unknown role");
                  if (rule.id.empty()) rule.id = new_config_id(org, "02c");
              }
              // Manual shares are managed per record; keep the existing ones.
              model.manual = org.sharing.manual;
              org.sharing = std::move(model);
              org.save_sharing();
              audit(ctx, who, "Sharing", "Changed sharing settings");
              return json_response(200, org.sharing.to_json());
          }));

    // Org settings.
    r.get(base + "/settings", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.view_setup || who.profile.customize_application, "view settings");
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value v = who.org->settings.to_json();
              v.set("orgId", who.org->id());
              v.set("name", who.org->name());
              return json_response(200, v);
          }));

    r.patch(base + "/settings", authed(ctx, [&ctx](http::Request& req) {
                const engine::Principal& who = principal(req);
                require_customize(who);
                const json::Value body = body_object(req);
                engine::Org& org = *who.org;
                std::unique_lock<std::shared_mutex> lock(org.mutex());
                json::Value merged = org.settings.to_json();
                for (const auto& [k, v] : body.as_object()) merged.set(k, v);
                engine::OrgSettings updated = engine::OrgSettings::from_json(merged);
                timeutil::TimeZone tz;
                if (!timeutil::TimeZone::lookup(updated.timezone, &tz)) {
                    throw errors::invalid_field("timezone", "Unknown time zone " + updated.timezone);
                }
                if (updated.hours.start_minute < 0 || updated.hours.end_minute > 24 * 60) {
                    throw errors::invalid_field("businessHours", "Business hours must fall within one day");
                }
                for (const auto& [code, rate] : updated.currency_rates) {
                    if (rate <= 0) throw errors::invalid_field("currencyRates", "Rates must be positive: " + code);
                }
                org.settings = updated;
                org.save_settings();
                audit(ctx, who, "Company Profile", "Changed organization settings");
                if (body.contains("name")) org.set_name(body.get_string("name"));
                return json_response(200, org.settings.to_json());
            }));
}

}  // namespace tcrm::api
