// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/templates.hpp"

#include "engine/formula_context.hpp"
#include "model/errors.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::services {
namespace {

struct MergeField {
    size_t start = 0;  // offset of "{!"
    size_t end = 0;    // offset just past "}"
    std::string path;
    std::string fallback;
};

/// Finds {!Path} and {!Path, "fallback"} tokens. Unterminated tokens are
/// left as literal text.
std::vector<MergeField> scan(const std::string& text) {
    std::vector<MergeField> out;
    size_t pos = 0;
    while ((pos = text.find("{!", pos)) != std::string::npos) {
        const size_t close = text.find('}', pos + 2);
        if (close == std::string::npos) break;
        MergeField m;
        m.start = pos;
        m.end = close + 1;
        std::string inner = text.substr(pos + 2, close - pos - 2);
        const size_t comma = inner.find(',');
        if (comma != std::string::npos) {
            std::string fb(str::trim(std::string_view(inner).substr(comma + 1)));
            if (fb.size() >= 2 && fb.front() == '"' && fb.back() == '"') fb = fb.substr(1, fb.size() - 2);
            m.fallback = fb;
            inner = inner.substr(0, comma);
        }
        m.path = std::string(str::trim(inner));
        out.push_back(std::move(m));
        pos = close + 1;
    }
    return out;
}

std::string fill(const std::string& text, engine::RecordFormulaContext& ctx) {
    std::string out;
    size_t last = 0;
    for (const MergeField& m : scan(text)) {
        out.append(text, last, m.start - last);
        std::string value;
        try {
            value = ctx.field(m.path).to_text();
        } catch (const formula::FormulaError&) {
            value.clear();
        }
        out += value.empty() ? m.fallback : value;
        last = m.end;
    }
    out.append(text, last, std::string::npos);
    return out;
}

}  // namespace

json::Value EmailTemplate::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("object", object);
    v.set("subject", subject);
    v.set("body", body);
    v.set("ownerId", owner_id);
    v.set("public", is_public);
    v.set("createdDate", timeutil::format_datetime(created_at));
    return v;
}

EmailTemplate EmailTemplate::from_json(const json::Value& v) {
    EmailTemplate t;
    t.id = v.get_string("id");
    t.name = v.get_string("name");
    t.object = v.get_string("object");
    t.subject = v.get_string("subject");
    t.body = v.get_string("body");
    t.owner_id = v.get_string("ownerId");
    t.is_public = v.get_bool("public", true);
    timeutil::parse_datetime(v.get_string("createdDate"), &t.created_at);
    return t;
}

std::vector<EmailTemplate> load_templates(const engine::Org& org) {
    std::vector<EmailTemplate> out;
    if (const json::Value* v = org.db.meta("emailTemplates"); v && v->is_array()) {
        for (const json::Value& t : v->as_array()) out.push_back(EmailTemplate::from_json(t));
    }
    return out;
}

void store_templates(engine::Org& org, const std::vector<EmailTemplate>& templates) {
    json::Value arr = json::Value::array();
    for (const EmailTemplate& t : templates) arr.push_back(t.to_json());
    org.db.put_meta("emailTemplates", arr);
}

std::vector<std::string> merge_fields(const std::string& text) {
    std::vector<std::string> out;
    for (const MergeField& m : scan(text)) out.push_back(m.path);
    return out;
}

void validate_template(const engine::Org& org, EmailTemplate* t) {
    if (str::trim(t->name).empty()) throw errors::invalid_field("name", "Template name is required");
    const meta::ObjectDef* obj = org.schema.object(t->object);
    if (obj == nullptr || obj->name == "User") throw errors::invalid_field("object", "Unsupported object " + t->object);
    t->object = obj->name;
    if (t->subject.size() > 255) throw errors::invalid_field("subject", "Subjects are limited to 255 characters");
    if (t->body.size() > 32000) throw errors::invalid_field("body", "Bodies are limited to 32,000 characters");
    for (const std::string* text : {&t->subject, &t->body}) {
        for (const std::string& path : merge_fields(*text)) {
            if (path.empty()) throw errors::invalid_field("body", "Empty merge field");
            // The first segment must be a field or relationship on the object.
            const std::string first = str::split(path, '.')[0];
            if (path.find('.') == std::string::npos ? obj->field(first) == nullptr
                                                    : obj->relationship_field(first) == nullptr) {
                throw errors::invalid_field("body", "Unknown merge field {!" + path + "}");
            }
        }
    }
}

Rendered render_template(const engine::Org& org, const engine::Principal& who, const EmailTemplate& t,
                         const meta::ObjectDef& obj, const Record& r) {
    engine::RecordFormulaContext ctx(org, obj, r, nullptr, who);
    Rendered out;
    out.subject = fill(t.subject, ctx);
    out.body = fill(t.body, ctx);
    return out;
}

}  // namespace tcrm::services
