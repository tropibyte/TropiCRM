// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "api/context.hpp"

namespace tcrm::api {

void register_bulk_routes(http::Router& r, ApiContext& ctx) {
    const std::string base = "/services/data/v1/jobs/ingest";

    r.post(base, authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const json::Value body = body_object(req);
               const std::string id = ctx.bulk.create(who, body.get_string("object"), body.get_string("operation"),
                                                      body.get_string("externalIdFieldName"));
               return json_response(201, ctx.bulk.info(who, id));
           }));

    r.get(base, authed(ctx, [&ctx](http::Request& req) { return json_response(200, ctx.bulk.list(principal(req))); }));

    r.get(base + "/{id}", authed(ctx, [&ctx](http::Request& req) {
              return json_response(200, ctx.bulk.info(principal(req), req.path_param("id")));
          }));

    r.put(base + "/{id}/batches", authed(ctx, [&ctx](http::Request& req) {
              ctx.bulk.upload(principal(req), req.path_param("id"), req.body);
              http::Response resp;
              resp.status = 201;
              return resp;
          }));

    r.patch(base + "/{id}", authed(ctx, [&ctx](http::Request& req) {
                const json::Value body = body_object(req);
                return json_response(200, ctx.bulk.set_state(principal(req), req.path_param("id"), body.get_string("state")));
            }));

    r.del(base + "/{id}", authed(ctx, [&ctx](http::Request& req) {
              ctx.bulk.remove(principal(req), req.path_param("id"));
              return http::Response::no_content();
          }));

    r.get(base + "/{id}/successfulResults", authed(ctx, [&ctx](http::Request& req) {
              return http::Response::text(200, ctx.bulk.results_csv(principal(req), req.path_param("id"), false),
                                          "text/csv");
          }));

    r.get(base + "/{id}/failedResults", authed(ctx, [&ctx](http::Request& req) {
              return http::Response::text(200, ctx.bulk.results_csv(principal(req), req.path_param("id"), true),
                                          "text/csv");
          }));
}

}  // namespace tcrm::api
