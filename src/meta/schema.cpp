// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "meta/schema.hpp"

#include <algorithm>

#include "model/errors.hpp"
#include "util/id.hpp"
#include "util/strutil.hpp"

namespace tcrm::meta {
namespace {

constexpr size_t kMaxCustomNameLength = 40;
constexpr size_t kMaxCustomFieldsPerObject = 500;

bool valid_custom_name(const std::string& name) {
    if (name.size() < 4 || name.size() > kMaxCustomNameLength) return false;
    if (!str::iends_with(name, "__c")) return false;
    const std::string base = name.substr(0, name.size() - 3);
    if (!str::is_identifier(base)) return false;
    if (base.find("__") != std::string::npos) return false;
    if (base.back() == '_') return false;
    return true;
}

const char* delete_rule_name(DeleteRule r) {
    switch (r) {
        case DeleteRule::SetNull: return "SetNull";
        case DeleteRule::Restrict: return "Restrict";
        case DeleteRule::Cascade: return "Cascade";
    }
    return "SetNull";
}

DeleteRule parse_delete_rule(const std::string& s) {
    if (str::iequals(s, "Restrict")) return DeleteRule::Restrict;
    if (str::iequals(s, "Cascade")) return DeleteRule::Cascade;
    return DeleteRule::SetNull;
}

}  // namespace

Schema Schema::standard() {
    Schema s;
    for (ObjectDef& o : standard_objects()) {
        const std::string key = str::to_lower(o.name);
        s.objects_.emplace(key, std::move(o));
    }
    s.rebuild_relationships();
    return s;
}

const ObjectDef* Schema::object(std::string_view name) const {
    auto it = objects_.find(str::to_lower(name));
    return it == objects_.end() ? nullptr : &it->second;
}

ObjectDef* Schema::object(std::string_view name) {
    auto it = objects_.find(str::to_lower(name));
    return it == objects_.end() ? nullptr : &it->second;
}

const ObjectDef* Schema::object_by_prefix(std::string_view key_prefix) const {
    for (const auto& [key, o] : objects_) {
        if (o.key_prefix == key_prefix) return &o;
    }
    return nullptr;
}

std::vector<const ObjectDef*> Schema::objects() const {
    std::vector<const ObjectDef*> out;
    out.reserve(objects_.size());
    for (const auto& [key, o] : objects_) out.push_back(&o);
    std::sort(out.begin(), out.end(), [](const ObjectDef* a, const ObjectDef* b) { return a->name < b->name; });
    return out;
}

const ObjectDef& Schema::create_custom_object(const std::string& name, const std::string& label,
                                              const std::string& plural_label, const std::string& description) {
    if (!valid_custom_name(name)) {
        throw errors::bad_request("INVALID_NAME",
                                  "Custom object names must be identifiers ending in __c, at most 40 characters");
    }
    if (object(name) != nullptr) throw errors::bad_request("DUPLICATE_DEVELOPER_NAME", "Object " + name + " exists");
    ObjectDef o;
    o.name = name;
    o.label = label.empty() ? name.substr(0, name.size() - 3) : label;
    o.plural_label = plural_label.empty() ? o.label + "s" : plural_label;
    o.description = description;
    o.custom = true;
    o.track_history = false;
    o.key_prefix = "a" + ids::encode_base62(static_cast<uint64_t>(next_custom_prefix_++), 2);
    add_system_fields(&o, true);
    FieldDef nf;
    nf.name = "Name";
    nf.label = o.label + " Name";
    nf.type = FieldType::Text;
    nf.length = 80;
    nf.required = true;
    nf.name_field = true;
    nf.searchable = true;
    o.add_field(std::move(nf));
    const std::string key = str::to_lower(name);
    auto [it, inserted] = objects_.emplace(key, std::move(o));
    (void)inserted;
    ++version_;
    return it->second;
}

const FieldDef& Schema::add_custom_field(const std::string& object_name, FieldDef field) {
    ObjectDef* obj = object(object_name);
    if (obj == nullptr) throw errors::not_found("No such object: " + object_name);
    if (obj->name == "User") throw errors::bad_request("CANNOT_MODIFY", "User fields are managed by the platform");
    if (!valid_custom_name(field.name)) {
        throw errors::bad_request("INVALID_NAME",
                                  "Custom field names must be identifiers ending in __c, at most 40 characters");
    }
    for (const FieldDef& existing : obj->fields) {
        if (existing.name == field.name) {
            throw errors::bad_request("DUPLICATE_DEVELOPER_NAME", "Field " + field.name + " already exists");
        }
    }
    if (obj->fields.size() >= kMaxCustomFieldsPerObject) {
        throw errors::bad_request("LIMIT_EXCEEDED", "Too many fields on " + obj->name);
    }
    if (field.label.empty()) field.label = field.name.substr(0, field.name.size() - 3);
    field.custom = true;
    field.system = false;

    switch (field.type) {
        case FieldType::Text:
            if (field.length < 1 || field.length > 255) throw errors::invalid_field("length", "Text length must be 1-255");
            break;
        case FieldType::TextArea:
            if (field.length < 1 || field.length > 255) field.length = 255;
            break;
        case FieldType::LongTextArea:
            if (field.length < 256 || field.length > 131072) field.length = 32768;
            break;
        case FieldType::Number:
        case FieldType::Currency:
        case FieldType::Percent:
            if (field.precision < 1 || field.precision > 18 || field.scale < 0 || field.scale > field.precision) {
                throw errors::invalid_field("precision", "precision must be 1-18 and scale 0-precision");
            }
            break;
        case FieldType::Picklist:
        case FieldType::MultiPicklist:
            if (field.picklist.empty()) throw errors::invalid_field("picklist", "A picklist needs at least one value");
            break;
        case FieldType::Lookup:
        case FieldType::MasterDetail: {
            const ObjectDef* target = object(field.reference_to);
            if (target == nullptr) throw errors::invalid_field("referenceTo", "No such object: " + field.reference_to);
            field.reference_to = target->name;
            if (field.relationship_name.empty()) {
                field.relationship_name = field.name.substr(0, field.name.size() - 3) + "__r";
            }
            if (field.type == FieldType::MasterDetail) {
                if (!obj->custom) {
                    throw errors::bad_request("INVALID_TYPE", "Master-detail fields can only be added to custom objects");
                }
                field.required = true;
                field.delete_rule = DeleteRule::Cascade;
            }
            field.indexed = true;
            field.length = 18;
            break;
        }
        case FieldType::Formula:
            if (field.formula.empty()) throw errors::invalid_field("formula", "Formula fields need an expression");
            break;
        case FieldType::AutoNumber:
            if (field.autonumber_format.find('{') == std::string::npos) {
                throw errors::invalid_field("displayFormat", "Auto-number formats need a {0000} placeholder");
            }
            break;
        case FieldType::RollupSummary: {
            const ObjectDef* child = object(field.rollup.child_object);
            if (child == nullptr) throw errors::invalid_field("summarizedObject", "No such object");
            const FieldDef* via = child->field(field.rollup.relationship_field);
            if (via == nullptr || !is_reference_type(via->type) || !str::iequals(via->reference_to, obj->name)) {
                throw errors::invalid_field("summaryForeignKey", "The child field must reference " + obj->name);
            }
            const std::string op = str::to_upper(field.rollup.op);
            if (op != "COUNT" && op != "SUM" && op != "MIN" && op != "MAX") {
                throw errors::invalid_field("summaryOperation", "Use COUNT, SUM, MIN or MAX");
            }
            field.rollup.op = op;
            if (op != "COUNT") {
                const FieldDef* agg = child->field(field.rollup.field);
                if (agg == nullptr) throw errors::invalid_field("summarizedField", "No such field on the child");
                field.rollup.field = agg->name;
            }
            field.rollup.child_object = child->name;
            field.rollup.relationship_field = via->name;
            break;
        }
        case FieldType::Id:
            throw errors::bad_request("INVALID_TYPE", "Id fields can't be created");
        default:
            break;
    }
    if (field.unique || field.external_id) field.indexed = true;
    obj->add_field(std::move(field));
    rebuild_relationships();
    ++version_;
    return obj->fields.back();
}

void Schema::remove_custom_field(const std::string& object_name, const std::string& field_name) {
    ObjectDef* obj = object(object_name);
    if (obj == nullptr) throw errors::not_found("No such object: " + object_name);
    auto it = std::find_if(obj->fields.begin(), obj->fields.end(),
                           [&](const FieldDef& f) { return str::iequals(f.name, field_name); });
    if (it == obj->fields.end()) throw errors::not_found("No such field: " + field_name);
    if (!it->custom) throw errors::bad_request("CANNOT_DELETE", "Standard fields can't be deleted");
    obj->fields.erase(it);
    obj->rebuild_index();
    rebuild_relationships();
    ++version_;
}

const FieldDef& Schema::update_field(const std::string& object_name, const std::string& field_name,
                                     const json::Value& changes) {
    ObjectDef* obj = object(object_name);
    if (obj == nullptr) throw errors::not_found("No such object: " + object_name);
    FieldDef* f = obj->field(field_name);
    if (f == nullptr) throw errors::not_found("No such field: " + field_name);
    if (f->system) throw errors::bad_request("CANNOT_MODIFY", "System fields can't be changed");
    FieldDef updated = *f;
    for (const auto& [key, v] : changes.as_object()) {
        if (key == "label" && v.is_string() && !v.as_string().empty()) {
            updated.label = v.as_string();
        } else if (key == "inlineHelpText" && v.is_string()) {
            updated.help_text = v.as_string();
        } else if (key == "description" && v.is_string()) {
            updated.description = v.as_string();
        } else if (key == "picklistValues" && v.is_array()) {
            if (updated.type != FieldType::Picklist && updated.type != FieldType::MultiPicklist) {
                throw errors::invalid_field("picklistValues", "Only picklists have values");
            }
            updated.picklist = field_from_json(changes).picklist;
            if (updated.picklist.empty()) throw errors::invalid_field("picklistValues", "A picklist needs at least one value");
        } else if (!f->custom) {
            throw errors::bad_request("CANNOT_MODIFY", "Only the label, help text and picklist values of standard fields "
                                                       "can be changed");
        } else if (key == "required" && v.is_bool()) {
            updated.required = v.as_bool();
        } else if (key == "unique" && v.is_bool()) {
            updated.unique = v.as_bool();
            if (updated.unique) updated.indexed = true;
        } else if (key == "externalId" && v.is_bool()) {
            updated.external_id = v.as_bool();
            if (updated.external_id) updated.indexed = true;
        } else if (key == "historyTracked" && v.is_bool()) {
            updated.history_tracked = v.as_bool();
        } else if (key == "length" && v.is_number()) {
            const int length = static_cast<int>(v.as_int());
            if (!is_text_type(updated.type) || length < 1 || length > (updated.type == FieldType::LongTextArea ? 131072 : 255)) {
                throw errors::invalid_field("length", "Invalid length for a " + std::string(field_type_name(updated.type)) +
                                                          " field");
            }
            updated.length = length;
        } else if (key == "restrictedPicklist" && v.is_bool()) {
            updated.restricted_picklist = v.as_bool();
        } else if (key == "defaultValue" && v.is_string()) {
            updated.default_value = v.as_string();
        } else {
            throw errors::invalid_field(key, "Field attribute " + key + " can't be changed");
        }
    }
    *f = std::move(updated);
    ++version_;
    return *f;
}

void Schema::rebuild_relationships() {
    for (auto& [key, o] : objects_) o.children.clear();
    for (auto& [key, child] : objects_) {
        for (const FieldDef& f : child.fields) {
            if (!is_reference_type(f.type) || f.system || f.name == "OwnerId") continue;
            for (const std::string& target : str::split_trimmed(f.reference_to, ',')) {
                ObjectDef* parent = object(target);
                if (parent == nullptr) continue;
                ChildRelationship rel;
                rel.child_object = child.name;
                rel.field = f.name;
                std::string plural = child.plural_label;
                plural.erase(std::remove(plural.begin(), plural.end(), ' '), plural.end());
                rel.relationship_name = child.custom ? child.name.substr(0, child.name.size() - 3) + "s__r" : plural;
                rel.delete_rule = f.delete_rule;
                parent->children.push_back(std::move(rel));
            }
        }
    }
}

json::Value Schema::field_to_json(const FieldDef& f) {
    json::Value v = json::Value::object();
    v.set("name", f.name);
    v.set("label", f.label);
    v.set("type", field_type_name(f.type));
    v.set("length", f.length);
    v.set("precision", f.precision);
    v.set("scale", f.scale);
    v.set("required", f.required);
    v.set("unique", f.unique);
    v.set("externalId", f.external_id);
    v.set("indexed", f.indexed);
    v.set("historyTracked", f.history_tracked);
    v.set("searchable", f.searchable);
    if (!f.default_value.empty()) v.set("defaultValue", f.default_value);
    if (!f.description.empty()) v.set("description", f.description);
    if (!f.help_text.empty()) v.set("inlineHelpText", f.help_text);
    if (!f.picklist.empty()) {
        json::Value pl = json::Value::array();
        for (const PicklistValue& p : f.picklist) {
            json::Value pv = json::Value::object();
            pv.set("value", p.value);
            pv.set("label", p.label);
            pv.set("default", p.is_default);
            pv.set("active", p.active);
            pl.push_back(std::move(pv));
        }
        v.set("picklistValues", std::move(pl));
        v.set("restrictedPicklist", f.restricted_picklist);
    }
    if (is_reference_type(f.type)) {
        v.set("referenceTo", f.reference_to);
        v.set("relationshipName", f.relationship_name);
        v.set("deleteRule", delete_rule_name(f.delete_rule));
    }
    if (f.type == FieldType::Formula) {
        v.set("formula", f.formula);
        v.set("returnType", field_type_name(f.formula_return));
    }
    if (f.type == FieldType::AutoNumber) v.set("displayFormat", f.autonumber_format);
    if (f.type == FieldType::RollupSummary) {
        v.set("summarizedObject", f.rollup.child_object);
        v.set("summaryForeignKey", f.rollup.relationship_field);
        v.set("summaryOperation", f.rollup.op);
        v.set("summarizedField", f.rollup.field);
    }
    return v;
}

FieldDef Schema::field_from_json(const json::Value& v) {
    FieldDef f;
    f.name = v.get_string("name");
    f.label = v.get_string("label");
    if (!parse_field_type(v.get_string("type", "string"), &f.type)) {
        throw errors::invalid_field("type", "Unknown field type: " + v.get_string("type"));
    }
    f.length = static_cast<int>(v.get_int("length", 255));
    f.precision = static_cast<int>(v.get_int("precision", 18));
    f.scale = static_cast<int>(v.get_int("scale", 2));
    f.required = v.get_bool("required");
    f.unique = v.get_bool("unique");
    f.external_id = v.get_bool("externalId");
    f.indexed = v.get_bool("indexed");
    f.history_tracked = v.get_bool("historyTracked");
    f.searchable = v.get_bool("searchable");
    f.default_value = v.get_string("defaultValue");
    f.description = v.get_string("description");
    f.help_text = v.get_string("inlineHelpText");
    if (const json::Value* pl = v.find("picklistValues"); pl && pl->is_array()) {
        for (const json::Value& pv : pl->as_array()) {
            PicklistValue p;
            if (pv.is_string()) {
                p.value = pv.as_string();
                p.label = p.value;
            } else {
                p.value = pv.get_string("value");
                p.label = pv.get_string("label", p.value);
                p.is_default = pv.get_bool("default");
                p.active = pv.get_bool("active", true);
            }
            if (!p.value.empty()) f.picklist.push_back(std::move(p));
        }
    }
    f.restricted_picklist = v.get_bool("restrictedPicklist");
    f.reference_to = v.get_string("referenceTo");
    f.relationship_name = v.get_string("relationshipName");
    f.delete_rule = parse_delete_rule(v.get_string("deleteRule"));
    f.formula = v.get_string("formula");
    if (v.contains("returnType")) parse_field_type(v.get_string("returnType"), &f.formula_return);
    f.autonumber_format = v.get_string("displayFormat");
    f.rollup.child_object = v.get_string("summarizedObject");
    f.rollup.relationship_field = v.get_string("summaryForeignKey");
    f.rollup.op = v.get_string("summaryOperation");
    f.rollup.field = v.get_string("summarizedField");
    return f;
}

json::Value Schema::custom_to_json() const {
    json::Value doc = json::Value::object();
    doc.set("nextCustomPrefix", next_custom_prefix_);
    json::Value objs = json::Value::array();
    for (const auto& [key, o] : objects_) {
        const bool has_custom_fields =
            std::any_of(o.fields.begin(), o.fields.end(), [](const FieldDef& f) { return f.custom; });
        if (!o.custom && !has_custom_fields) continue;
        json::Value jo = json::Value::object();
        jo.set("name", o.name);
        jo.set("custom", o.custom);
        if (o.custom) {
            jo.set("label", o.label);
            jo.set("pluralLabel", o.plural_label);
            jo.set("keyPrefix", o.key_prefix);
            jo.set("description", o.description);
        }
        json::Value fields = json::Value::array();
        for (const FieldDef& f : o.fields) {
            if (f.custom) fields.push_back(field_to_json(f));
        }
        jo.set("fields", std::move(fields));
        objs.push_back(std::move(jo));
    }
    doc.set("objects", std::move(objs));
    return doc;
}

void Schema::load_custom_json(const json::Value& doc) {
    next_custom_prefix_ = static_cast<int>(doc.get_int("nextCustomPrefix", next_custom_prefix_));
    const json::Value& objs = doc.at("objects");
    if (!objs.is_array()) return;
    // Objects first so lookups between custom objects resolve.
    for (const json::Value& jo : objs.as_array()) {
        if (!jo.get_bool("custom")) continue;
        ObjectDef o;
        o.name = jo.get_string("name");
        o.label = jo.get_string("label", o.name);
        o.plural_label = jo.get_string("pluralLabel", o.label);
        o.key_prefix = jo.get_string("keyPrefix");
        o.description = jo.get_string("description");
        o.custom = true;
        add_system_fields(&o, true);
        FieldDef nf;
        nf.name = "Name";
        nf.label = o.label + " Name";
        nf.length = 80;
        nf.required = true;
        nf.name_field = true;
        nf.searchable = true;
        o.add_field(std::move(nf));
        objects_[str::to_lower(o.name)] = std::move(o);
    }
    for (const json::Value& jo : objs.as_array()) {
        ObjectDef* o = object(jo.get_string("name"));
        if (o == nullptr) continue;
        const json::Value& fields = jo.at("fields");
        if (!fields.is_array()) continue;
        for (const json::Value& jf : fields.as_array()) {
            FieldDef f = field_from_json(jf);
            f.custom = true;
            if (o->field(f.name) == nullptr) o->add_field(std::move(f));
        }
    }
    rebuild_relationships();
    ++version_;
}

}  // namespace tcrm::meta
