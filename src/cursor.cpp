#include "cursor.h"

#include <cmath>
#include <cstdio>

namespace sf {

static const char* default_type_name(SQLSMALLINT t) {
  switch (t) {
    case SQL_WVARCHAR: return "NVARCHAR";
    case SQL_WLONGVARCHAR: return "NLONGVARCHAR";
    case SQL_VARCHAR: return "VARCHAR";
    case SQL_SMALLINT: return "SMALLINT";
    case SQL_INTEGER: return "INTEGER";
    case SQL_BIGINT: return "BIGINT";
    case SQL_BIT: return "BIT";
    case SQL_DECIMAL: return "DECIMAL";
    case SQL_DOUBLE: return "DOUBLE";
    case SQL_TYPE_DATE: return "DATE";
    case SQL_TYPE_TIME: return "TIME";
    case SQL_TYPE_TIMESTAMP: return "TIMESTAMP";
    default: return "NVARCHAR";
  }
}

ColumnInfo make_column(const std::string& name, SQLSMALLINT sql_type, SQLULEN size, SQLSMALLINT digits,
                       SQLSMALLINT nullable) {
  ColumnInfo c;
  c.name = name;
  c.base_column = name;
  c.sql_type = sql_type;
  if (size == 0) {
    switch (sql_type) {
      case SQL_SMALLINT: size = 5; break;
      case SQL_INTEGER: size = 10; break;
      case SQL_BIGINT: size = 19; break;
      case SQL_BIT: size = 1; break;
      case SQL_TYPE_DATE: size = 10; break;
      case SQL_TYPE_TIMESTAMP: size = 23; break;
      default: size = 255;
    }
  }
  c.column_size = size;
  c.decimal_digits = digits;
  c.nullable = nullable;
  c.type_name = default_type_name(sql_type);
  return c;
}

ColumnInfo column_from_field(const FieldMeta& f, const std::string& object_name) {
  SqlType t = map_field_type(f);
  ColumnInfo c;
  c.name = f.name;
  c.base_column = f.name;
  c.base_table = object_name;
  c.sql_type = t.sql_type;
  c.column_size = t.column_size;
  c.decimal_digits = t.decimal_digits;
  c.type_name = t.type_name;
  c.nullable = f.nillable ? SQL_NULLABLE : SQL_NO_NULLS;
  c.case_sensitive = f.case_sensitive;
  c.searchable = f.filterable;
  c.auto_unique = f.auto_number;
  c.sf_type = f.type;
  c.json_path = {f.name};
  return c;
}

// ------------------------------------------------------------ values

Value json_to_value(const json& v, const std::string& sf_type) {
  if (v.is_null()) return std::nullopt;
  if (v.is_boolean()) return std::string(v.get<bool>() ? "1" : "0");
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
  if (v.is_number_float()) {
    double d = v.get<double>();
    if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 1e15) {
      char b[32];
      std::snprintf(b, sizeof b, "%.0f", d);
      return std::string(b);
    }
    char b[40];
    std::snprintf(b, sizeof b, "%.15g", d);
    return std::string(b);
  }
  if (v.is_string()) {
    const std::string& s = v.get_ref<const std::string&>();
    if (sf_type == "datetime" || sf_type == "date" || sf_type == "time") {
      DateTimeParts p;
      if (parse_datetime(s, p)) {
        if (sf_type == "datetime") return format_timestamp(p);
        if (sf_type == "date") return format_date(p);
        return format_time(p);
      }
    }
    return s;
  }
  return v.dump();  // compound values / anyType
}

static const json* find_key_ci(const json& obj, const std::string& key) {
  if (!obj.is_object()) return nullptr;
  auto it = obj.find(key);
  if (it != obj.end()) return &*it;
  for (auto i = obj.begin(); i != obj.end(); ++i)
    if (iequals(i.key(), key)) return &i.value();
  return nullptr;
}

bool VectorCursor::next(Row& out) {
  if (pos_ >= rows_.size()) return false;
  out = rows_[pos_++];
  return true;
}

SoqlCursor::SoqlCursor(SalesforceClient& client, std::vector<ColumnInfo> cols, const std::string& soql,
                       std::atomic<bool>* cancel, SQLULEN max_rows)
    : Cursor(std::move(cols)), client_(client), cancel_(cancel), max_rows_(max_rows) {
  load(client_.query(soql));
}

void SoqlCursor::load(const json& page) {
  records_ = page.contains("records") ? page["records"] : json::array();
  pos_ = 0;
  if (page.contains("totalSize")) total_ = page["totalSize"].get<SQLLEN>();
  done_ = page.value("done", true);
  next_url_ = page.contains("nextRecordsUrl") && page["nextRecordsUrl"].is_string()
                  ? page["nextRecordsUrl"].get<std::string>()
                  : "";
  if (next_url_.empty()) done_ = true;
}

bool SoqlCursor::next(Row& out) {
  if (max_rows_ > 0 && delivered_ >= max_rows_) return false;
  while (pos_ >= records_.size()) {
    if (done_) return false;
    if (cancel_ && cancel_->load()) throw OdbcError("HY008", "Operation canceled");
    load(client_.query_more(next_url_));
  }
  const json& rec = records_[pos_++];
  out.assign(cols_.size(), std::nullopt);
  for (size_t i = 0; i < cols_.size(); ++i) {
    const ColumnInfo& c = cols_[i];
    if (c.is_const) { out[i] = c.const_value; continue; }
    const json* cur = &rec;
    for (auto& part : c.json_path) {
      cur = find_key_ci(*cur, part);
      if (!cur || cur->is_null()) break;
    }
    out[i] = cur ? json_to_value(*cur, c.sf_type) : std::nullopt;
  }
  ++delivered_;
  return true;
}

}  // namespace sf
