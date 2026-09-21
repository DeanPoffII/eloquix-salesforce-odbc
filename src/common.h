// Salesforce ODBC Driver - Eloquix Labs
// Common platform includes. Every source file includes this first.
#pragma once

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>
#include <odbcinst.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define SFODBC_DRIVER_NAME    "Eloquix Salesforce ODBC Driver"
#define SFODBC_DRIVER_VERSION "00.01.0000"
#define SFODBC_VENDOR_PREFIX  "[Eloquix][Salesforce ODBC] "
#define SFODBC_DEFAULT_API    "64.0"

namespace sf {

// Thrown anywhere inside the driver; converted to a diagnostic record at the API boundary.
class OdbcError : public std::runtime_error {
 public:
  OdbcError(std::string state, const std::string& msg, int native = 0)
      : std::runtime_error(msg), state_(std::move(state)), native_(native) {}
  const std::string& state() const { return state_; }
  int native() const { return native_; }

 private:
  std::string state_;
  int native_;
};

struct DiagRec {
  std::string state;
  SQLINTEGER native = 0;
  std::string message;
};

struct Diag {
  std::vector<DiagRec> recs;
  SQLRETURN last_return = SQL_SUCCESS;
  SQLLEN row_count = 0;

  void clear() { recs.clear(); last_return = SQL_SUCCESS; }
  void add(const std::string& state, const std::string& msg, SQLINTEGER native = 0) {
    recs.push_back({state, native, SFODBC_VENDOR_PREFIX + msg});
  }
  bool has_records() const { return !recs.empty(); }
};

}  // namespace sf
