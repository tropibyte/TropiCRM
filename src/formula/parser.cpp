// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <cctype>
#include <set>

#include "formula/formula.hpp"
#include "formula/internal.hpp"
#include "util/strutil.hpp"

namespace tcrm::formula {
namespace {

enum class Tok { End, Number, String, Ident, Op, LParen, RParen, Comma };

struct Token {
    Tok type = Tok::End;
    std::string text;
    double number = 0;
    bool integral = false;
    size_t pos = 0;
};

class Lexer {
public:
    explicit Lexer(const std::string& s) : s_(s) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            skip_space_and_comments();
            Token t;
            t.pos = pos_;
            if (pos_ >= s_.size()) {
                out.push_back(t);
                return out;
            }
            const char c = s_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
                lex_number(&t);
            } else if (c == '"' || c == '\'') {
                lex_string(&t);
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
                lex_ident(&t);
            } else if (c == '(') {
                t.type = Tok::LParen;
                ++pos_;
            } else if (c == ')') {
                t.type = Tok::RParen;
                ++pos_;
            } else if (c == ',') {
                t.type = Tok::Comma;
                ++pos_;
            } else {
                lex_operator(&t);
            }
            out.push_back(std::move(t));
        }
    }

private:
    void skip_space_and_comments() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
                const size_t end = s_.find("*/", pos_ + 2);
                if (end == std::string::npos) throw FormulaError("unterminated comment", pos_);
                pos_ = end + 2;
            } else {
                break;
            }
        }
    }

    void lex_number(Token* t) {
        const size_t start = pos_;
        bool dot = false;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (c == '.' && !dot) {
                dot = true;
                ++pos_;
            } else {
                break;
            }
        }
        t->type = Tok::Number;
        t->text = s_.substr(start, pos_ - start);
        if (!str::parse_double(t->text, &t->number)) throw FormulaError("bad number", start);
        t->integral = !dot;
    }

    void lex_string(Token* t) {
        const char quote = s_[pos_];
        const size_t start = pos_;
        ++pos_;
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) throw FormulaError("unterminated string", start);
            const char c = s_[pos_++];
            if (c == quote) break;
            if (c == '\\' && pos_ < s_.size()) {
                const char e = s_[pos_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    default: out += e; break;
                }
                continue;
            }
            out += c;
        }
        t->type = Tok::String;
        t->text = std::move(out);
    }

    void lex_ident(Token* t) {
        const size_t start = pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$') {
                ++pos_;
            } else {
                break;
            }
        }
        t->type = Tok::Ident;
        t->text = s_.substr(start, pos_ - start);
        if (t->text.back() == '.') throw FormulaError("field name can't end with '.'", pos_ - 1);
    }

    void lex_operator(Token* t) {
        static const char* const kOps[] = {"&&", "||", "==", "!=", "<>", "<=", ">=", "+", "-", "*", "/",
                                           "^",  "&",  "=",  "<",  ">",  "!"};
        for (const char* op : kOps) {
            const size_t n = std::char_traits<char>::length(op);
            if (s_.compare(pos_, n, op) == 0) {
                t->type = Tok::Op;
                t->text = op;
                pos_ += n;
                return;
            }
        }
        throw FormulaError(std::string("unexpected character '") + s_[pos_] + "'", pos_);
    }

    const std::string& s_;
    size_t pos_ = 0;
};

// Precedence climbing, lowest first:
//   || | && | = == != <> | < <= > >= | & | + - | * / | ^ | unary - !
class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    NodePtr parse() {
        NodePtr n = parse_or();
        if (peek().type != Tok::End) throw FormulaError("unexpected '" + peek().text + "'", peek().pos);
        return n;
    }

private:
    const Token& peek() const { return toks_[i_]; }
    const Token& next() { return toks_[i_++]; }

    bool accept_op(const char* op) {
        if (peek().type == Tok::Op && peek().text == op) {
            ++i_;
            return true;
        }
        return false;
    }

    bool accept_word(const char* word) {
        if (peek().type == Tok::Ident && str::iequals(peek().text, word)) {
            // Only treat AND/OR/NOT as operators when not followed by '('.
            if (toks_[i_ + 1].type == Tok::LParen) return false;
            ++i_;
            return true;
        }
        return false;
    }

    static NodePtr binary(std::string op, NodePtr l, NodePtr r, size_t pos) {
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::Binary;
        n->name = std::move(op);
        n->pos = pos;
        n->args.push_back(std::move(l));
        n->args.push_back(std::move(r));
        return n;
    }

    NodePtr parse_or() {
        NodePtr l = parse_and();
        while (true) {
            const size_t pos = peek().pos;
            if (accept_op("||") || accept_word("OR")) {
                l = binary("||", std::move(l), parse_and(), pos);
            } else {
                return l;
            }
        }
    }

    NodePtr parse_and() {
        NodePtr l = parse_equality();
        while (true) {
            const size_t pos = peek().pos;
            if (accept_op("&&") || accept_word("AND")) {
                l = binary("&&", std::move(l), parse_equality(), pos);
            } else {
                return l;
            }
        }
    }

    NodePtr parse_equality() {
        NodePtr l = parse_relational();
        while (true) {
            const size_t pos = peek().pos;
            if (accept_op("=") || accept_op("==")) {
                l = binary("=", std::move(l), parse_relational(), pos);
            } else if (accept_op("!=") || accept_op("<>")) {
                l = binary("!=", std::move(l), parse_relational(), pos);
            } else {
                return l;
            }
        }
    }

    NodePtr parse_relational() {
        NodePtr l = parse_concat();
        while (true) {
            const size_t pos = peek().pos;
            if (accept_op("<=")) l = binary("<=", std::move(l), parse_concat(), pos);
            else if (accept_op(">=")) l = binary(">=", std::move(l), parse_concat(), pos);
            else if (accept_op("<")) l = binary("<", std::move(l), parse_concat(), pos);
            else if (accept_op(">")) l = binary(">", std::move(l), parse_concat(), pos);
            else return l;
        }
    }

    NodePtr parse_concat() {
        NodePtr l = parse_additive();
        while (true) {
            const size_t pos = peek().pos;
            if (accept_op("&")) {
                l = binary("&", std::move(l), parse_additive(), pos);
            } else {
                return l;
            }
        }
    }

    NodePtr parse_additive() {
        NodePtr l = parse_multiplicative();
        while (true) {
            const size_t pos = peek().pos;
            if (accept_op("+")) l = binary("+", std::move(l), parse_multiplicative(), pos);
            else if (accept_op("-")) l = binary("-", std::move(l), parse_multiplicative(), pos);
            else return l;
        }
    }

    NodePtr parse_multiplicative() {
        NodePtr l = parse_power();
        while (true) {
            const size_t pos = peek().pos;
            if (accept_op("*")) l = binary("*", std::move(l), parse_power(), pos);
            else if (accept_op("/")) l = binary("/", std::move(l), parse_power(), pos);
            else return l;
        }
    }

    NodePtr parse_power() {
        NodePtr l = parse_unary();
        const size_t pos = peek().pos;
        if (accept_op("^")) return binary("^", std::move(l), parse_power(), pos);
        return l;
    }

    NodePtr parse_unary() {
        const size_t pos = peek().pos;
        if (accept_op("-") || accept_op("!") || accept_word("NOT")) {
            const std::string op = toks_[i_ - 1].text == "-" ? "neg" : "!";
            auto n = std::make_unique<Node>();
            n->kind = Node::Kind::Unary;
            n->name = op;
            n->pos = pos;
            n->args.push_back(parse_unary());
            return n;
        }
        if (accept_op("+")) return parse_unary();
        return parse_primary();
    }

    NodePtr parse_primary() {
        const Token& t = next();
        auto n = std::make_unique<Node>();
        n->pos = t.pos;
        switch (t.type) {
            case Tok::Number:
                n->kind = Node::Kind::Literal;
                n->literal = t.integral && t.number < 9e15 ? FieldValue::integer(static_cast<int64_t>(t.number))
                                                            : FieldValue::number(t.number);
                return n;
            case Tok::String:
                n->kind = Node::Kind::Literal;
                n->literal = FieldValue::string(t.text);
                return n;
            case Tok::LParen: {
                NodePtr inner = parse_or();
                if (next().type != Tok::RParen) throw FormulaError("expected ')'", toks_[i_ - 1].pos);
                return inner;
            }
            case Tok::Ident: {
                if (peek().type == Tok::LParen) {
                    ++i_;
                    n->kind = Node::Kind::Call;
                    n->name = str::to_upper(t.text);
                    if (peek().type != Tok::RParen) {
                        while (true) {
                            n->args.push_back(parse_or());
                            if (peek().type == Tok::Comma) {
                                ++i_;
                                continue;
                            }
                            break;
                        }
                    }
                    if (next().type != Tok::RParen) throw FormulaError("expected ')' after arguments", t.pos);
                    check_function(*n);
                    return n;
                }
                if (str::iequals(t.text, "true") || str::iequals(t.text, "false")) {
                    n->kind = Node::Kind::Literal;
                    n->literal = FieldValue::boolean(str::iequals(t.text, "true"));
                    return n;
                }
                if (str::iequals(t.text, "null")) {
                    n->kind = Node::Kind::Literal;
                    return n;
                }
                n->kind = Node::Kind::Field;
                n->name = t.text;
                return n;
            }
            case Tok::End: throw FormulaError("unexpected end of formula", t.pos);
            default: throw FormulaError("unexpected '" + t.text + "'", t.pos);
        }
    }

    static void check_function(const Node& n) {
        const FunctionInfo* info = find_function(n.name);
        if (info == nullptr) throw FormulaError("unknown function " + n.name, n.pos);
        const int argc = static_cast<int>(n.args.size());
        if (argc < info->min_args || (info->max_args >= 0 && argc > info->max_args)) {
            throw FormulaError("wrong number of arguments to " + n.name, n.pos);
        }
    }

    std::vector<Token> toks_;
    size_t i_ = 0;
};

void collect_fields(const Node& n, std::set<std::string>* out) {
    if (n.kind == Node::Kind::Field && !n.name.empty() && n.name[0] != '$') out->insert(n.name);
    for (const NodePtr& a : n.args) collect_fields(*a, out);
}

}  // namespace

Formula Formula::compile(const std::string& source) {
    Formula f;
    f.source_ = source;
    Lexer lexer(source);
    Parser parser(lexer.run());
    f.root_ = std::shared_ptr<const Node>(parser.parse().release());
    return f;
}

std::vector<std::string> Formula::referenced_fields() const {
    std::set<std::string> fields;
    if (root_) collect_fields(*root_, &fields);
    return {fields.begin(), fields.end()};
}

FieldValue Formula::evaluate(Context& ctx) const {
    if (!root_) return FieldValue::null();
    return eval_node(*root_, ctx);
}

bool truthy(const FieldValue& v) {
    return v.kind() == ValueKind::Bool && v.as_bool();
}

}  // namespace tcrm::formula
