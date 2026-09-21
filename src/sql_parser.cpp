// SQL subset parser.
// Phase 1 grammar (single Salesforce object):
//   SELECT [TOP n] items [FROM object [alias]] [WHERE cond] [ORDER BY ...]
//          [LIMIT n] [OFFSET m [ROWS]] [FETCH FIRST|NEXT n ROWS ONLY]
// Conditions: = != <> < <= > >= LIKE [NOT] IN (...) IS [NOT] NULL BETWEEN AND OR NOT, parameters (?),
// ODBC escapes {d '...'} {t '...'} {ts '...'}.
#include "sql.h"

namespace sf {
namespace {

enum class Tok { End, Ident, QIdent, String, Number, Param, Op, DateLit, TimeLit, TsLit };

struct Token {
  Tok t;
  std::string v;
  size_t pos;
};

[[noreturn]] void syntax(const std::string& msg, size_t pos) {
  throw OdbcError("42000", "SQL syntax error at position " + std::to_string(pos + 1) + ": " + msg);
}
[[noreturn]] void unsupported(const std::string& what) {
  throw OdbcError("42000", what + " is not supported yet (single-object SELECT only in this release).");
}

std::vector<Token> tokenize(const std::string& s) {
  std::vector<Token> out;
  size_t i = 0;
  auto quoted = [&](char close, bool doubled) {
    std::string v;
    size_t start = i++;
    while (true) {
      if (i >= s.size()) syntax("unterminated quoted text", start);
      if (s[i] == close) {
        if (doubled && i + 1 < s.size() && s[i + 1] == close) { v += close; i += 2; continue; }
        ++i;
        break;
      }
      v += s[i++];
    }
    return v;
  };
  auto skip_ws = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };

  while (true) {
    skip_ws();
    if (i >= s.size()) break;
    char c = s[i];
    size_t pos = i;
    if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      size_t e = s.find("*/", i + 2);
      i = e == std::string::npos ? s.size() : e + 2;
      continue;
    }
    if (c == '\'') { out.push_back({Tok::String, quoted('\'', true), pos}); continue; }
    if (c == '"') { out.push_back({Tok::QIdent, quoted('"', true), pos}); continue; }
    if (c == '`') { out.push_back({Tok::QIdent, quoted('`', true), pos}); continue; }
    if (c == '[') { out.push_back({Tok::QIdent, quoted(']', false), pos}); continue; }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      size_t st = i;
      while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
      if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
      }
      out.push_back({Tok::Number, s.substr(st, i - st), pos});
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
      size_t st = i;
      while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '$' ||
                              static_cast<unsigned char>(s[i]) >= 0x80))
        ++i;
      out.push_back({Tok::Ident, s.substr(st, i - st), pos});
      continue;
    }
    if (c == '?') { out.push_back({Tok::Param, "?", pos}); ++i; continue; }
    if (c == '{') {
      ++i;
      skip_ws();
      size_t st = i;
      while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
      std::string kw = to_lower(s.substr(st, i - st));
      if (kw != "d" && kw != "t" && kw != "ts") unsupported("ODBC escape {" + kw + " ...}");
      skip_ws();
      if (i >= s.size() || s[i] != '\'') syntax("expected quoted literal in escape", i);
      std::string v = quoted('\'', true);
      skip_ws();
      if (i >= s.size() || s[i] != '}') syntax("expected } to close escape", i);
      ++i;
      out.push_back({kw == "d" ? Tok::DateLit : kw == "t" ? Tok::TimeLit : Tok::TsLit, v, pos});
      continue;
    }
    if (i + 1 < s.size()) {
      std::string two = s.substr(i, 2);
      if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
        out.push_back({Tok::Op, two, pos});
        i += 2;
        continue;
      }
    }
    if (std::strchr("=<>(),.*;-+", c)) {
      out.push_back({Tok::Op, std::string(1, c), pos});
      ++i;
      continue;
    }
    syntax(std::string("unexpected character '") + c + "'", pos);
  }
  out.push_back({Tok::End, "", s.size()});
  return out;
}

const char* kReserved[] = {"SELECT", "FROM",  "WHERE", "ORDER", "GROUP",   "HAVING", "LIMIT", "OFFSET",
                           "FETCH",  "UNION", "JOIN",  "INNER", "LEFT",    "RIGHT",  "FULL",  "CROSS",
                           "ON",     "AND",   "OR",    "NOT",   "AS",      "BY",     "ASC",   "DESC",
                           "NULLS",  "IN",    "LIKE",  "IS",    "NULL",    "BETWEEN", "TOP",  "DISTINCT",
                           "EXCEPT", "INTERSECT", "ROWS", "ROW", "ONLY", "ESCAPE"};

class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : t_(std::move(toks)) {}

  SelectStmt parse() {
    SelectStmt st;
    if (is_kw("WITH")) unsupported("WITH (common table expressions)");
    if (!is_kw("SELECT")) {
      if (is_kw("INSERT") || is_kw("UPDATE") || is_kw("DELETE") || is_kw("MERGE") || is_kw("UPSERT"))
        throw OdbcError("HY000", "This driver release is read-only; " + to_upper(peek().v) + " is not supported.");
      syntax("expected SELECT", peek().pos);
    }
    next();
    if (accept_kw("DISTINCT")) unsupported("SELECT DISTINCT");
    if (accept_kw("TOP")) {
      bool paren = accept_op("(");
      st.limit = parse_int("TOP");
      if (paren) expect_op(")");
    }
    parse_items(st);
    if (accept_kw("FROM")) {
      if (is_op("(")) unsupported("Subqueries");
      auto at_join = [&] {
        return is_op(",") || is_kw("JOIN") || is_kw("INNER") || is_kw("LEFT") || is_kw("RIGHT") ||
               is_kw("FULL") || is_kw("CROSS");
      };
      st.table = parse_path();
      if (at_join()) unsupported("Joins");
      if (accept_kw("AS")) st.table_alias = ident("table alias");
      else if (is_ident_nonreserved()) st.table_alias = ident("table alias");
      if (at_join()) unsupported("Joins");  // alias came first: "FROM Account a JOIN ..."
    }
    if (accept_kw("WHERE")) st.where = parse_or();
    if (is_kw("GROUP") || is_kw("HAVING")) unsupported("GROUP BY / HAVING");
    if (accept_kw("ORDER")) {
      expect_kw("BY");
      do {
        OrderItem oi;
        if (peek().t == Tok::Number) oi.ordinal = static_cast<int>(parse_int("ORDER BY ordinal"));
        else oi.path = parse_path();
        if (accept_kw("DESC")) oi.desc = true;
        else accept_kw("ASC");
        if (accept_kw("NULLS")) {
          if (accept_kw("FIRST")) oi.nulls = "FIRST";
          else { expect_kw("LAST"); oi.nulls = "LAST"; }
        }
        st.order_by.push_back(std::move(oi));
      } while (accept_op(","));
    }
    if (accept_kw("LIMIT")) {
      st.limit = parse_int("LIMIT");
      if (accept_kw("OFFSET")) st.offset = parse_int("OFFSET");
    }
    if (accept_kw("OFFSET")) {
      st.offset = parse_int("OFFSET");
      if (!accept_kw("ROWS")) accept_kw("ROW");
    }
    if (accept_kw("FETCH")) {
      if (!accept_kw("FIRST")) expect_kw("NEXT");
      st.limit = parse_int("FETCH");
      if (!accept_kw("ROWS")) expect_kw("ROW");
      expect_kw("ONLY");
    }
    accept_op(";");
    if (is_kw("UNION") || is_kw("EXCEPT") || is_kw("INTERSECT")) unsupported("UNION/EXCEPT/INTERSECT");
    if (peek().t != Tok::End) syntax("unexpected '" + peek().v + "'", peek().pos);
    st.param_count = params_;
    return st;
  }

 private:
  const Token& peek(size_t k = 0) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
  const Token& next() { return t_[std::min(p_++, t_.size() - 1)]; }
  bool is_kw(const char* kw, size_t k = 0) const { return peek(k).t == Tok::Ident && iequals(peek(k).v, kw); }
  bool accept_kw(const char* kw) { if (is_kw(kw)) { next(); return true; } return false; }
  void expect_kw(const char* kw) { if (!accept_kw(kw)) syntax(std::string("expected ") + kw, peek().pos); }
  bool is_op(const char* op, size_t k = 0) const { return peek(k).t == Tok::Op && peek(k).v == op; }
  bool accept_op(const char* op) { if (is_op(op)) { next(); return true; } return false; }
  void expect_op(const char* op) { if (!accept_op(op)) syntax(std::string("expected '") + op + "'", peek().pos); }

  static bool reserved(const std::string& w) {
    for (const char* r : kReserved) if (iequals(w, r)) return true;
    return false;
  }
  bool is_ident_nonreserved() const {
    return peek().t == Tok::QIdent || (peek().t == Tok::Ident && !reserved(peek().v));
  }
  std::string ident(const char* what) {
    if (peek().t == Tok::QIdent || peek().t == Tok::Ident) return next().v;
    syntax(std::string("expected ") + what, peek().pos);
  }
  long long parse_int(const char* what) {
    if (peek().t != Tok::Number) syntax(std::string("expected integer after ") + what, peek().pos);
    try { return std::stoll(next().v); } catch (...) { syntax("invalid integer", peek().pos); }
  }
  std::vector<std::string> parse_path() {
    std::vector<std::string> path{ident("identifier")};
    while (is_op(".") && (peek(1).t == Tok::Ident || peek(1).t == Tok::QIdent)) {
      next();
      path.push_back(next().v);
    }
    return path;
  }

  bool is_literal_start() const {
    Tok k = peek().t;
    return k == Tok::String || k == Tok::Number || k == Tok::DateLit || k == Tok::TimeLit || k == Tok::TsLit ||
           (k == Tok::Op && (peek().v == "-" || peek().v == "+") && peek(1).t == Tok::Number) ||
           is_kw("NULL") || is_kw("TRUE") || is_kw("FALSE");
  }

  Lit parse_literal() {
    Lit l;
    const Token& tk = peek();
    if (tk.t == Tok::Op) {
      std::string sign = next().v == "-" ? "-" : "";
      l.kind = Lit::Number;
      l.text = sign + next().v;
    } else if (tk.t == Tok::Number) { l.kind = Lit::Number; l.text = next().v; }
    else if (tk.t == Tok::String) { l.kind = Lit::String; l.text = next().v; }
    else if (tk.t == Tok::DateLit) { l.kind = Lit::Date; l.text = next().v; }
    else if (tk.t == Tok::TimeLit) { l.kind = Lit::Time; l.text = next().v; }
    else if (tk.t == Tok::TsLit) { l.kind = Lit::Timestamp; l.text = next().v; }
    else if (accept_kw("NULL")) { l.kind = Lit::Null; }
    else if (accept_kw("TRUE")) { l.kind = Lit::Bool; l.text = "true"; }
    else if (accept_kw("FALSE")) { l.kind = Lit::Bool; l.text = "false"; }
    else syntax("expected literal", tk.pos);
    return l;
  }

  void parse_items(SelectStmt& st) {
    do {
      SelectItem it;
      if (accept_op("*")) {
        it.kind = SelectItem::Star;
      } else if (is_kw("COUNT") && is_op("(", 1)) {
        next(); next();
        if (!accept_op("*")) unsupported("COUNT(column)");
        expect_op(")");
        it.kind = SelectItem::CountStar;
      } else if (is_literal_start()) {
        it.kind = SelectItem::Literal;
        it.lit = parse_literal();
      } else if (peek().t == Tok::Param) {
        unsupported("Parameters in the select list");
      } else {
        if (peek().t == Tok::Ident && is_op("(", 1)) unsupported("Function " + to_upper(peek().v) + "()");
        it.kind = SelectItem::Column;
        it.path = parse_path();
        if (is_op(".") && is_op("*", 1)) {  // alias.*
          next(); next();
          it.kind = SelectItem::Star;
          it.path.clear();
        }
      }
      if (is_op("+") || is_op("-") || is_op("*") || (peek().t == Tok::Op && peek().v == "/"))
        unsupported("Expressions in the select list");
      if (accept_kw("AS")) it.alias = ident("column alias");
      else if (is_ident_nonreserved()) it.alias = next().v;
      st.items.push_back(std::move(it));
    } while (accept_op(","));
  }

  Operand parse_operand() {
    Operand o;
    if (peek().t == Tok::Param) {
      next();
      o.kind = Operand::Param;
      o.param_index = params_++;
    } else if (is_literal_start()) {
      o.kind = Operand::Literal;
      o.lit = parse_literal();
    } else if (peek().t == Tok::Ident || peek().t == Tok::QIdent) {
      if (peek().t == Tok::Ident && is_op("(", 1)) unsupported("Function " + to_upper(peek().v) + "()");
      if (peek().t == Tok::Ident && is_kw("SELECT")) unsupported("Subqueries");
      o.kind = Operand::Column;
      o.path = parse_path();
    } else {
      syntax("expected column, literal or ?", peek().pos);
    }
    if (is_op("+") || is_op("-") || is_op("*")) unsupported("Arithmetic expressions");
    return o;
  }

  ExprPtr parse_or() {
    ExprPtr l = parse_and();
    while (accept_kw("OR")) {
      auto e = std::make_shared<Expr>();
      e->kind = Expr::Or; e->left = l; e->right = parse_and();
      l = e;
    }
    return l;
  }
  ExprPtr parse_and() {
    ExprPtr l = parse_not();
    while (accept_kw("AND")) {
      auto e = std::make_shared<Expr>();
      e->kind = Expr::And; e->left = l; e->right = parse_not();
      l = e;
    }
    return l;
  }
  ExprPtr parse_not() {
    if (accept_kw("NOT")) {
      auto e = std::make_shared<Expr>();
      e->kind = Expr::Not; e->left = parse_not();
      return e;
    }
    return parse_predicate();
  }

  ExprPtr parse_predicate() {
    if (is_kw("EXISTS")) unsupported("EXISTS");
    if (accept_op("(")) {
      if (is_kw("SELECT")) unsupported("Subqueries");
      ExprPtr e = parse_or();
      expect_op(")");
      return e;
    }
    auto e = std::make_shared<Expr>();
    e->a = parse_operand();
    const Token& tk = peek();
    if (tk.t == Tok::Op && (tk.v == "=" || tk.v == "!=" || tk.v == "<>" || tk.v == "<" || tk.v == "<=" ||
                            tk.v == ">" || tk.v == ">=")) {
      e->kind = Expr::Compare;
      e->op = next().v == "<>" ? "!=" : t_[p_ - 1].v;
      e->b = parse_operand();
      return e;
    }
    bool neg = accept_kw("NOT");
    if (accept_kw("IN")) {
      e->kind = Expr::In;
      e->negated = neg;
      expect_op("(");
      if (is_kw("SELECT")) unsupported("IN (subquery)");
      do e->list.push_back(parse_operand()); while (accept_op(","));
      expect_op(")");
      return e;
    }
    if (accept_kw("LIKE")) {
      e->kind = Expr::Like;
      e->negated = neg;
      e->b = parse_operand();
      if (is_kw("ESCAPE")) unsupported("LIKE ... ESCAPE");
      return e;
    }
    if (accept_kw("BETWEEN")) {
      Operand lo = parse_operand();
      expect_kw("AND");
      Operand hi = parse_operand();
      auto ge = std::make_shared<Expr>(); ge->kind = Expr::Compare; ge->op = ">="; ge->a = e->a; ge->b = lo;
      auto le = std::make_shared<Expr>(); le->kind = Expr::Compare; le->op = "<="; le->a = e->a; le->b = hi;
      auto both = std::make_shared<Expr>(); both->kind = Expr::And; both->left = ge; both->right = le;
      if (!neg) return both;
      auto n = std::make_shared<Expr>(); n->kind = Expr::Not; n->left = both;
      return n;
    }
    if (neg) syntax("expected IN, LIKE or BETWEEN after NOT", peek().pos);
    if (accept_kw("IS")) {
      e->kind = Expr::IsNull;
      e->negated = accept_kw("NOT");
      expect_kw("NULL");
      return e;
    }
    syntax("expected comparison operator", peek().pos);
  }

  std::vector<Token> t_;
  size_t p_ = 0;
  int params_ = 0;
};

}  // namespace

SelectStmt parse_sql(const std::string& sql) { return Parser(tokenize(sql)).parse(); }

}  // namespace sf
