// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <map>

#include "api/context.hpp"
#include "util/strutil.hpp"

namespace tcrm::api {
namespace {

constexpr size_t kMaxCompositeRequests = 25;

int64_t batch_size(const http::Request& req) {
    const std::string opts = req.headers.get("Sforce-Query-Options");
    const size_t at = opts.find("batchSize=");
    if (at == std::string::npos) return 2000;
    int64_t n = 0;
    const std::string_view rest = std::string_view(opts).substr(at + 10);
    if (!str::parse_int64(rest.substr(0, rest.find_first_of(", ;")), &n)) return 2000;
    return std::clamp<int64_t>(n, 200, 2000);
}

/// Replaces @{ref.field} with values from earlier composite responses.
std::string substitute_refs(const std::string& text, const std::map<std::string, json::Value>& results) {
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t start = text.find("@{", pos);
        if (start == std::string::npos) {
            out.append(text, pos, std::string::npos);
            return out;
        }
        const size_t end = text.find('}', start);
        if (end == std::string::npos) {
            out.append(text, pos, std::string::npos);
            return out;
        }
        out.append(text, pos, start - pos);
        const std::string expr = text.substr(start + 2, end - start - 2);
        const size_t dot = expr.find('.');
        const std::string ref = expr.substr(0, dot);
        auto it = results.find(ref);
        if (it == results.end() || dot == std::string::npos) {
            throw errors::bad_request("INVALID_REFERENCE", "Unknown reference " + expr);
        }
        const json::Value* v = &it->second;
        for (const std::string& part : str::split(expr.substr(dot + 1), '.')) {
            v = v->find(part);
            if (v == nullptr) throw errors::bad_request("INVALID_REFERENCE", "Unknown reference " + expr);
        }
        out += v->is_string() ? v->as_string() : v->dump();
        pos = end + 1;
    }
}

}  // namespace

void register_query_routes(http::Router& r, ApiContext& ctx) {
    auto run_query = [&ctx](http::Request& req, bool include_deleted) {
        const engine::Principal& who = principal(req);
        const std::string q = req.query_param("q");
        if (str::trim(q).empty()) throw errors::malformed_query("Use the q parameter to send a query");
        query::ExecOptions opts;
        opts.batch_size = batch_size(req);
        opts.include_deleted = include_deleted;
        return json_response(200, query::execute(q, who, opts, &ctx.cursors).to_json());
    };

    r.get("/services/data/v1/query", authed(ctx, [run_query](http::Request& req) { return run_query(req, false); }));
    r.get("/services/data/v1/queryAll", authed(ctx, [run_query](http::Request& req) { return run_query(req, true); }));

    r.get("/services/data/v1/query/{cursor}", authed(ctx, [&ctx](http::Request& req) {
              query::QueryResult result;
              if (!ctx.cursors.next(req.path_param("cursor"), batch_size(req), &result)) {
                  throw ApiError(400, "INVALID_QUERY_LOCATOR", "invalid query locator");
              }
              return json_response(200, result.to_json());
          }));

    r.get("/services/data/v1/search", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              services::SearchService::Options opts;
              opts.query = req.query_param("q");
              for (const std::string& o : str::split_trimmed(req.query_param("objects"), ',')) opts.objects.insert(o);
              int64_t limit = 25;
              if (req.has_query_param("limit") && str::parse_int64(req.query_param("limit"), &limit)) {
                  opts.limit = static_cast<size_t>(std::clamp<int64_t>(limit, 1, 200));
              }
              return json_response(200, ctx.search.search(who, opts));
          }));

    r.post("/services/data/v1/composite", authed(ctx, [&ctx](http::Request& req) {
               const json::Value body = body_object(req);
               const json::Value& subs = body.at("compositeRequest");
               if (!subs.is_array() || subs.empty()) {
                   throw errors::bad_request("INVALID_INPUT", "compositeRequest must be a non-empty array");
               }
               if (subs.size() > kMaxCompositeRequests) {
                   throw errors::bad_request("LIMIT_EXCEEDED", "A composite request can have at most 25 subrequests");
               }
               const bool all_or_none = body.get_bool("allOrNone");
               std::map<std::string, json::Value> results;
               json::Value responses = json::Value::array();
               bool failed = false;
               for (const json::Value& sub : subs.as_array()) {
                   const std::string ref = sub.get_string("referenceId");
                   json::Value entry = json::Value::object();
                   entry.set("referenceId", ref);
                   if (failed && all_or_none) {
                       entry.set("httpStatusCode", 400);
                       json::Value err = json::Value::array();
                       json::Value e = json::Value::object();
                       e.set("errorCode", "PROCESSING_HALTED");
                       e.set("message", "The transaction was rolled back since another operation in the same "
                                        "transaction failed.");
                       err.push_back(std::move(e));
                       entry.set("body", std::move(err));
                       responses.push_back(std::move(entry));
                       continue;
                   }
                   http::Request inner;
                   inner.method_text = str::to_upper(sub.get_string("method", "GET"));
                   inner.method = http::parse_method(inner.method_text);
                   inner.target = substitute_refs(sub.get_string("url"), results);
                   const size_t q = inner.target.find('?');
                   inner.path = inner.target.substr(0, q);
                   if (q != std::string::npos) {
                       inner.query_string = inner.target.substr(q + 1);
                       inner.query = url::parse_query(inner.query_string);
                   }
                   if (const json::Value* b = sub.find("body")) inner.body = substitute_refs(b->dump(), results);
                   inner.headers = req.headers;
                   inner.headers.set("Content-Type", "application/json");
                   inner.remote_addr = req.remote_addr;
                   inner.request_id = req.request_id + "." + ref;

                   http::Response resp;
                   try {
                       http::Router::Match m = ctx.router.match(inner);
                       if (m.handler == nullptr) throw errors::not_found("No such resource: " + inner.path);
                       if (inner.path.rfind("/services/data/", 0) != 0) {
                           throw errors::bad_request("INVALID_INPUT", "Only data API resources are allowed in composite");
                       }
                       resp = (*m.handler)(inner);
                   } catch (const ApiError& e) {
                       resp = error_response(e);
                   }
                   json::Value rbody;
                   if (!resp.body.empty()) json::try_parse(resp.body, &rbody);
                   entry.set("httpStatusCode", resp.status);
                   entry.set("body", rbody);
                   if (resp.status >= 400) failed = true;
                   if (!ref.empty()) results[ref] = rbody;
                   responses.push_back(std::move(entry));
               }
               json::Value out = json::Value::object();
               out.set("compositeResponse", std::move(responses));
               return json_response(200, out);
           }));
}

}  // namespace tcrm::api
