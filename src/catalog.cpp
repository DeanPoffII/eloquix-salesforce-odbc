#include "catalog.h"

namespace sf {
namespace {

Value S(const std::string& s) { return s; }
Value N(long long n) { return std::to_string(n); }
const Value NUL = std::nullopt;

ColumnInfo vc(const char* name, SQLSMALLINT nullable = SQL_NULLABLE) {
  return make_column(name, SQL_WVARCHAR, 128, 0, nullable);
}
ColumnInfo si(const char* name, SQLSMALLINT nullable = SQL_NULLABLE) {
  return make_column(name, SQL_SMALLINT, 5, 0, nullable);
}
ColumnInfo in(const char* name, SQLSMALLINT nullable = SQL_NULLABLE) {
  return make_column(name, SQL_INTEGER, 10, 0, nullable);
}

bool name_matches(bool metadata_id, const OptStr& pattern, const std::string& name) {
  if (!pattern) return true;
  if (metadata_id) return iequals(trim(*pattern), name);
  return like_match(*pattern, name);
}

// We expose no catalogs or schemas; a non-empty, non-wildcard value matches nothing.
bool empty_or_wild(const OptStr& v) { return !v || v->empty() || *v == "%"; }

SQLSMALLINT report_type(SQLSMALLINT t, SQLINTEGER ver) { return ver == SQL_OV_ODBC2 ? odbc2_type(t) : t; }

std::vector<GlobalObject> matching_objects(Conn& c, bool metadata_id, const OptStr& pattern) {
  std::vector<GlobalObject> out;
  bool include_all = c.cfg.get_bool("IncludeNonQueryable", false);
  for (auto& o : c.meta->objects(c.sf()))
    if ((o.queryable || include_all) && name_matches(metadata_id, pattern, o.name)) out.push_back(o);
  return out;
}

std::shared_ptr<const ObjectMeta> require_object(Conn& c, const OptStr& table) {
  if (!table || table->empty()) throw OdbcError("HY009", "Table name is required");
  std::string name = c.meta->resolve_object_name(c.sf(), *table);
  if (name.empty()) return nullptr;
  return c.meta->object(c.sf(), name);
}

}  // namespace

std::unique_ptr<Cursor> catalog_tables(Conn& c, bool mid, const OptStr& cat, const OptStr& schema,
                                       const OptStr& table, const OptStr& types) {
  std::vector<ColumnInfo> cols = {vc("TABLE_CAT"), vc("TABLE_SCHEM"), vc("TABLE_NAME"), vc("TABLE_TYPE"),
                                  make_column("REMARKS", SQL_WVARCHAR, 254)};
  std::vector<Row> rows;
  auto blank = [](const OptStr& v) { return v && v->empty(); };

  if (cat && *cat == SQL_ALL_CATALOGS && blank(schema) && blank(table))
    return std::make_unique<VectorCursor>(cols, rows);  // no catalogs
  if (schema && *schema == SQL_ALL_SCHEMAS && blank(cat) && blank(table))
    return std::make_unique<VectorCursor>(cols, rows);  // no schemas
  if (types && *types == SQL_ALL_TABLE_TYPES && blank(cat) && blank(schema) && blank(table)) {
    rows.push_back({NUL, NUL, NUL, S("TABLE"), NUL});
    return std::make_unique<VectorCursor>(cols, rows);
  }
  if (!empty_or_wild(cat) || !empty_or_wild(schema)) return std::make_unique<VectorCursor>(cols, rows);

  if (types && !types->empty()) {
    bool wants_table = false;
    std::stringstream ss(*types);
    std::string t;
    while (std::getline(ss, t, ',')) {
      t = trim(t);
      if (t.size() >= 2 && t.front() == '\'' && t.back() == '\'') t = t.substr(1, t.size() - 2);
      if (iequals(t, "TABLE") || t == "%") wants_table = true;
    }
    if (!wants_table) return std::make_unique<VectorCursor>(cols, rows);
  }
  for (auto& o : matching_objects(c, mid, table)) rows.push_back({NUL, NUL, S(o.name), S("TABLE"), S(o.label)});
  return std::make_unique<VectorCursor>(cols, rows);
}

std::unique_ptr<Cursor> catalog_columns(Conn& c, bool mid, const OptStr& cat, const OptStr& schema,
                                        const OptStr& table, const OptStr& column) {
  std::vector<ColumnInfo> cols = {
      vc("TABLE_CAT"), vc("TABLE_SCHEM"), vc("TABLE_NAME", SQL_NO_NULLS), vc("COLUMN_NAME", SQL_NO_NULLS),
      si("DATA_TYPE", SQL_NO_NULLS), vc("TYPE_NAME", SQL_NO_NULLS), in("COLUMN_SIZE"), in("BUFFER_LENGTH"),
      si("DECIMAL_DIGITS"), si("NUM_PREC_RADIX"), si("NULLABLE", SQL_NO_NULLS),
      make_column("REMARKS", SQL_WVARCHAR, 254), make_column("COLUMN_DEF", SQL_WVARCHAR, 254),
      si("SQL_DATA_TYPE", SQL_NO_NULLS), si("SQL_DATETIME_SUB"), in("CHAR_OCTET_LENGTH"),
      in("ORDINAL_POSITION", SQL_NO_NULLS), make_column("IS_NULLABLE", SQL_WVARCHAR, 3)};
  std::vector<Row> rows;
  if (!empty_or_wild(cat) || !empty_or_wild(schema)) return std::make_unique<VectorCursor>(cols, rows);

  std::vector<std::string> names;
  for (auto& o : matching_objects(c, mid, table)) names.push_back(o.name);
  if (names.size() > 50) LOG_INFO("SQLColumns describing " << names.size() << " objects (batched)");
  SQLINTEGER ver = c.env->odbc_version;

  for (auto& meta : c.meta->objects_meta(c.sf(), names)) {
    for (auto& f : meta->fields) {
      if (!name_matches(mid, column, f.name)) continue;
      SqlType t = map_field_type(f);
      bool is_char = t.sql_type == SQL_WVARCHAR || t.sql_type == SQL_WLONGVARCHAR;
      bool is_num = t.sql_type == SQL_INTEGER || t.sql_type == SQL_BIGINT || t.sql_type == SQL_DECIMAL ||
                    t.sql_type == SQL_BIT || t.sql_type == SQL_DOUBLE;
      bool is_dt = t.sql_type == SQL_TYPE_DATE || t.sql_type == SQL_TYPE_TIME || t.sql_type == SQL_TYPE_TIMESTAMP;
      Value dt_sub = NUL;
      if (t.sql_type == SQL_TYPE_DATE) dt_sub = N(SQL_CODE_DATE);
      if (t.sql_type == SQL_TYPE_TIME) dt_sub = N(SQL_CODE_TIME);
      if (t.sql_type == SQL_TYPE_TIMESTAMP) dt_sub = N(SQL_CODE_TIMESTAMP);
      rows.push_back({NUL,
                      NUL,
                      S(meta->name),
                      S(f.name),
                      N(report_type(t.sql_type, ver)),
                      S(t.type_name),
                      N(static_cast<long long>(t.column_size)),
                      N(octet_length(t.sql_type, t.column_size)),
                      (is_num || t.sql_type == SQL_TYPE_TIMESTAMP) ? N(t.decimal_digits) : NUL,
                      is_num ? N(10) : NUL,
                      N(f.nillable ? SQL_NULLABLE : SQL_NO_NULLS),
                      S(f.label),
                      NUL,
                      N(is_dt ? SQL_DATETIME : t.sql_type),
                      dt_sub,
                      is_char ? N(octet_length(t.sql_type, t.column_size)) : NUL,
                      N(f.position),
                      S(f.nillable ? "YES" : "NO")});
    }
  }
  return std::make_unique<VectorCursor>(cols, rows);
}

std::unique_ptr<Cursor> catalog_primary_keys(Conn& c, const OptStr& cat, const OptStr& schema, const OptStr& table) {
  std::vector<ColumnInfo> cols = {vc("TABLE_CAT"), vc("TABLE_SCHEM"), vc("TABLE_NAME", SQL_NO_NULLS),
                                  vc("COLUMN_NAME", SQL_NO_NULLS), si("KEY_SEQ", SQL_NO_NULLS), vc("PK_NAME")};
  std::vector<Row> rows;
  if (empty_or_wild(cat) && empty_or_wild(schema))
    if (auto m = require_object(c, table))
      if (m->field("Id")) rows.push_back({NUL, NUL, S(m->name), S("Id"), N(1), S("PK_" + m->name)});
  return std::make_unique<VectorCursor>(cols, rows);
}

std::unique_ptr<Cursor> catalog_foreign_keys(Conn& c, const OptStr& pk_table, const OptStr& fk_table) {
  std::vector<ColumnInfo> cols = {
      vc("PKTABLE_CAT"), vc("PKTABLE_SCHEM"), vc("PKTABLE_NAME", SQL_NO_NULLS), vc("PKCOLUMN_NAME", SQL_NO_NULLS),
      vc("FKTABLE_CAT"), vc("FKTABLE_SCHEM"), vc("FKTABLE_NAME", SQL_NO_NULLS), vc("FKCOLUMN_NAME", SQL_NO_NULLS),
      si("KEY_SEQ", SQL_NO_NULLS), si("UPDATE_RULE"), si("DELETE_RULE"), vc("FK_NAME"), vc("PK_NAME"),
      si("DEFERRABILITY")};
  std::vector<Row> rows;
  auto row = [&](const std::string& pk, const std::string& fk, const std::string& fkcol) {
    rows.push_back({NUL, NUL, S(pk), S("Id"), NUL, NUL, S(fk), S(fkcol), N(1), N(SQL_NO_ACTION), N(SQL_NO_ACTION),
                    S("FK_" + fk + "_" + fkcol), S("PK_" + pk), N(SQL_NOT_DEFERRABLE)});
  };
  bool has_fk = fk_table && !fk_table->empty();
  bool has_pk = pk_table && !pk_table->empty();
  if (!has_fk && !has_pk) throw OdbcError("HY009", "PKTableName or FKTableName is required");

  if (has_fk) {
    auto fk = require_object(c, fk_table);
    std::string pk_name = has_pk ? c.meta->resolve_object_name(c.sf(), *pk_table) : "";
    if (fk && (!has_pk || !pk_name.empty()))
      for (auto& f : fk->fields)
        if (f.type == "reference")
          for (auto& target : f.reference_to)
            if (!has_pk || iequals(target, pk_name)) row(target, fk->name, f.name);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return *a[2] < *b[2]; });
  } else {
    auto pk = require_object(c, pk_table);
    if (pk) {
      std::vector<GlobalObject> globals = c.meta->objects(c.sf());
      auto queryable = [&](const std::string& n) {
        for (auto& g : globals) if (iequals(g.name, n)) return g.queryable;
        return false;
      };
      for (auto& ch : pk->children)
        if (!ch.field.empty() && queryable(ch.child_object)) row(pk->name, ch.child_object, ch.field);
      std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return std::make_pair(*a[6], *a[7]) < std::make_pair(*b[6], *b[7]);
      });
    }
  }
  return std::make_unique<VectorCursor>(cols, rows);
}

std::unique_ptr<Cursor> catalog_statistics(Conn& c, const OptStr& table) {
  std::vector<ColumnInfo> cols = {vc("TABLE_CAT"), vc("TABLE_SCHEM"), vc("TABLE_NAME", SQL_NO_NULLS),
                                  si("NON_UNIQUE"), vc("INDEX_QUALIFIER"), vc("INDEX_NAME"),
                                  si("TYPE", SQL_NO_NULLS), si("ORDINAL_POSITION"), vc("COLUMN_NAME"),
                                  make_column("ASC_OR_DESC", SQL_WVARCHAR, 1), in("CARDINALITY"), in("PAGES"),
                                  vc("FILTER_CONDITION")};
  std::vector<Row> rows;
  if (auto m = require_object(c, table))
    rows.push_back({NUL, NUL, S(m->name), N(SQL_FALSE), NUL, S("PK_" + m->name), N(SQL_INDEX_OTHER), N(1), S("Id"),
                    S("A"), NUL, NUL, NUL});
  return std::make_unique<VectorCursor>(cols, rows);
}

std::unique_ptr<Cursor> catalog_special_columns(Conn& c, SQLUSMALLINT id_type, const OptStr& table) {
  std::vector<ColumnInfo> cols = {si("SCOPE"), vc("COLUMN_NAME", SQL_NO_NULLS), si("DATA_TYPE", SQL_NO_NULLS),
                                  vc("TYPE_NAME", SQL_NO_NULLS), in("COLUMN_SIZE"), in("BUFFER_LENGTH"),
                                  si("DECIMAL_DIGITS"), si("PSEUDO_COLUMN")};
  std::vector<Row> rows;
  auto m = require_object(c, table);
  if (m && id_type == SQL_BEST_ROWID && m->field("Id")) {
    rows.push_back({N(SQL_SCOPE_SESSION), S("Id"), N(SQL_WVARCHAR), S("NVARCHAR"), N(18),
                    N(octet_length(SQL_WVARCHAR, 18)), NUL, N(SQL_PC_NOT_PSEUDO)});
  } else if (m && id_type == SQL_ROWVER && m->field("SystemModstamp")) {
    SQLSMALLINT t = report_type(SQL_TYPE_TIMESTAMP, c.env->odbc_version);
    rows.push_back({NUL, S("SystemModstamp"), N(t), S("TIMESTAMP"), N(23), N(sizeof(SQL_TIMESTAMP_STRUCT)), N(3),
                    N(SQL_PC_NOT_PSEUDO)});
  }
  return std::make_unique<VectorCursor>(cols, rows);
}

std::unique_ptr<Cursor> catalog_procedures() {
  std::vector<ColumnInfo> cols = {vc("PROCEDURE_CAT"), vc("PROCEDURE_SCHEM"), vc("PROCEDURE_NAME"),
                                  in("NUM_INPUT_PARAMS"), in("NUM_OUTPUT_PARAMS"), in("NUM_RESULT_SETS"),
                                  vc("REMARKS"), si("PROCEDURE_TYPE")};
  return std::make_unique<VectorCursor>(cols, std::vector<Row>{});
}

std::unique_ptr<Cursor> catalog_procedure_columns() {
  std::vector<ColumnInfo> cols = {vc("PROCEDURE_CAT"), vc("PROCEDURE_SCHEM"), vc("PROCEDURE_NAME"),
                                  vc("COLUMN_NAME"), si("COLUMN_TYPE"), si("DATA_TYPE"), vc("TYPE_NAME"),
                                  in("COLUMN_SIZE"), in("BUFFER_LENGTH"), si("DECIMAL_DIGITS"),
                                  si("NUM_PREC_RADIX"), si("NULLABLE"), vc("REMARKS"), vc("COLUMN_DEF"),
                                  si("SQL_DATA_TYPE"), si("SQL_DATETIME_SUB"), in("CHAR_OCTET_LENGTH"),
                                  in("ORDINAL_POSITION"), vc("IS_NULLABLE")};
  return std::make_unique<VectorCursor>(cols, std::vector<Row>{});
}

std::unique_ptr<Cursor> type_info(SQLSMALLINT data_type, SQLINTEGER ver) {
  std::vector<ColumnInfo> cols = {
      vc("TYPE_NAME", SQL_NO_NULLS), si("DATA_TYPE", SQL_NO_NULLS), in("COLUMN_SIZE"), vc("LITERAL_PREFIX"),
      vc("LITERAL_SUFFIX"), vc("CREATE_PARAMS"), si("NULLABLE", SQL_NO_NULLS), si("CASE_SENSITIVE", SQL_NO_NULLS),
      si("SEARCHABLE", SQL_NO_NULLS), si("UNSIGNED_ATTRIBUTE"), si("FIXED_PREC_SCALE", SQL_NO_NULLS),
      si("AUTO_UNIQUE_VALUE"), vc("LOCAL_TYPE_NAME"), si("MINIMUM_SCALE"), si("MAXIMUM_SCALE"),
      si("SQL_DATA_TYPE", SQL_NO_NULLS), si("SQL_DATETIME_SUB"), in("NUM_PREC_RADIX"), si("INTERVAL_PRECISION")};
  struct T {
    const char* name;
    SQLSMALLINT type;
    long long size;
    const char* prefix;
    const char* suffix;
    const char* params;
    int searchable;
    bool num;
    int min_scale, max_scale;
    int dt_sub;
  };
  const T types[] = {
      {"NLONGVARCHAR", SQL_WLONGVARCHAR, 131072, "'", "'", nullptr, SQL_PRED_CHAR, false, -1, -1, 0},
      {"NVARCHAR", SQL_WVARCHAR, 131072, "'", "'", "max length", SQL_SEARCHABLE, false, -1, -1, 0},
      {"BIT", SQL_BIT, 1, nullptr, nullptr, nullptr, SQL_PRED_BASIC, false, -1, -1, 0},
      {"BIGINT", SQL_BIGINT, 19, nullptr, nullptr, nullptr, SQL_PRED_BASIC, true, 0, 0, 0},
      {"DECIMAL", SQL_DECIMAL, 18, nullptr, nullptr, "precision,scale", SQL_PRED_BASIC, true, 0, 18, 0},
      {"INTEGER", SQL_INTEGER, 10, nullptr, nullptr, nullptr, SQL_PRED_BASIC, true, 0, 0, 0},
      {"DOUBLE", SQL_DOUBLE, 15, nullptr, nullptr, nullptr, SQL_PRED_BASIC, true, -1, -1, 0},
      {"DATE", SQL_TYPE_DATE, 10, "'", "'", nullptr, SQL_PRED_BASIC, false, -1, -1, SQL_CODE_DATE},
      {"TIME", SQL_TYPE_TIME, 8, "'", "'", nullptr, SQL_PRED_BASIC, false, -1, -1, SQL_CODE_TIME},
      {"TIMESTAMP", SQL_TYPE_TIMESTAMP, 23, "'", "'", nullptr, SQL_PRED_BASIC, false, 3, 3, SQL_CODE_TIMESTAMP},
  };
  std::vector<Row> rows;
  for (auto& t : types) {
    SQLSMALLINT reported = report_type(t.type, ver);
    if (data_type != SQL_ALL_TYPES && data_type != reported && data_type != t.type) continue;
    bool dt = t.dt_sub != 0;
    rows.push_back({S(t.name), N(reported), N(t.size), t.prefix ? S(t.prefix) : NUL, t.suffix ? S(t.suffix) : NUL,
                    t.params ? S(t.params) : NUL, N(SQL_NULLABLE), N(SQL_FALSE), N(t.searchable),
                    t.num ? N(SQL_FALSE) : NUL, N(SQL_FALSE), t.num ? N(SQL_FALSE) : NUL, S(t.name),
                    t.min_scale >= 0 ? N(t.min_scale) : NUL, t.max_scale >= 0 ? N(t.max_scale) : NUL,
                    N(dt ? SQL_DATETIME : t.type), dt ? N(t.dt_sub) : NUL, t.num ? N(10) : NUL, NUL});
  }
  std::stable_sort(rows.begin(), rows.end(),
                   [](const Row& a, const Row& b) { return std::stoi(*a[1]) < std::stoi(*b[1]); });
  return std::make_unique<VectorCursor>(cols, rows);
}

}  // namespace sf
