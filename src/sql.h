#pragma once
#include "cursor.h"

#include <memory>
#include <variant>

namespace sf {

// ------------------------------------------------------------ literals

struct Lit {
  enum Kind { Null, String, Number, Bool, Date, Time, Timestamp };
  Kind kind = Null;
  std::string text;
};

// ------------------------------------------------------------ AST

struct Operand {
  enum Kind { Column, Literal, Param };
  Kind kind = Literal;
  std::vector<std::string> path;  // Column
  Lit lit;                        // Literal
  int param_index = -1;           // Param (0-based)
};

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
  enum Kind { And, Or, Not, Compare, In, Like, IsNull };
  Kind kind = Compare;
  std::string op;  // Compare: = != < <= > >=
  bool negated = false;
  ExprPtr left, right;  // And/Or/Not
  Operand a, b;         // Compare/Like/IsNull use a (and b)
  std::vector<Operand> list;  // In
};

struct SelectItem {
  enum Kind { Star, Column, CountStar, Literal };
  Kind kind = Column;
  std::vector<std::string> path;
  Lit lit;
  std::string alias;
};

struct OrderItem {
  std::vector<std::string> path;
  int ordinal = 0;  // ORDER BY 2
  bool desc = false;
  std::string nulls;  // "", "FIRST", "LAST"
};

struct SelectStmt {
  std::vector<SelectItem> items;
  std::vector<std::string> table;  // possibly qualified
  std::string table_alias;
  ExprPtr where;
  std::vector<OrderItem> order_by;
  std::optional<long long> limit;
  std::optional<long long> offset;
  int param_count = 0;
};

SelectStmt parse_sql(const std::string& sql);

// ------------------------------------------------------------ plan

struct ParamSlot {
  int index;
  std::string sf_type;  // type of the field the parameter is compared against
  std::string field;
};

struct QueryPlan {
  enum Kind { Soql, Count, Constant };
  Kind kind = Soql;
  std::vector<ColumnInfo> columns;
  std::vector<std::variant<std::string, ParamSlot>> soql;  // template up to (excluding) LIMIT
  std::optional<long long> limit, offset;
  bool always_empty = false;
  int param_count = 0;
  std::vector<ParamSlot> params;  // by index, for SQLDescribeParam
  std::string object_name;
};

QueryPlan build_plan(const std::string& sql, SalesforceClient& client, MetadataCache& meta);
std::string render_soql(const QueryPlan& plan, const std::vector<Lit>& params, SQLULEN max_rows);

// Render a literal for comparison with a field of the given Salesforce type.
std::string soql_literal(const Lit& lit, const std::string& sf_type, const std::string& field_name);

}  // namespace sf
