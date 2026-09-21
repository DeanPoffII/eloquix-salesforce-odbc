#pragma once
#include "salesforce.h"

#include <chrono>
#include <unordered_map>

namespace sf {

struct FieldMeta {
  std::string name, label, type, relationship_name, compound_field_name;
  std::vector<std::string> reference_to;
  int length = 0, precision = 0, scale = 0, digits = 0, position = 0;
  bool nillable = true, filterable = true, sortable = true, calculated = false, unique = false;
  bool case_sensitive = false, auto_number = false, updateable = false, createable = false;

  bool is_compound() const { return type == "address" || type == "location"; }
};

struct ChildRelationship {
  std::string child_object, field, relationship_name;
};

struct ObjectMeta {
  std::string name, label;
  bool queryable = true, custom = false;
  std::vector<FieldMeta> fields;
  std::vector<ChildRelationship> children;
  std::unordered_map<std::string, size_t> by_lower;

  const FieldMeta* field(const std::string& name) const;
  const FieldMeta* relationship(const std::string& rel_name) const;
};

struct GlobalObject {
  std::string name, label;
  bool queryable = true, custom = false;
};

// ODBC view of a Salesforce field
struct SqlType {
  SQLSMALLINT sql_type;  // ODBC 3 concise type
  SQLULEN column_size;
  SQLSMALLINT decimal_digits;
  const char* type_name;
};
SqlType map_field_type(const FieldMeta& f);
SQLSMALLINT odbc2_type(SQLSMALLINT t);  // date/time type codes for ODBC 2.x apps
SQLLEN octet_length(SQLSMALLINT sql_type, SQLULEN column_size);
SQLLEN display_size(SQLSMALLINT sql_type, SQLULEN column_size);

// Metadata cache shared by all connections for the same org+user (catalog calls are
// hammered by BI tools; this keeps them off the API).
class MetadataCache {
 public:
  static std::shared_ptr<MetadataCache> shared(const std::string& key, long ttl_seconds);

  std::vector<GlobalObject> objects(SalesforceClient& c);
  // Case-insensitive; returns canonical API name or empty if not found.
  std::string resolve_object_name(SalesforceClient& c, const std::string& name);
  std::shared_ptr<const ObjectMeta> object(SalesforceClient& c, const std::string& name);
  std::vector<std::shared_ptr<const ObjectMeta>> objects_meta(SalesforceClient& c,
                                                              const std::vector<std::string>& names);
  void invalidate();

 private:
  using Clock = std::chrono::steady_clock;
  bool fresh(Clock::time_point t) const { return Clock::now() - t < std::chrono::seconds(ttl_); }
  static std::shared_ptr<ObjectMeta> parse_describe(const json& j);

  std::mutex mu_;
  long ttl_ = 3600;
  std::vector<GlobalObject> globals_;
  Clock::time_point globals_at_{};
  bool globals_loaded_ = false;
  struct Entry {
    std::shared_ptr<const ObjectMeta> meta;
    Clock::time_point at;
  };
  std::unordered_map<std::string, Entry> objects_;  // key: lower-case name
};

}  // namespace sf
