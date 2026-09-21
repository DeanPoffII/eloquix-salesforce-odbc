#pragma once
#include "metadata.h"

#include <atomic>

namespace sf {

using Value = std::optional<std::string>;  // UTF-8 text form; nullopt == SQL NULL
using Row = std::vector<Value>;

struct ColumnInfo {
  std::string name;            // label reported to the application
  std::string base_column;     // Salesforce field API name (or path)
  std::string base_table;
  SQLSMALLINT sql_type = SQL_WVARCHAR;
  SQLULEN column_size = 255;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = SQL_NULLABLE_UNKNOWN;
  std::string type_name = "NVARCHAR";
  bool case_sensitive = false;
  bool searchable = true;
  bool unsigned_type = false;
  bool auto_unique = false;

  // How to extract the value from a SOQL record
  std::string sf_type;                 // Salesforce field type ("datetime", ...)
  std::vector<std::string> json_path;  // e.g. {"Account","Name"}
  bool is_const = false;
  Value const_value;
};

ColumnInfo make_column(const std::string& name, SQLSMALLINT sql_type, SQLULEN size = 0,
                       SQLSMALLINT digits = 0, SQLSMALLINT nullable = SQL_NULLABLE);
ColumnInfo column_from_field(const FieldMeta& f, const std::string& object_name);

class Cursor {
 public:
  explicit Cursor(std::vector<ColumnInfo> cols) : cols_(std::move(cols)) {}
  virtual ~Cursor() = default;
  const std::vector<ColumnInfo>& columns() const { return cols_; }
  virtual bool next(Row& out) = 0;
  virtual SQLLEN row_count() const { return -1; }

 protected:
  std::vector<ColumnInfo> cols_;
};

class VectorCursor : public Cursor {
 public:
  VectorCursor(std::vector<ColumnInfo> cols, std::vector<Row> rows)
      : Cursor(std::move(cols)), rows_(std::move(rows)) {}
  bool next(Row& out) override;
  SQLLEN row_count() const override { return static_cast<SQLLEN>(rows_.size()); }

 private:
  std::vector<Row> rows_;
  size_t pos_ = 0;
};

// Streams SOQL results page by page (query + queryMore).
class SoqlCursor : public Cursor {
 public:
  SoqlCursor(SalesforceClient& client, std::vector<ColumnInfo> cols, const std::string& soql,
             std::atomic<bool>* cancel, SQLULEN max_rows);
  bool next(Row& out) override;
  SQLLEN row_count() const override { return total_; }

 private:
  void load(const json& page);

  SalesforceClient& client_;
  std::atomic<bool>* cancel_;
  json records_;
  size_t pos_ = 0;
  std::string next_url_;
  bool done_ = false;
  SQLLEN total_ = -1;
  SQLULEN max_rows_ = 0;
  SQLULEN delivered_ = 0;
};

// Convert a JSON value from a SOQL record into our text representation.
Value json_to_value(const json& v, const std::string& sf_type);

}  // namespace sf
