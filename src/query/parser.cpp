// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <cctype>

#include "model/errors.hpp"
#include "query/ast.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::query {
namespace {

enum class Tok { End, Ident, String, Number, DateLit, DateTimeLit, Comma, LParen, RParen, Op, Colon };

struct Token {
    Tok type = Tok::End;
    std::string text;
    size_t pos = 0;
};

[[noreturn]] void syntax_error(const std::string& msg, size_t pos) {
    throw errors::malformed_query(msg + " at position " + std::to_string(pos + 1));
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

class Lexer {
public:
    explicit Lexer(const std::string& s) : s_(s) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
            Token t;
            t.pos = pos_;
            if (pos_ >= s_.size()) {
                out.push_back(t);
                return out;
            }
            const char c = s_[pos_];
            if (c == '\'') {
                lex_string(&t);
            } else if (is_digit(c) || ((c == '-' || c == '.') && pos_ + 1 < s_.size() && is_digit(s_[pos_ + 1]))) {
                lex_number_or_date(&t);
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                const size_t start = pos_;
                while (pos_ < s_.size() &&
                       (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_' || s_[pos_] == '.')) {
                    ++pos_;
                }
                t.type = Tok::Ident;
                t.text = s_.substr(start, pos_ - start);
            } else if (c == ',') {
                t.type = Tok::Comma;
                ++pos_;
            } else if (c == '(') {
                t.type = Tok::LParen;
                ++pos_;
            } else if (c == ')') {
                t.type = Tok::RParen;
                ++pos_;
            } else if (c == ':') {
                t.type = Tok::Colon;
                ++pos_;
            } else {
                lex_op(&t);
            }
            out.push_back(std::move(t));
        }
    }

private:
    void lex_string(Token* t) {
        const size_t start = pos_++;
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) syntax_error("unterminated string literal", start);
            const char c = s_[pos_++];
            if (c == '\'') break;
            if (c == '\\') {
                if (pos_ >= s_.size()) syntax_error("unterminated string literal", start);
                const char e = s_[pos_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '\'': out += '\''; break;
                    case '\\': out += '\\'; break;
                    case '_': out += "\\_"; break;  // keep LIKE escapes for the matcher
                    case '%': out += "\\%"; break;
                    default: syntax_error("invalid escape sequence", pos_ - 2);
                }
                continue;
            }
            out += c;
        }
        t->type = Tok::String;
        t->text = std::move(out);
    }

    void lex_number_or_date(Token* t) {
        const size_t start = pos_;
        // Date literal: YYYY-MM-DD, optionally followed by Thh:mm:ss(.fff)(Z|+hh:mm)
        if (pos_ + 10 <= s_.size() && is_digit(s_[pos_]) && s_[pos_ + 4] == '-' && s_[pos_ + 7] == '-') {
            pos_ += 10;
            if (pos_ < s_.size() && (s_[pos_] == 'T' || s_[pos_] == 't')) {
                while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == ':' ||
                                            s_[pos_] == '.' || s_[pos_] == '+' || s_[pos_] == '-')) {
                    ++pos_;
                }
                t->type = Tok::DateTimeLit;
            } else {
                t->type = Tok::DateLit;
            }
            t->text = s_.substr(start, pos_ - start);
            return;
        }
        ++pos_;
        while (pos_ < s_.size() && (is_digit(s_[pos_]) || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
        }
        t->type = Tok::Number;
        t->text = s_.substr(start, pos_ - start);
    }

    void lex_op(Token* t) {
        static const char* const kOps[] = {"!=", "<>", "<=", ">=", "=", "<", ">"};
        for (const char* op : kOps) {
            const size_t n = std::char_traits<char>::length(op);
            if (s_.compare(pos_, n, op) == 0) {
                t->type = Tok::Op;
                t->text = op == std::string("<>") ? "!=" : op;
                pos_ += n;
                return;
            }
        }
        syntax_error(std::string("unexpected character '") + s_[pos_] + "'", pos_);
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool is_date_keyword(const std::string& upper) {
    static const char* const kKeywords[] = {
        "TODAY",      "YESTERDAY",  "TOMORROW",    "LAST_WEEK",    "THIS_WEEK",   "NEXT_WEEK",
        "LAST_MONTH", "THIS_MONTH", "NEXT_MONTH",  "LAST_90_DAYS", "NEXT_90_DAYS", "THIS_QUARTER",
        "LAST_QUARTER", "NEXT_QUARTER", "THIS_YEAR", "LAST_YEAR",   "NEXT_YEAR"};
    for (const char* k : kKeywords) {
        if (upper == k) return true;
    }
    return false;
}

bool is_parametric_date_keyword(const std::string& upper) {
    return upper == "LAST_N_DAYS" || upper == "NEXT_N_DAYS" || upper == "LAST_N_MONTHS" || upper == "NEXT_N_MONTHS" ||
           upper == "N_DAYS_AGO";
}

bool is_aggregate_function(const std::string& upper) {
    return upper == "COUNT" || upper == "COUNT_DISTINCT" || upper == "SUM" || upper == "AVG" || upper == "MIN" ||
           upper == "MAX";
}

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    Query parse_query(bool nested) {
        Query q;
        expect_keyword("SELECT");
        parse_select_list(&q);
        expect_keyword("FROM");
        const Token& from = next();
        if (from.type != Tok::Ident) syntax_error("expected an object name after FROM", from.pos);
        q.from = from.text;
        if (accept_keyword("WHERE")) q.where = parse_or();
        if (accept_keyword("GROUP")) {
            expect_keyword("BY");
            do {
                q.group_by.push_back(parse_path());
            } while (accept(Tok::Comma));
        }
        if (accept_keyword("HAVING")) q.having = parse_or();
        if (accept_keyword("ORDER")) {
            expect_keyword("BY");
            do {
                q.order_by.push_back(parse_order_item());
            } while (accept(Tok::Comma));
        }
        if (accept_keyword("LIMIT")) q.limit = parse_int("LIMIT");
        if (accept_keyword("OFFSET")) q.offset = parse_int("OFFSET");
        if (accept_keyword("ALL")) {
            expect_keyword("ROWS");
            q.all_rows = true;
        }
        if (!nested && peek().type != Tok::End) syntax_error("unexpected '" + peek().text + "'", peek().pos);
        return q;
    }

private:
    const Token& peek(size_t ahead = 0) const {
        const size_t i = std::min(i_ + ahead, toks_.size() - 1);
        return toks_[i];
    }
    const Token& next() {
        const Token& t = toks_[i_];
        if (i_ + 1 < toks_.size()) ++i_;
        return t;
    }
    bool accept(Tok type) {
        if (peek().type == type) {
            next();
            return true;
        }
        return false;
    }
    bool is_keyword(const Token& t, const char* kw) const {
        return t.type == Tok::Ident && str::iequals(t.text, kw);
    }
    bool accept_keyword(const char* kw) {
        if (is_keyword(peek(), kw)) {
            next();
            return true;
        }
        return false;
    }
    void expect_keyword(const char* kw) {
        if (!accept_keyword(kw)) syntax_error(std::string("expected ") + kw, peek().pos);
    }

    int64_t parse_int(const char* clause) {
        const Token& t = next();
        int64_t v = 0;
        if (t.type != Tok::Number || !str::parse_int64(t.text, &v) || v < 0) {
            syntax_error(std::string(clause) + " expects a non-negative integer", t.pos);
        }
        return v;
    }

    FieldPath parse_path() {
        const Token& t = next();
        if (t.type != Tok::Ident) syntax_error("expected a field name", t.pos);
        FieldPath p;
        p.parts = str::split(t.text, '.');
        for (const std::string& part : p.parts) {
            if (part.empty()) syntax_error("empty name in field path", t.pos);
        }
        return p;
    }

    void parse_select_list(Query* q) {
        do {
            SelectItem item;
            if (peek().type == Tok::LParen && is_keyword(peek(1), "SELECT")) {
                next();
                item.kind = SelectItem::Kind::Subquery;
                item.subquery = std::make_shared<Query>(parse_query(true));
                if (!accept(Tok::RParen)) syntax_error("expected ')' after subquery", peek().pos);
                q->select.push_back(std::move(item));
                continue;
            }
            const Token& t = peek();
            if (t.type == Tok::Ident && peek(1).type == Tok::LParen && is_aggregate_function(str::to_upper(t.text))) {
                next();
                next();
                item.kind = SelectItem::Kind::Aggregate;
                item.function = str::to_upper(t.text);
                if (peek().type == Tok::RParen) {
                    if (item.function != "COUNT") syntax_error(item.function + " needs a field", peek().pos);
                    item.count_star = true;
                } else {
                    item.path = parse_path();
                }
                if (!accept(Tok::RParen)) syntax_error("expected ')'", peek().pos);
                if (peek().type == Tok::Ident && !is_keyword(peek(), "FROM")) item.alias = next().text;
            } else {
                item.path = parse_path();
            }
            q->select.push_back(std::move(item));
        } while (accept(Tok::Comma));
        if (q->select.empty()) syntax_error("SELECT needs at least one field", peek().pos);
    }

    OrderItem parse_order_item() {
        OrderItem o;
        const Token& t = peek();
        if (t.type == Tok::Ident && peek(1).type == Tok::LParen && is_aggregate_function(str::to_upper(t.text))) {
            next();
            next();
            o.aggregate = str::to_upper(t.text);
            if (peek().type != Tok::RParen) o.path = parse_path();
            if (!accept(Tok::RParen)) syntax_error("expected ')'", peek().pos);
        } else {
            o.path = parse_path();
        }
        if (accept_keyword("DESC")) {
            o.descending = true;
        } else {
            accept_keyword("ASC");
        }
        if (accept_keyword("NULLS")) {
            if (accept_keyword("FIRST")) o.nulls = OrderItem::Nulls::First;
            else if (accept_keyword("LAST")) o.nulls = OrderItem::Nulls::Last;
            else syntax_error("expected FIRST or LAST after NULLS", peek().pos);
        }
        return o;
    }

    Condition parse_or() {
        Condition left = parse_and();
        if (!is_keyword(peek(), "OR")) return left;
        Condition node;
        node.kind = Condition::Kind::Or;
        node.children.push_back(std::move(left));
        while (accept_keyword("OR")) node.children.push_back(parse_and());
        return node;
    }

    Condition parse_and() {
        Condition left = parse_not();
        if (!is_keyword(peek(), "AND")) return left;
        Condition node;
        node.kind = Condition::Kind::And;
        node.children.push_back(std::move(left));
        while (accept_keyword("AND")) node.children.push_back(parse_not());
        return node;
    }

    Condition parse_not() {
        if (accept_keyword("NOT")) {
            Condition node;
            node.kind = Condition::Kind::Not;
            node.children.push_back(parse_not());
            return node;
        }
        if (accept(Tok::LParen)) {
            Condition inner = parse_or();
            if (!accept(Tok::RParen)) syntax_error("expected ')'", peek().pos);
            return inner;
        }
        return parse_comparison();
    }

    Operand parse_operand() {
        const Token& t = next();
        Operand o;
        o.raw = t.text;
        switch (t.type) {
            case Tok::String:
                o.value = FieldValue::string(t.text);
                return o;
            case Tok::Number: {
                int64_t i = 0;
                double d = 0;
                if (str::parse_int64(t.text, &i)) o.value = FieldValue::integer(i);
                else if (str::parse_double(t.text, &d)) o.value = FieldValue::number(d);
                else syntax_error("invalid number " + t.text, t.pos);
                return o;
            }
            case Tok::DateLit: {
                int64_t days = 0;
                if (!timeutil::parse_date(t.text, &days)) syntax_error("invalid date " + t.text, t.pos);
                o.value = FieldValue::date(days);
                o.is_date_literal = true;
                return o;
            }
            case Tok::DateTimeLit: {
                int64_t ms = 0;
                if (!timeutil::parse_datetime(t.text, &ms)) syntax_error("invalid date-time " + t.text, t.pos);
                o.value = FieldValue::datetime(ms);
                o.is_datetime_literal = true;
                return o;
            }
            case Tok::Ident: {
                const std::string upper = str::to_upper(t.text);
                if (upper == "NULL") return o;
                if (upper == "TRUE" || upper == "FALSE") {
                    o.value = FieldValue::boolean(upper == "TRUE");
                    return o;
                }
                if (is_date_keyword(upper)) {
                    o.kind = Operand::Kind::DateRange;
                    o.literal = upper;
                    return o;
                }
                if (is_parametric_date_keyword(upper)) {
                    if (!accept(Tok::Colon)) syntax_error(upper + " needs :n", peek().pos);
                    const Token& n = next();
                    int64_t v = 0;
                    if (n.type != Tok::Number || !str::parse_int64(n.text, &v) || v < 0 || v > 4000) {
                        syntax_error(upper + " needs a number from 0 to 4000", n.pos);
                    }
                    o.kind = Operand::Kind::DateRange;
                    o.literal = upper + ":" + n.text;
                    return o;
                }
                syntax_error("unexpected '" + t.text + "' where a value was expected", t.pos);
            }
            default:
                syntax_error("expected a value", t.pos);
        }
    }

    Condition parse_comparison() {
        Condition c;
        const Token& t = peek();
        if (t.type == Tok::Ident && peek(1).type == Tok::LParen && is_aggregate_function(str::to_upper(t.text))) {
            next();
            next();
            c.aggregate = str::to_upper(t.text);
            if (peek().type != Tok::RParen) c.field = parse_path();
            if (!accept(Tok::RParen)) syntax_error("expected ')'", peek().pos);
        } else {
            c.field = parse_path();
        }
        if (accept_keyword("NOT")) {
            c.negated = true;
            if (accept_keyword("IN")) {
                c.kind = Condition::Kind::In;
            } else if (accept_keyword("LIKE")) {
                c.kind = Condition::Kind::Like;
            } else {
                syntax_error("expected IN or LIKE after NOT", peek().pos);
            }
        } else if (accept_keyword("IN")) {
            c.kind = Condition::Kind::In;
        } else if (accept_keyword("LIKE")) {
            c.kind = Condition::Kind::Like;
        } else if (accept_keyword("INCLUDES")) {
            c.kind = Condition::Kind::Includes;
        } else if (accept_keyword("EXCLUDES")) {
            c.kind = Condition::Kind::Excludes;
        } else {
            const Token& op = next();
            if (op.type != Tok::Op) syntax_error("expected a comparison operator", op.pos);
            c.kind = Condition::Kind::Compare;
            c.op = op.text;
            c.values.push_back(parse_operand());
            return c;
        }
        if (c.kind == Condition::Kind::Like) {
            const Token& pat = next();
            if (pat.type != Tok::String) syntax_error("LIKE needs a string pattern", pat.pos);
            Operand o;
            o.value = FieldValue::string(pat.text);
            c.values.push_back(std::move(o));
            return c;
        }
        if (!accept(Tok::LParen)) syntax_error("expected '(' to start a value list", peek().pos);
        do {
            c.values.push_back(parse_operand());
        } while (accept(Tok::Comma));
        if (!accept(Tok::RParen)) syntax_error("expected ')' to end the value list", peek().pos);
        return c;
    }

    std::vector<Token> toks_;
    size_t i_ = 0;
};

}  // namespace

std::string FieldPath::text() const {
    return str::join(parts, ".");
}

std::string SelectItem::expression_key() const {
    if (kind != Kind::Aggregate) return path.text();
    return function + "(" + (count_star ? std::string() : str::to_lower(path.text())) + ")";
}

bool Query::is_aggregate() const {
    if (!group_by.empty()) return true;
    for (const SelectItem& s : select) {
        if (s.kind == SelectItem::Kind::Aggregate) return true;
    }
    return false;
}

bool Query::is_count_only() const {
    return select.size() == 1 && select[0].kind == SelectItem::Kind::Aggregate && select[0].count_star &&
           group_by.empty();
}

Query parse(const std::string& text) {
    if (text.size() > 100000) throw errors::malformed_query("query is too long");
    Lexer lexer(text);
    Parser parser(lexer.run());
    return parser.parse_query(false);
}

std::string quote_literal(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        switch (c) {
            case '\'': out += "\\'"; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    out += '\'';
    return out;
}

}  // namespace tcrm::query
