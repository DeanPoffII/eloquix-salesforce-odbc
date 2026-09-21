#pragma once
#include "util.h"

#include <map>

namespace sf {

// Connection settings from the connection string and/or DSN.
// Keys are case-insensitive. UID/PWD are aliases for Username/Password.
class ConnConfig {
 public:
  static ConnConfig parse(const std::string& conn_str);
  // Fill keys not already present from the DSN section of odbc.ini / registry.
  void merge_dsn(const std::string& dsn);

  std::string get(const std::string& key, const std::string& def = "") const;
  long long get_int(const std::string& key, long long def) const;
  bool get_bool(const std::string& key, bool def) const;
  bool has(const std::string& key) const;
  void set(const std::string& key, const std::string& value);

  std::string to_connection_string() const;  // for SQLDriverConnect output
  std::string redacted() const;              // for logs

  static bool is_secret(const std::string& key);

 private:
  static std::string canonical(const std::string& key);
  std::map<std::string, std::string, CiLess> kv_;
};

}  // namespace sf
