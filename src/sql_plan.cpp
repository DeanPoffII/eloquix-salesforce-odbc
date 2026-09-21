// Translates the parsed SQL into a SOQL template, resolving names and types against
// Salesforce metadata. Relationship paths (Account.Owner.Name) are supported.
#include "sql.h"

#include <cstdlib>

namespace sf {
namespace {

using Part = std::variant<std::string, ParamSlot>;
using Parts = std::vector<Part>;

struct Resolved {
  std::string soql;                    // canonical SOQL path
  std::vector<std::string> json_path;  // record keys
  const FieldMeta* field = nullptr;
  ColumnInfo col;
};

struct Ctx {
  SalesforceClient& client;
  MetadataCache& meta;
  std::shared_ptr<const ObjectMeta> root;
  std::string alias;
  std::vector<std::shared_ptr<const ObjectMeta>> keep_alive;
  std::vector<ParamSlot> params;
};

std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
  return out;
}

Resolved resolve(Ctx& ctx, std::vector<std::string> path) {
  if (path.size() > 1 && ((!ctx.alias.empty() && iequals(path[0], ctx.alias)) ||
                           (iequals(path[0], ctx.root->name) && !ctx.root->relationship(path[0]))))
    path.erase(path.begin());
  if (path.size() > 6) throw OdbcError("42000", "Relationship path too deep: " + join(path, "."));

  const ObjectMeta* obj = ctx.root.get();
  Resolved r;
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    const FieldMeta* rel = obj->relationship(path[i]);
    if (!rel || rel->reference_to.empty())
      throw OdbcError("42S22", "No relationship named '" + path[i] + "' on " + obj->name);
    r.json_path.push_back(rel->relationship_name);
    const ObjectMeta* next = nullptr;
    auto metas = ctx.meta.objects_meta(ctx.client, rel->reference_to);
    for (auto& m : metas) {  // polymorphic lookups: first target that has the next name
      bool is_leaf = i + 2 == path.size();
      if ((is_leaf && m->field(path[i + 1])) || (!is_leaf && m->relationship(path[i + 1]))) {
        next = m.get();
        ctx.keep_alive.push_back(m);
        break;
      }
    }
    if (!next) throw OdbcError("42S22", "Cannot resolve '" + join(path, ".") + "'");
    obj = next;
  }
  const FieldMeta* f = obj->field(path.back());
  if (!f) throw OdbcError("42S22", "Column '" + join(path, ".") + "' not found on " + ctx.root->name);
  r.json_path.push_back(f->name);
  r.soql = join(r.json_path, ".");
  r.field = f;
  r.col = column_from_field(*f, obj->name);
  r.col.json_path = r.json_path;
  r.col.name = r.soql;
  if (path.size() > 1) r.col.nullable = SQL_NULLABLE;  // parent may be null
  return r;
}

// ---------------------------------------------------------------- WHERE

struct Frag {
  bool is_const = false;
  bool value = false;
  Parts parts;
};

Frag const_frag(bool v) { Frag f; f.is_const = true; f.value = v; return f; }
Frag text_frag(std::string s) { Frag f; f.parts.push_back(std::move(s)); return f; }
void append(Parts& out, const Parts& in) { out.insert(out.end(), in.begin(), in.end()); }
void append(Parts& out, std::string s) { out.push_back(std::move(s)); }

bool fold_compare(const Lit& a, const std::string& op, const Lit& b) {
  if (a.kind == Lit::Null || b.kind == Lit::Null) return false;  // unknown -> not true
  int cmp;
  char* e1 = nullptr;
  char* e2 = nullptr;
  double da = std::strtod(a.text.c_str(), &e1), db = std::strtod(b.text.c_str(), &e2);
  if (!a.text.empty() && !b.text.empty() && *e1 == 0 && *e2 == 0) cmp = da < db ? -1 : da > db ? 1 : 0;
  else cmp = a.text.compare(b.text);
  if (op == "=") return cmp == 0;
  if (op == "!=") return cmp != 0;
  if (op == "<") return cmp < 0;
  if (op == "<=") return cmp <= 0;
  if (op == ">") return cmp > 0;
  return cmp >= 0;
}

std::string flip(const std::string& op) {
  if (op == "<") return ">";
  if (op == ">") return "<";
  if (op == "<=") return ">=";
  if (op == ">=") return "<=";
  return op;
}

Part value_part(Ctx& ctx, const Operand& o, const Resolved& col) {
  if (o.kind == Operand::Column)
    throw OdbcError("42000", "Comparing two columns is not supported by SOQL (" + col.soql + ")");
  if (o.kind == Operand::Param) {
    ParamSlot slot{o.param_index, col.field->type, col.soql};
    if (static_cast<int>(ctx.params.size()) <= o.param_index) ctx.params.resize(o.param_index + 1, {-1, "", ""});
    ctx.params[o.param_index] = slot;
    return slot;
  }
  return soql_literal(o.lit, col.field->type, col.soql);
}

Frag render(Ctx& ctx, const Expr& e) {
  switch (e.kind) {
    case Expr::And:
    case Expr::Or: {
      bool is_and = e.kind == Expr::And;
      Frag l = render(ctx, *e.left), r = render(ctx, *e.right);
      if (l.is_const && r.is_const) return const_frag(is_and ? (l.value && r.value) : (l.value || r.value));
      if (l.is_const) return (l.value == is_and) ? r : const_frag(l.value);
      if (r.is_const) return (r.value == is_and) ? l : const_frag(r.value);
      Frag f;
      append(f.parts, "(");
      append(f.parts, l.parts);
      append(f.parts, is_and ? " AND " : " OR ");
      append(f.parts, r.parts);
      append(f.parts, ")");
      return f;
    }
    case Expr::Not: {
      Frag in = render(ctx, *e.left);
      if (in.is_const) return const_frag(!in.value);
      Frag f;
      append(f.parts, "(NOT ");
      append(f.parts, in.parts);
      append(f.parts, ")");
      return f;
    }
    case Expr::Compare: {
      Operand a = e.a, b = e.b;
      std::string op = e.op;
      if (a.kind != Operand::Column && b.kind == Operand::Column) { std::swap(a, b); op = flip(op); }
      if (a.kind != Operand::Column) {
        if (a.kind == Operand::Param || b.kind == Operand::Param)
          throw OdbcError("42000", "A parameter must be compared with a column");
        return const_frag(fold_compare(a.lit, op, b.lit));
      }
      Resolved col = resolve(ctx, a.path);
      if (b.kind == Operand::Literal && b.lit.kind == Lit::Null) {
        if (op == "=" || op == "!=") return text_frag(col.soql + " " + op + " null");
        return const_frag(false);
      }
      Frag f;
      append(f.parts, col.soql + " " + op + " ");
      f.parts.push_back(value_part(ctx, b, col));
      return f;
    }
    case Expr::IsNull: {
      if (e.a.kind != Operand::Column) {
        bool is_null = e.a.kind == Operand::Literal && e.a.lit.kind == Lit::Null;
        if (e.a.kind == Operand::Param) throw OdbcError("42000", "? IS NULL is not supported");
        return const_frag(e.negated ? !is_null : is_null);
      }
      Resolved col = resolve(ctx, e.a.path);
      return text_frag(col.soql + (e.negated ? " != null" : " = null"));
    }
    case Expr::Like: {
      if (e.a.kind != Operand::Column) throw OdbcError("42000", "LIKE requires a column on the left");
      Resolved col = resolve(ctx, e.a.path);
      Frag f;
      append(f.parts, std::string(e.negated ? "(NOT " : "") + col.soql + " LIKE ");
      f.parts.push_back(value_part(ctx, e.b, col));
      if (e.negated) append(f.parts, ")");
      return f;
    }
    case Expr::In: {
      if (e.a.kind != Operand::Column) throw OdbcError("42000", "IN requires a column on the left");
      Resolved col = resolve(ctx, e.a.path);
      Frag f;
      append(f.parts, col.soql + (e.negated ? " NOT IN (" : " IN ("));
      for (size_t i = 0; i < e.list.size(); ++i) {
        if (i) append(f.parts, ",");
        f.parts.push_back(value_part(ctx, e.list[i], col));
      }
      append(f.parts, ")");
      return f;
    }
  }
  throw OdbcError("HY000", "internal: unknown expression");
}

ColumnInfo literal_column(const Lit& l, const std::string& name) {
  ColumnInfo c;
  switch (l.kind) {
    case Lit::Number:
      c = l.text.find_first_of(".eE") == std::string::npos ? make_column(name, SQL_BIGINT)
                                                          : make_column(name, SQL_DOUBLE, 15);
      break;
    case Lit::Bool:
      c = make_column(name, SQL_BIT);
      break;
    case Lit::Date:
      c = make_column(name, SQL_TYPE_DATE);
      break;
    case Lit::Timestamp:
      c = make_column(name, SQL_TYPE_TIMESTAMP, 23, 3);
      break;
    default:
      c = make_column(name, SQL_WVARCHAR, std::max<size_t>(l.text.size(), 1));
  }
  c.is_const = true;
  if (l.kind == Lit::Null) c.const_value = std::nullopt;
  else if (l.kind == Lit::Bool) c.const_value = std::string(l.text == "true" ? "1" : "0");
  else c.const_value = l.text;
  c.nullable = l.kind == Lit::Null ? SQL_NULLABLE : SQL_NO_NULLS;
  return c;
}

}  // namespace

// ---------------------------------------------------------------- literals

std::string soql_literal(const Lit& lit, const std::string& t, const std::string& field) {
  if (lit.kind == Lit::Null) return "null";
  const std::string& s = lit.text;
  auto quote = [](const std::string& v) {
    std::string out = "'";
    for (char c : v) {
      switch (c) {
        case '\'': out += "\\'"; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
      }
    }
    return out + "'";
  };
  auto bad = [&](const char* state, const char* what) -> OdbcError {
    return OdbcError(state, std::string("Invalid ") + what + " value '" + s + "' for " + field);
  };

  if (t == "boolean") {
    std::string v = to_lower(trim(s));
    if (v == "true" || v == "1") return "true";
    if (v == "false" || v == "0") return "false";
    throw bad("22018", "boolean");
  }
  if (t == "int" || t == "long" || t == "double" || t == "currency" || t == "percent") {
    std::string v = trim(s);
    if (lit.kind == Lit::Bool) return v == "true" ? "1" : "0";
    char* end = nullptr;
    std::strtod(v.c_str(), &end);
    if (v.empty() || *end != 0) throw bad("22018", "numeric");
    return v;
  }
  if (t == "date" || t == "datetime" || t == "time") {
    DateTimeParts p;
    if (!parse_datetime(s, p)) throw bad("22007", t.c_str());
    if (t == "date") {
      if (!p.has_date) throw bad("22007", "date");
      return format_date(p);
    }
    if (t == "datetime") {
      if (!p.has_date) throw bad("22007", "datetime");
      char b[32];
      std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", p.year, p.month, p.day, p.hour, p.minute,
                    p.second);
      return b;
    }
    if (!p.has_time) throw bad("22007", "time");
    char b[24];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d.%03dZ", p.hour, p.minute, p.second, p.millis);
    return b;
  }
  return quote(s);
}

// ---------------------------------------------------------------- plan

QueryPlan build_plan(const std::string& sql, SalesforceClient& client, MetadataCache& meta) {
  SelectStmt st = parse_sql(sql);
  QueryPlan plan;
  plan.param_count = st.param_count;
  plan.limit = st.limit;
  plan.offset = st.offset;

  if (st.table.empty()) {  // SELECT 1, 'x'  (connection tests)
    plan.kind = QueryPlan::Constant;
    int n = 0;
    for (auto& it : st.items) {
      if (it.kind != SelectItem::Literal) throw OdbcError("42000", "SELECT without FROM may only contain literals");
      plan.columns.push_back(literal_column(it.lit, it.alias.empty() ? "EXPR_" + std::to_string(n) : it.alias));
      ++n;
    }
    if (st.where || st.param_count) throw OdbcError("42000", "WHERE requires FROM");
    return plan;
  }

  std::string obj_name = meta.resolve_object_name(client, st.table.back());
  if (obj_name.empty()) throw OdbcError("42S02", "Salesforce object not found: " + st.table.back());
  Ctx ctx{client, meta, meta.object(client, obj_name), st.table_alias, {}, {}};
  plan.object_name = obj_name;

  // --- select list
  std::vector<std::string> soql_fields;
  auto add_field = [&](const std::string& f) {
    for (auto& x : soql_fields) if (iequals(x, f)) return;
    soql_fields.push_back(f);
  };
  bool has_count = false;
  int expr_n = 0;
  for (auto& it : st.items) {
    switch (it.kind) {
      case SelectItem::Star:
        for (auto& f : ctx.root->fields) {
          plan.columns.push_back(column_from_field(f, obj_name));
          add_field(f.name);
        }
        break;
      case SelectItem::Column: {
        Resolved r = resolve(ctx, it.path);
        if (!it.alias.empty()) r.col.name = it.alias;
        plan.columns.push_back(r.col);
        add_field(r.soql);
        break;
      }
      case SelectItem::CountStar: {
        has_count = true;
        ColumnInfo c = make_column(it.alias.empty() ? "EXPR_" + std::to_string(expr_n) : it.alias, SQL_BIGINT, 19,
                                   0, SQL_NO_NULLS);
        plan.columns.push_back(c);
        break;
      }
      case SelectItem::Literal:
        plan.columns.push_back(
            literal_column(it.lit, it.alias.empty() ? "EXPR_" + std::to_string(expr_n) : it.alias));
        break;
    }
    ++expr_n;
  }
  if (has_count && st.items.size() != 1)
    throw OdbcError("42000", "COUNT(*) must be the only item in the select list (GROUP BY is not supported yet)");

  // --- where
  Frag where;
  if (st.where) {
    where = render(ctx, *st.where);
    if (where.is_const && !where.value) plan.always_empty = true;
  }

  // --- assemble template
  Parts& out = plan.soql;
  if (has_count) {
    plan.kind = QueryPlan::Count;
    out.push_back("SELECT COUNT() FROM " + obj_name);
  } else {
    if (soql_fields.empty()) soql_fields.push_back("Id");
    out.push_back("SELECT " + join(soql_fields, ",") + " FROM " + obj_name);
  }
  if (st.where && !where.is_const) {
    out.push_back(" WHERE ");
    append(out, where.parts);
  }
  if (!st.order_by.empty() && !has_count) {
    std::vector<std::string> keys;
    for (auto& oi : st.order_by) {
      std::string path;
      if (oi.ordinal > 0) {
        if (oi.ordinal > static_cast<int>(plan.columns.size()) || plan.columns[oi.ordinal - 1].is_const ||
            plan.columns[oi.ordinal - 1].json_path.empty())
          throw OdbcError("42000", "ORDER BY ordinal " + std::to_string(oi.ordinal) + " is not a field column");
        path = join(plan.columns[oi.ordinal - 1].json_path, ".");
      } else {
        bool matched = false;
        if (oi.path.size() == 1)
          for (auto& c : plan.columns)
            if (!c.is_const && !c.json_path.empty() && iequals(c.name, oi.path[0])) {
              path = join(c.json_path, ".");
              matched = true;
              break;
            }
        if (!matched) path = resolve(ctx, oi.path).soql;
      }
      keys.push_back(path + (oi.desc ? " DESC" : " ASC") + (oi.nulls.empty() ? "" : " NULLS " + oi.nulls));
    }
    out.push_back(" ORDER BY " + join(keys, ","));
  }
  plan.params = ctx.params;
  plan.params.resize(plan.param_count, {-1, "", ""});
  return plan;
}

std::string render_soql(const QueryPlan& plan, const std::vector<Lit>& params, SQLULEN max_rows) {
  std::string s;
  for (auto& part : plan.soql) {
    if (auto* str = std::get_if<std::string>(&part)) {
      s += *str;
    } else {
      const ParamSlot& slot = std::get<ParamSlot>(part);
      if (slot.index >= static_cast<int>(params.size()))
        throw OdbcError("07002", "Parameter " + std::to_string(slot.index + 1) + " is not bound");
      s += soql_literal(params[slot.index], slot.sf_type, slot.field);
    }
  }
  std::optional<long long> limit = plan.limit;
  if (max_rows > 0 && (!limit || static_cast<long long>(max_rows) < *limit))
    limit = static_cast<long long>(max_rows);
  if (limit) s += " LIMIT " + std::to_string(*limit);
  if (plan.offset) s += " OFFSET " + std::to_string(*plan.offset);
  return s;
}

}  // namespace sf
