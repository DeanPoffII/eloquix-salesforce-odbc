#pragma once
#include "convert.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace sf {

constexpr uint32_t kEnvMagic = 0x53464556;   // SFEV
constexpr uint32_t kConnMagic = 0x53464443;  // SFDC
constexpr uint32_t kStmtMagic = 0x53465354;  // SFST
constexpr uint32_t kDescMagic = 0x53464453;  // SFDS

struct Conn;
struct Stmt;

struct Env {
  uint32_t magic = kEnvMagic;
  Diag diag;
  std::recursive_mutex mu;
  SQLINTEGER odbc_version = SQL_OV_ODBC3;
  SQLUINTEGER pooling = SQL_CP_OFF;
  SQLUINTEGER cp_match = SQL_CP_STRICT_MATCH;
  std::vector<Conn*> conns;

  static Env* from(SQLHANDLE h) {
    auto* e = static_cast<Env*>(h);
    return e && e->magic == kEnvMagic ? e : nullptr;
  }
};

struct Conn {
  uint32_t magic = kConnMagic;
  Env* env = nullptr;
  Diag diag;
  std::recursive_mutex mu;
  std::vector<Stmt*> stmts;

  bool connected = false;
  ConnConfig cfg;
  std::string dsn;
  std::unique_ptr<SalesforceClient> client;
  std::shared_ptr<MetadataCache> meta;

  SQLUINTEGER login_timeout = 0;
  SQLUINTEGER connection_timeout = 0;
  SQLUINTEGER autocommit = SQL_AUTOCOMMIT_ON;
  SQLUINTEGER access_mode = SQL_MODE_READ_ONLY;
  SQLUINTEGER metadata_id = SQL_FALSE;
  SQLUINTEGER txn_isolation = 0;
  std::string current_catalog;

  static Conn* from(SQLHANDLE h) {
    auto* c = static_cast<Conn*>(h);
    return c && c->magic == kConnMagic ? c : nullptr;
  }
  SalesforceClient& sf() {
    if (!connected || !client) throw OdbcError("08003", "Connection not open");
    return *client;
  }
};

// Stub descriptor: returned for SQL_ATTR_*_DESC so driver managers get a valid handle.
struct Desc {
  uint32_t magic = kDescMagic;
  Diag diag;
  Stmt* stmt = nullptr;
  static Desc* from(SQLHANDLE h) {
    auto* d = static_cast<Desc*>(h);
    return d && d->magic == kDescMagic ? d : nullptr;
  }
};

struct ColBinding {
  bool bound = false;
  SQLSMALLINT c_type = SQL_C_DEFAULT;
  SQLPOINTER target = nullptr;
  SQLLEN buffer_len = 0;
  SQLLEN* ind = nullptr;
};

struct Stmt {
  uint32_t magic = kStmtMagic;
  Conn* conn = nullptr;
  Diag diag;
  std::atomic<bool> cancel{false};

  // Prepared / executed state
  std::string sql;
  std::optional<QueryPlan> plan;
  std::unique_ptr<Cursor> cursor;
  std::vector<Row> rowset;       // rows from the last fetch
  bool positioned = false;       // a row is current for SQLGetData
  std::vector<GetDataState> getdata;
  SQLLEN row_count = -1;
  SQLULEN row_number = 0;
  std::string cursor_name;

  // Bindings
  std::vector<ColBinding> cols;       // index 1..n (0 unused: bookmarks)
  std::vector<ParamBinding> params;   // index 0..n-1

  // Attributes
  SQLULEN row_array_size = 1;
  SQLULEN row_bind_type = SQL_BIND_BY_COLUMN;
  SQLULEN* row_bind_offset = nullptr;
  SQLUSMALLINT* row_status = nullptr;
  SQLULEN* rows_fetched = nullptr;
  SQLULEN max_rows = 0;
  SQLULEN query_timeout = 0;
  SQLULEN max_length = 0;
  SQLULEN paramset_size = 1;
  SQLULEN param_bind_type = SQL_PARAM_BIND_BY_COLUMN;
  SQLULEN* param_bind_offset = nullptr;
  SQLULEN* params_processed = nullptr;
  SQLUSMALLINT* param_status = nullptr;
  SQLULEN metadata_id = SQL_FALSE;
  SQLULEN noscan = SQL_NOSCAN_OFF;
  SQLULEN retrieve_data = SQL_RD_ON;

  Desc ard, apd, ird, ipd;

  static Stmt* from(SQLHANDLE h) {
    auto* s = static_cast<Stmt*>(h);
    return s && s->magic == kStmtMagic ? s : nullptr;
  }

  const std::vector<ColumnInfo>* columns() const {
    if (cursor) return &cursor->columns();
    if (plan) return &plan->columns;
    return nullptr;
  }
  void close_cursor() {
    cursor.reset();
    rowset.clear();
    getdata.clear();
    positioned = false;
    row_number = 0;
    cancel = false;
  }
  void set_cursor(std::unique_ptr<Cursor> c) {
    close_cursor();
    cursor = std::move(c);
    row_count = cursor->row_count();
  }
};

}  // namespace sf
