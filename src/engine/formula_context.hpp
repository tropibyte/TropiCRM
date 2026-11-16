// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "engine/access.hpp"
#include "engine/org.hpp"
#include "formula/formula.hpp"

namespace tcrm::engine {

/// Evaluates formulas against one record, resolving relationship paths
/// such as Account.Owner.Name through the org's data.
class RecordFormulaContext : public formula::Context {
public:
    RecordFormulaContext(const Org& org, const meta::ObjectDef& obj, const Record& record, const Record* prior,
                         const Principal& who);

    FieldValue field(const std::string& path) override;
    FieldValue prior(const std::string& field) override;
    bool is_new() override { return prior_ == nullptr; }
    FieldValue global(const std::string& name) override;
    const timeutil::TimeZone& timezone() override { return tz_; }

private:
    const Org& org_;
    const meta::ObjectDef& obj_;
    const Record& record_;
    const Record* prior_;
    const Principal& who_;
    timeutil::TimeZone tz_;
};

/// Compiled formulas, keyed by source text. Thread-safe.
class FormulaCache {
public:
    static FormulaCache& instance();

    /// Throws formula::FormulaError for invalid source.
    std::shared_ptr<const formula::Formula> get(const std::string& source);

private:
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<const formula::Formula>> cache_;
};

/// Evaluates `source` for a record. FormulaErrors become ApiErrors with the
/// given error code.
FieldValue evaluate_formula(const std::string& source, const Org& org, const meta::ObjectDef& obj, const Record& r,
                            const Record* prior, const Principal& who);

}  // namespace tcrm::engine
