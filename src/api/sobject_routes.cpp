// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <shared_mutex>

#include "api/context.hpp"
#include "engine/records.hpp"
#include "util/crypto.hpp"
#include "util/id.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

using engine::kApiBase;

json::Value save_result(const std::string& id, bool created) {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("success", true);
    v.set("errors", json::Value::array());
    v.set("created", created);
    return v;
}

engine::SaveOptions save_options(const http::Request& req) {
    engine::SaveOptions opts;
    const std::string dup = str::to_lower(req.headers.get("Sforce-Duplicate-Rule-Header"));
    opts.allow_duplicates = dup.find("allowsave=true") != std::string::npos;
    const std::string assign = str::to_lower(req.headers.get("Sforce-Auto-Assign"));
    opts.run_assignment_rules = assign != "false";
    const std::string if_match = req.headers.get("If-Match");
    if (if_match.size() > 3 && if_match.front() == '"' && if_match[1] == 'v' && if_match.back() == '"') {
        int64_t v = 0;
        if (str::parse_int64(std::string_view(if_match).substr(2, if_match.size() - 3), &v) && v > 0) {
            opts.expected_version = static_cast<uint64_t>(v);
        }
    }
    return opts;
}

std::string etag(const Record& r) {
    return "\"v" + std::to_string(r.version) + "\"";
}

json::Value record_json(const engine::Principal& who, const std::string& object, const Record& r,
                        const std::vector<std::string>* fields) {
    std::shared_lock<std::shared_mutex> lock(who.org->mutex());
    const meta::ObjectDef* obj = who.org->schema.object(object);
    engine::AccessChecker access(*who.org, who);
    if (fields != nullptr) {
        for (const std::string& f : *fields) {
            if (obj->field(f) == nullptr) {
                throw errors::bad_request("INVALID_FIELD", "No such column '" + f + "' on sobject of type " + obj->name);
            }
        }
    }
    return engine::record_to_api_json(*obj, r, access, fields);
}

}  // namespace

void register_sobject_routes(http::Router& r, ApiContext& ctx) {
    r.get("/services/data", [](http::Request&) {
        json::Value arr = json::Value::array();
        json::Value v = json::Value::object();
        v.set("version", "1.0");
        v.set("label", "Tropic '26");
        v.set("url", std::string(kApiBase));
        arr.push_back(std::move(v));
        return json_response(200, arr);
    });

    r.get("/services/data/v1", authed(ctx, [](http::Request&) {
              json::Value v = json::Value::object();
              const std::string base(kApiBase);
              for (const char* res : {"sobjects", "query", "queryAll", "search", "composite", "limits", "recyclebin",
                                      "analytics", "jobs", "tooling", "users", "webhooks", "actions"}) {
                  v.set(res, base + "/" + res);
              }
              return json_response(200, v);
          }));

    r.get("/services/data/v1/limits", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              engine::Org& org = *who.org;
              json::Value v = json::Value::object();
              json::Value api = json::Value::object();
              const int64_t used = org.usage.calls.load();
              api.set("Max", org.settings.api_calls_per_day);
              api.set("Remaining", std::max<int64_t>(0, org.settings.api_calls_per_day - used));
              v.set("DailyApiRequests", std::move(api));
              json::Value storage = json::Value::object();
              uint64_t bytes = 0;
              {
                  std::shared_lock<std::shared_mutex> lock(org.mutex());
                  bytes = org.db.approximate_bytes();
              }
              storage.set("Max", org.settings.storage_mb);
              storage.set("Remaining", org.settings.storage_mb - static_cast<int64_t>(bytes / (1024 * 1024)));
              v.set("DataStorageMB", std::move(storage));
              return json_response(200, v);
          }));

    r.get("/services/data/v1/sobjects", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              engine::AccessChecker access(*who.org, who);
              json::Value list = json::Value::array();
              for (const meta::ObjectDef* o : who.org->schema.objects()) {
                  if (!access.object_allowed(*o, engine::ObjectOp::Read)) continue;
                  json::Value jo = json::Value::object();
                  jo.set("name", o->name);
                  jo.set("label", o->label);
                  jo.set("labelPlural", o->plural_label);
                  jo.set("keyPrefix", o->key_prefix);
                  jo.set("custom", o->custom);
                  jo.set("createable", access.object_allowed(*o, engine::ObjectOp::Create) && o->name != "User");
                  jo.set("queryable", o->queryable);
                  jo.set("searchable", o->searchable);
                  json::Value urls = json::Value::object();
                  urls.set("sobject", std::string(kApiBase) + "/sobjects/" + o->name);
                  urls.set("describe", std::string(kApiBase) + "/sobjects/" + o->name + "/describe");
                  jo.set("urls", std::move(urls));
                  list.push_back(std::move(jo));
              }
              json::Value v = json::Value::object();
              v.set("encoding", "UTF-8");
              v.set("maxBatchSize", 200);
              v.set("sobjects", std::move(list));
              return json_response(200, v);
          }));

    r.get("/services/data/v1/sobjects/{object}/describe", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              const std::string key = who.org->id() + "/" + str::to_lower(req.path_param("object"));
              if (auto cached = ctx.describe_cache.get(key)) return json_response(200, *cached);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              const meta::ObjectDef* obj = who.org->schema.object(req.path_param("object"));
              if (obj == nullptr) throw errors::not_found();
              engine::AccessChecker access(*who.org, who);
              access.require_object(*obj, engine::ObjectOp::Read);
              json::Value d = describe_object(*who.org, access, *obj);
              ctx.describe_cache.put(key, d);
              return json_response(200, d);
          }));

    r.get("/services/data/v1/sobjects/{object}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              const meta::ObjectDef* obj = who.org->schema.object(req.path_param("object"));
              if (obj == nullptr) throw errors::not_found();
              engine::AccessChecker access(*who.org, who);
              access.require_object(*obj, engine::ObjectOp::Read);
              json::Value info = json::Value::object();
              info.set("name", obj->name);
              info.set("label", obj->label);
              info.set("keyPrefix", obj->key_prefix);
              // The ten most recently modified records the user can see.
              std::vector<const Record*> recent;
              if (const storage::Table* t = who.org->db.find_table(obj->name)) {
                  for (const auto& [id, rec] : t->rows()) {
                      if (!rec.is_deleted() && access.can_read(*obj, rec)) recent.push_back(&rec);
                  }
              }
              std::sort(recent.begin(), recent.end(), [](const Record* a, const Record* b) {
                  return b->get("LastModifiedDate") < a->get("LastModifiedDate");
              });
              if (recent.size() > 10) recent.resize(10);
              json::Value items = json::Value::array();
              for (const Record* rec : recent) {
                  json::Value item = json::Value::object();
                  item.set("attributes", engine::record_attributes(obj->name, rec->id));
                  item.set("Id", rec->id);
                  item.set(obj->name_field, engine::record_name(*obj, *rec));
                  items.push_back(std::move(item));
              }
              json::Value v = json::Value::object();
              v.set("objectDescribe", std::move(info));
              v.set("recentItems", std::move(items));
              return json_response(200, v);
          }));

    r.post("/services/data/v1/sobjects/{object}", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const Record rec = ctx.records.create(who, req.path_param("object"), body_object(req), save_options(req));
               http::Response resp = json_response(201, save_result(rec.id, true));
               resp.headers.set("Location", std::string(kApiBase) + "/sobjects/" + rec.object + "/" + rec.id);
               return resp;
           }));

    r.get("/services/data/v1/sobjects/{object}/{id}", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              const Record rec = ctx.records.get(who, req.path_param("object"), req.path_param("id"));
              ctx.recent.touch(who.org->id(), who.user_id, rec.id);
              std::vector<std::string> fields;
              const std::string fields_param = req.query_param("fields");
              if (!fields_param.empty()) fields = str::split_trimmed(fields_param, ',');
              const std::string tag = etag(rec);
              if (req.headers.get("If-None-Match") == tag) {
                  http::Response not_modified;
                  not_modified.status = 304;
                  not_modified.headers.set("ETag", tag);
                  return not_modified;
              }
              http::Response resp =
                  json_response(200, record_json(who, rec.object, rec, fields.empty() ? nullptr : &fields));
              resp.headers.set("ETag", tag);
              return resp;
          }));

    r.patch("/services/data/v1/sobjects/{object}/{id}", authed(ctx, [&ctx](http::Request& req) {
                const engine::Principal& who = principal(req);
                ctx.records.update(who, req.path_param("object"), req.path_param("id"), body_object(req),
                                   save_options(req));
                return http::Response::no_content();
            }));

    r.del("/services/data/v1/sobjects/{object}/{id}", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              ctx.records.remove(who, req.path_param("object"), req.path_param("id"));
              return http::Response::no_content();
          }));

    r.patch("/services/data/v1/sobjects/{object}/{field}/{value}", authed(ctx, [&ctx](http::Request& req) {
                const engine::Principal& who = principal(req);
                const std::string& object = req.path_param("object");
                const std::string& field = req.path_param("field");
                const std::string& value = req.path_param("value");
                auto result = ctx.records.upsert(who, object, field, value, body_object(req), save_options(req));
                http::Response resp = json_response(result.created ? 201 : 200, save_result(result.record.id, result.created));
                resp.headers.set("Location", std::string(kApiBase) + "/sobjects/" + object + "/" + field + "/" + value);
                return resp;
            }));

    r.get("/services/data/v1/sobjects/{object}/{id}/history", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              const auto entries = ctx.records.history(who, req.path_param("object"), req.path_param("id"));
              json::Value records = json::Value::array();
              for (const auto& e : entries) {
                  json::Value h = json::Value::object();
                  h.set("Id", e.id);
                  h.set("ParentId", e.record_id);
                  h.set("Field", e.field);
                  h.set("OldValue", e.old_value);
                  h.set("NewValue", e.new_value);
                  h.set("CreatedById", e.changed_by);
                  h.set("CreatedDate", timeutil::format_datetime(e.changed_at));
                  records.push_back(std::move(h));
              }
              json::Value v = json::Value::object();
              v.set("totalSize", records.size());
              v.set("done", true);
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    // Manual sharing: give another user read or edit access to one record.
    r.post("/services/data/v1/sobjects/{object}/{id}/shares", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const json::Value body = body_object(req);
               const Record rec = ctx.records.get(who, req.path_param("object"), req.path_param("id"));
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               const meta::ObjectDef* obj = org.schema.object(rec.object);
               engine::AccessChecker access(org, who);
               if (!access.can_delete(*obj, rec)) throw errors::forbidden("Only the owner or an administrator can share");
               security::ManualShare share;
               share.record_id = rec.id;
               share.user_id = body.get_string("userId");
               share.reason = body.get_string("reason", "Manual");
               if (!security::parse_access(body.get_string("access", "Read"), &share.access) ||
                   share.access == security::Access::All || share.access == security::Access::None) {
                   throw errors::invalid_field("access", "access must be Read or Edit");
               }
               const auth::User* target = org.users.find(share.user_id);
               if (target == nullptr || !target->active) throw errors::invalid_field("userId", "No such active user");
               org.sharing.manual.push_back(share);
               org.save_sharing();
               json::Value v = json::Value::object();
               v.set("success", true);
               return json_response(201, v);
           }));

    r.post("/services/data/v1/sobjects/{object}/{id}/attachments", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const Record parent = ctx.records.get(who, req.path_param("object"), req.path_param("id"));
               std::string name = req.headers.get("X-File-Name");
               if (name.empty()) name = req.query_param("name");
               if (name.empty()) throw errors::invalid_field("name", "Send the file name in X-File-Name or ?name=");
               std::string ctype = req.headers.get("Content-Type");
               if (ctype.empty() || ctype == "application/octet-stream") ctype = services::FileStore::guess_content_type(name);

               json::Value fields = json::Value::object();
               fields.set("Name", name);
               fields.set("ParentId", parent.id);
               fields.set("ContentType", ctype);
               if (req.has_query_param("description")) fields.set("Description", req.query_param("description"));
               Record att = ctx.records.create(who, "Attachment", fields);

               const std::string key = ctx.files.save(who.org->id(), att.id, name, req.body);
               {
                   engine::Org& org = *who.org;
                   std::unique_lock<std::shared_mutex> lock(org.mutex());
                   att.set("_StorageKey", FieldValue::string(key));
                   att.set("BodyLength", FieldValue::number(static_cast<double>(req.body.size())));
                   att.set("ContentHash", FieldValue::string(crypto::to_hex(crypto::sha256(req.body))));
                   org.db.put(att);
               }
               http::Response resp = json_response(201, save_result(att.id, true));
               resp.headers.set("Location", std::string(kApiBase) + "/sobjects/Attachment/" + att.id);
               return resp;
           }));

    r.get("/services/data/v1/sobjects/Attachment/{id}/body", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              const Record att = ctx.records.get(who, "Attachment", req.path_param("id"));
              std::string bytes;
              if (!ctx.files.load(who.org->id(), att.text("_StorageKey"), &bytes)) {
                  throw errors::not_found("The attachment body is missing");
              }
              http::Response resp = http::Response::text(200, std::move(bytes),
                                                         att.text("ContentType").empty() ? "application/octet-stream"
                                                                                         : att.text("ContentType"));
              resp.headers.set("Content-Disposition", "attachment; filename=\"" + att.text("Name") + "\"");
              return resp;
          }));

    r.get("/services/data/v1/recyclebin", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              engine::AccessChecker access(*who.org, who);
              json::Value records = json::Value::array();
              for (const meta::ObjectDef* obj : who.org->schema.objects()) {
                  const storage::Table* t = who.org->db.find_table(obj->name);
                  if (t == nullptr) continue;
                  for (const auto& [id, rec] : t->rows()) {
                      if (!rec.is_deleted() || !access.can_delete(*obj, rec)) continue;
                      json::Value item = json::Value::object();
                      item.set("attributes", engine::record_attributes(obj->name, id));
                      item.set("Id", id);
                      item.set("Name", engine::record_name(*obj, rec));
                      item.set("DeletedDate", rec.get("LastModifiedDate").to_text());
                      records.push_back(std::move(item));
                  }
              }
              json::Value v = json::Value::object();
              v.set("totalSize", records.size());
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    r.post("/services/data/v1/recyclebin/{id}/undelete", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const Record rec = ctx.records.undelete(who, req.path_param("id"));
               return json_response(200, save_result(rec.id, false));
           }));

    r.del("/services/data/v1/recyclebin/{id}", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              ctx.records.purge(who, req.path_param("id"));
              return http::Response::no_content();
          }));
}

}  // namespace tcrm::api
