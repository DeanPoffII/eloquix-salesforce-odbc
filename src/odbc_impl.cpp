#include "api.h"

#include <cinttypes>
#include <cstdio>

namespace sf::api {
namespace {

// Run f, converting exceptions into diagnostic records on `d`.
template <typename F>
SQLRETURN guarded(Diag& d, F&& f) {
  d.clear();
  SQLRETURN rc = SQL_ERROR;
  try {
    rc = f();
  } catch (const OdbcError& e) {
    d.add(e.state(), e.what(), e.native());
    LOG_ERROR(e.state() << " " << e.what());
  } catch (const std::bad_alloc&) {
    d.add("HY001", "Memory allocation error");
  } catch (const nlohmann::json::exception& e) {
    d.add("HY000", std::string("Unexpected response from Salesforce: ") + e.what());
  } catch (const std::exception& e) {
    d.add("HY000", e.what());
  }
  if (rc == SQL_SUCCESS && d.has_records()) rc = SQL_SUCCESS_WITH_INFO;
  d.last_return = rc;
  return rc;
}

SQLRETURN info_or_success(Diag& d) { return d.has_records() ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS; }

template <typename L>
bool write_str(bool wide, bool len_in_bytes, const std::string& v, SQLPOINTER buf, SQLLEN cap, L* out) {
  if (!wide) return out_a(v, buf, cap, out);
  return len_in_bytes ? out_w_bytes(v, buf, cap, out) : out_w_chars(v, buf, cap, out);
}

#define WITH_STMT(h)                                   \
  Stmt* s = Stmt::from(h);                             \
  if (!s) return SQL_INVALID_HANDLE;                   \
  std::lock_guard<std::recursive_mutex> _lk(s->conn->mu)

#define WITH_CONN(h)                                   \
  Conn* c = Conn::from(h);                             \
  if (!c) return SQL_INVALID_HANDLE;                   \
  std::lock_guard<std::recursive_mutex> _lk(c->mu)

intptr_t ival(SQLPOINTER p) { return static_cast<intptr_t>(reinterpret_cast<intptr_t>(p)); }

void write_uint(SQLPOINTER dst, SQLUINTEGER v, SQLINTEGER* len) {
  if (dst) std::memcpy(dst, &v, sizeof v);
  if (len) *len = sizeof v;
}
void write_ulen(SQLPOINTER dst, SQLULEN v, SQLINTEGER* len) {
  if (dst) std::memcpy(dst, &v, sizeof v);
  if (len) *len = sizeof v;
}
void write_ptr(SQLPOINTER dst, SQLPOINTER v, SQLINTEGER* len) {
  if (dst) std::memcpy(dst, &v, sizeof v);
  if (len) *len = sizeof v;
}

const ColumnInfo& column_at(Stmt* s, SQLUSMALLINT col) {
  auto* cols = s->columns();
  if (!cols) throw OdbcError("HY010", "Function sequence error: no prepared or executed statement");
  if (col == 0) throw OdbcError("07009", "Bookmark columns are not supported");
  if (col > cols->size()) throw OdbcError("07009", "Invalid column number " + std::to_string(col));
  return (*cols)[col - 1];
}

SQLSMALLINT reported_type(Stmt* s, SQLSMALLINT t) {
  return s->conn->env->odbc_version == SQL_OV_ODBC2 ? odbc2_type(t) : t;
}

bool is_char_type(SQLSMALLINT t) {
  return t == SQL_WVARCHAR || t == SQL_WLONGVARCHAR || t == SQL_VARCHAR || t == SQL_WCHAR || t == SQL_CHAR;
}
bool is_numeric_type(SQLSMALLINT t) {
  return t == SQL_INTEGER || t == SQL_BIGINT || t == SQL_SMALLINT || t == SQL_DECIMAL || t == SQL_DOUBLE ||
         t == SQL_BIT;
}
bool is_datetime_type(SQLSMALLINT t) {
  return t == SQL_TYPE_DATE || t == SQL_TYPE_TIME || t == SQL_TYPE_TIMESTAMP;
}

void open_catalog(Stmt* s, std::unique_ptr<Cursor> cur) {
  s->plan.reset();
  s->set_cursor(std::move(cur));
  s->getdata.assign(s->cursor->columns().size() + 1, {});
}

void do_connect(Conn* c, ConnConfig cfg) {
  if (c->connected) throw OdbcError("08002", "Connection already in use");
  Logger::instance().configure(cfg.get("LogFile"), static_cast<int>(cfg.get_int("LogLevel", 0)));
  LOG_INFO("Connecting: " << cfg.redacted());
  auto client = std::make_unique<SalesforceClient>(cfg);
  client->authenticate();
  c->meta = MetadataCache::shared(client->cache_key(), static_cast<long>(cfg.get_int("MetadataCacheTTL", 3600)));
  c->client = std::move(client);
  c->cfg = std::move(cfg);
  c->connected = true;
}

}  // namespace

// ====================================================================== handles

SQLRETURN AllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE* out) {
  if (!out) return SQL_ERROR;
  switch (type) {
    case SQL_HANDLE_ENV: {
      *out = new (std::nothrow) Env();
      return *out ? SQL_SUCCESS : SQL_ERROR;
    }
    case SQL_HANDLE_DBC: {
      Env* e = Env::from(input);
      if (!e) return SQL_INVALID_HANDLE;
      std::lock_guard<std::recursive_mutex> g(e->mu);
      auto* c = new (std::nothrow) Conn();
      if (!c) { e->diag.add("HY001", "Memory allocation error"); return SQL_ERROR; }
      c->env = e;
      e->conns.push_back(c);
      *out = c;
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_STMT: {
      Conn* c = Conn::from(input);
      if (!c) return SQL_INVALID_HANDLE;
      std::lock_guard<std::recursive_mutex> g(c->mu);
      c->diag.clear();
      if (!c->connected) { c->diag.add("08003", "Connection not open"); *out = SQL_NULL_HSTMT; return SQL_ERROR; }
      auto* s = new (std::nothrow) Stmt();
      if (!s) { c->diag.add("HY001", "Memory allocation error"); return SQL_ERROR; }
      s->conn = c;
      s->ard.stmt = s->apd.stmt = s->ird.stmt = s->ipd.stmt = s;
      c->stmts.push_back(s);
      *out = s;
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_DESC: {
      Conn* c = Conn::from(input);
      if (!c) return SQL_INVALID_HANDLE;
      c->diag.clear();
      c->diag.add("HYC00", "Explicitly allocated descriptors are not supported");
      return SQL_ERROR;
    }
  }
  return SQL_ERROR;
}

SQLRETURN FreeHandle(SQLSMALLINT type, SQLHANDLE h) {
  switch (type) {
    case SQL_HANDLE_ENV: {
      Env* e = Env::from(h);
      if (!e) return SQL_INVALID_HANDLE;
      if (!e->conns.empty()) { e->diag.clear(); e->diag.add("HY010", "Connections still allocated"); return SQL_ERROR; }
      e->magic = 0;
      delete e;
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_DBC: {
      Conn* c = Conn::from(h);
      if (!c) return SQL_INVALID_HANDLE;
      Env* e = c->env;
      {
        std::lock_guard<std::recursive_mutex> g(c->mu);
        if (c->connected) { c->diag.clear(); c->diag.add("HY010", "Connection is still open"); return SQL_ERROR; }
      }
      {
        std::lock_guard<std::recursive_mutex> g(e->mu);
        e->conns.erase(std::remove(e->conns.begin(), e->conns.end(), c), e->conns.end());
      }
      c->magic = 0;
      delete c;
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_STMT: {
      Stmt* s = Stmt::from(h);
      if (!s) return SQL_INVALID_HANDLE;
      Conn* c = s->conn;
      std::lock_guard<std::recursive_mutex> g(c->mu);
      c->stmts.erase(std::remove(c->stmts.begin(), c->stmts.end(), s), c->stmts.end());
      s->magic = 0;
      delete s;
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_DESC:
      return Desc::from(h) ? SQL_SUCCESS : SQL_INVALID_HANDLE;  // implicit descriptors are owned by the stmt
  }
  return SQL_ERROR;
}

// ====================================================================== environment

SQLRETURN SetEnvAttr(SQLHENV h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER) {
  Env* e = Env::from(h);
  if (!e) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> g(e->mu);
  return guarded(e->diag, [&]() -> SQLRETURN {
    switch (attr) {
      case SQL_ATTR_ODBC_VERSION: {
        intptr_t v = ival(value);
        if (v != SQL_OV_ODBC2 && v != SQL_OV_ODBC3 && v != SQL_OV_ODBC3_80)
          throw OdbcError("HY024", "Invalid ODBC version");
        e->odbc_version = static_cast<SQLINTEGER>(v);
        return SQL_SUCCESS;
      }
      case SQL_ATTR_CONNECTION_POOLING: e->pooling = static_cast<SQLUINTEGER>(ival(value)); return SQL_SUCCESS;
      case SQL_ATTR_CP_MATCH: e->cp_match = static_cast<SQLUINTEGER>(ival(value)); return SQL_SUCCESS;
      case SQL_ATTR_OUTPUT_NTS:
        if (ival(value) != SQL_TRUE) throw OdbcError("HYC00", "Only null-terminated output strings are supported");
        return SQL_SUCCESS;
    }
    throw OdbcError("HY092", "Invalid environment attribute " + std::to_string(attr));
  });
}

SQLRETURN GetEnvAttr(SQLHENV h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER, SQLINTEGER* len) {
  Env* e = Env::from(h);
  if (!e) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> g(e->mu);
  return guarded(e->diag, [&]() -> SQLRETURN {
    switch (attr) {
      case SQL_ATTR_ODBC_VERSION: write_uint(value, static_cast<SQLUINTEGER>(e->odbc_version), len); return SQL_SUCCESS;
      case SQL_ATTR_CONNECTION_POOLING: write_uint(value, e->pooling, len); return SQL_SUCCESS;
      case SQL_ATTR_CP_MATCH: write_uint(value, e->cp_match, len); return SQL_SUCCESS;
      case SQL_ATTR_OUTPUT_NTS: write_uint(value, SQL_TRUE, len); return SQL_SUCCESS;
    }
    throw OdbcError("HY092", "Invalid environment attribute " + std::to_string(attr));
  });
}

// ====================================================================== connection

SQLRETURN Connect(SQLHDBC h, const std::string& dsn, const OptStr& uid, const OptStr& pwd) {
  WITH_CONN(h);
  return guarded(c->diag, [&]() -> SQLRETURN {
    ConnConfig cfg;
    if (uid && !uid->empty()) cfg.set("Username", *uid);
    if (pwd && !pwd->empty()) cfg.set("Password", *pwd);
    cfg.merge_dsn(dsn);
    c->dsn = dsn;
    do_connect(c, std::move(cfg));
    return SQL_SUCCESS;
  });
}

SQLRETURN DriverConnect(SQLHDBC h, const std::string& in, bool wide, SQLPOINTER out, SQLSMALLINT cap,
                        SQLSMALLINT* out_len, SQLUSMALLINT) {
  WITH_CONN(h);
  return guarded(c->diag, [&]() -> SQLRETURN {
    ConnConfig cfg = ConnConfig::parse(in);
    std::string dsn = cfg.get("DSN");
    if (!dsn.empty()) cfg.merge_dsn(dsn);
    c->dsn = dsn;
    // No dialog in this release: all completion modes behave like SQL_DRIVER_NOPROMPT.
    do_connect(c, std::move(cfg));
    // Echo the caller's string (not DSN-merged values) so secrets from the DSN are not copied
    // into application-saved connection strings.
    if (write_str(wide, false, in, out, cap, out_len))
      c->diag.add("01004", "Output connection string truncated");
    return info_or_success(c->diag);
  });
}

SQLRETURN Disconnect(SQLHDBC h) {
  WITH_CONN(h);
  return guarded(c->diag, [&]() -> SQLRETURN {
    if (!c->connected) throw OdbcError("08003", "Connection not open");
    for (Stmt* s : c->stmts) { s->magic = 0; delete s; }
    c->stmts.clear();
    c->client.reset();
    c->meta.reset();
    c->connected = false;
    LOG_INFO("Disconnected");
    return SQL_SUCCESS;
  });
}

SQLRETURN SetConnectAttr(SQLHDBC h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER len, bool wide) {
  WITH_CONN(h);
  return guarded(c->diag, [&]() -> SQLRETURN {
    switch (attr) {
      case SQL_ATTR_AUTOCOMMIT: c->autocommit = static_cast<SQLUINTEGER>(ival(value)); return SQL_SUCCESS;
      case SQL_ATTR_ACCESS_MODE: c->access_mode = static_cast<SQLUINTEGER>(ival(value)); return SQL_SUCCESS;
      case SQL_ATTR_LOGIN_TIMEOUT: c->login_timeout = static_cast<SQLUINTEGER>(ival(value)); return SQL_SUCCESS;
      case SQL_ATTR_CONNECTION_TIMEOUT: c->connection_timeout = static_cast<SQLUINTEGER>(ival(value)); return SQL_SUCCESS;
      case SQL_ATTR_TXN_ISOLATION: c->txn_isolation = static_cast<SQLUINTEGER>(ival(value)); return SQL_SUCCESS;
      case SQL_ATTR_METADATA_ID: c->metadata_id = static_cast<SQLUINTEGER>(ival(value)); return SQL_SUCCESS;
      case SQL_ATTR_CURRENT_CATALOG:
        c->current_catalog = wide ? wide_in(static_cast<SQLWCHAR*>(value),
                                            len == SQL_NTS ? SQL_NTS : len / static_cast<SQLINTEGER>(sizeof(SQLWCHAR)))
                                  : narrow_in(static_cast<SQLCHAR*>(value), len);
        return SQL_SUCCESS;
      case SQL_ATTR_QUIET_MODE:
      case SQL_ATTR_PACKET_SIZE:
      case SQL_ATTR_TRACE:
      case SQL_ATTR_TRACEFILE:
      case SQL_ATTR_TRANSLATE_LIB:
      case SQL_ATTR_TRANSLATE_OPTION:
      case SQL_ATTR_ODBC_CURSORS:
        return SQL_SUCCESS;
      case SQL_ATTR_ASYNC_ENABLE:
        if (ival(value) != SQL_ASYNC_ENABLE_OFF) throw OdbcError("HYC00", "Asynchronous execution is not supported");
        return SQL_SUCCESS;
      case SQL_ATTR_ANSI_APP:
        // Same behaviour for ANSI and Unicode applications.
        return SQL_ERROR;
      // Statement attributes set at connection level (ODBC 2.x behaviour): accept.
      case SQL_ATTR_MAX_ROWS: case SQL_ATTR_QUERY_TIMEOUT: case SQL_ATTR_NOSCAN: case SQL_ATTR_MAX_LENGTH:
      case SQL_ATTR_CURSOR_TYPE: case SQL_ATTR_CONCURRENCY: case SQL_ROWSET_SIZE: case SQL_ATTR_RETRIEVE_DATA:
        return SQL_SUCCESS;
    }
    throw OdbcError("HYC00", "Connection attribute " + std::to_string(attr) + " is not supported");
  });
}

SQLRETURN GetConnectAttr(SQLHDBC h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER cap, SQLINTEGER* len, bool wide) {
  WITH_CONN(h);
  return guarded(c->diag, [&]() -> SQLRETURN {
    switch (attr) {
      case SQL_ATTR_AUTOCOMMIT: write_uint(value, c->autocommit, len); return SQL_SUCCESS;
      case SQL_ATTR_ACCESS_MODE: write_uint(value, c->access_mode, len); return SQL_SUCCESS;
      case SQL_ATTR_LOGIN_TIMEOUT: write_uint(value, c->login_timeout, len); return SQL_SUCCESS;
      case SQL_ATTR_CONNECTION_TIMEOUT: write_uint(value, c->connection_timeout, len); return SQL_SUCCESS;
      case SQL_ATTR_TXN_ISOLATION: write_uint(value, c->txn_isolation, len); return SQL_SUCCESS;
      case SQL_ATTR_METADATA_ID: write_uint(value, c->metadata_id, len); return SQL_SUCCESS;
      case SQL_ATTR_AUTO_IPD: write_uint(value, SQL_FALSE, len); return SQL_SUCCESS;
      case SQL_ATTR_ASYNC_ENABLE: write_uint(value, SQL_ASYNC_ENABLE_OFF, len); return SQL_SUCCESS;
      case SQL_ATTR_CONNECTION_DEAD: write_uint(value, c->connected ? SQL_CD_FALSE : SQL_CD_TRUE, len); return SQL_SUCCESS;
      case SQL_ATTR_PACKET_SIZE: write_uint(value, 0, len); return SQL_SUCCESS;
      case SQL_ATTR_CURRENT_CATALOG: {
        if (write_str(wide, true, c->current_catalog, value, cap, len))
          c->diag.add("01004", "String data, right truncated");
        return info_or_success(c->diag);
      }
    }
    throw OdbcError("HYC00", "Connection attribute " + std::to_string(attr) + " is not supported");
  });
}

SQLRETURN GetInfo(SQLHDBC h, SQLUSMALLINT type, SQLPOINTER value, SQLSMALLINT cap, SQLSMALLINT* len, bool wide) {
  WITH_CONN(h);
  return guarded(c->diag, [&]() -> SQLRETURN {
    InfoValue v = get_info(*c, type);
    switch (v.kind) {
      case InfoValue::Str:
        if (write_str(wide, true, v.s, value, cap, len)) c->diag.add("01004", "String data, right truncated");
        break;
      case InfoValue::U16: {
        SQLUSMALLINT n = static_cast<SQLUSMALLINT>(v.n);
        if (value) std::memcpy(value, &n, sizeof n);
        if (len) *len = sizeof n;
        break;
      }
      case InfoValue::U32: {
        SQLUINTEGER n = v.n;
        if (value) std::memcpy(value, &n, sizeof n);
        if (len) *len = sizeof n;
        break;
      }
    }
    return info_or_success(c->diag);
  });
}

SQLRETURN GetFunctions(SQLHDBC h, SQLUSMALLINT id, SQLUSMALLINT* supported) {
  WITH_CONN(h);
  return guarded(c->diag, [&]() -> SQLRETURN {
    if (!supported) throw OdbcError("HY009", "Null output pointer");
    if (id == SQL_API_ODBC3_ALL_FUNCTIONS) {
      std::memset(supported, 0, sizeof(SQLUSMALLINT) * SQL_API_ODBC3_ALL_FUNCTIONS_SIZE);
      for (int f = 0; f < SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * 16; ++f)
        if (driver_supports(static_cast<SQLUSMALLINT>(f))) supported[f >> 4] |= static_cast<SQLUSMALLINT>(1 << (f & 0xF));
    } else if (id == SQL_API_ALL_FUNCTIONS) {
      for (int f = 0; f < 100; ++f) supported[f] = driver_supports(static_cast<SQLUSMALLINT>(f)) ? SQL_TRUE : SQL_FALSE;
    } else {
      *supported = driver_supports(id) ? SQL_TRUE : SQL_FALSE;
    }
    return SQL_SUCCESS;
  });
}

SQLRETURN NativeSql(SQLHDBC h, const std::string& in, bool wide, SQLPOINTER out, SQLINTEGER cap, SQLINTEGER* len) {
  WITH_CONN(h);
  return guarded(c->diag, [&]() -> SQLRETURN {
    // Return the SOQL we would send: handy for debugging query folding.
    std::string result = in;
    if (c->connected) {
      try {
        QueryPlan p = build_plan(in, c->sf(), *c->meta);
        if (p.kind != QueryPlan::Constant) {
          std::string soql;
          for (auto& part : p.soql)
            soql += std::holds_alternative<std::string>(part) ? std::get<std::string>(part) : std::string("?");
          if (p.limit) soql += " LIMIT " + std::to_string(*p.limit);
          if (p.offset) soql += " OFFSET " + std::to_string(*p.offset);
          result = soql;
        }
      } catch (const OdbcError&) {
      }
    }
    if (write_str(wide, false, result, out, cap, len)) c->diag.add("01004", "String data, right truncated");
    return info_or_success(c->diag);
  });
}

SQLRETURN EndTran(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT) {
  if (type == SQL_HANDLE_ENV) return Env::from(h) ? SQL_SUCCESS : SQL_INVALID_HANDLE;
  if (type == SQL_HANDLE_DBC) return Conn::from(h) ? SQL_SUCCESS : SQL_INVALID_HANDLE;
  return SQL_ERROR;
}

// ====================================================================== statement attributes

SQLRETURN SetStmtAttr(SQLHSTMT h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    SQLULEN v = static_cast<SQLULEN>(ival(value));
    switch (attr) {
      case SQL_ATTR_ROW_ARRAY_SIZE:
      case SQL_ROWSET_SIZE:
        if (v == 0) throw OdbcError("HY024", "Row array size must be at least 1");
        s->row_array_size = v;
        return SQL_SUCCESS;
      case SQL_ATTR_ROW_BIND_TYPE: s->row_bind_type = v; return SQL_SUCCESS;
      case SQL_ATTR_ROW_BIND_OFFSET_PTR: s->row_bind_offset = static_cast<SQLULEN*>(value); return SQL_SUCCESS;
      case SQL_ATTR_ROW_STATUS_PTR: s->row_status = static_cast<SQLUSMALLINT*>(value); return SQL_SUCCESS;
      case SQL_ATTR_ROWS_FETCHED_PTR: s->rows_fetched = static_cast<SQLULEN*>(value); return SQL_SUCCESS;
      case SQL_ATTR_MAX_ROWS: s->max_rows = v; return SQL_SUCCESS;
      case SQL_ATTR_QUERY_TIMEOUT: s->query_timeout = v; return SQL_SUCCESS;
      case SQL_ATTR_MAX_LENGTH: s->max_length = v; return SQL_SUCCESS;
      case SQL_ATTR_NOSCAN: s->noscan = v; return SQL_SUCCESS;
      case SQL_ATTR_RETRIEVE_DATA: s->retrieve_data = v; return SQL_SUCCESS;
      case SQL_ATTR_METADATA_ID: s->metadata_id = v; return SQL_SUCCESS;
      case SQL_ATTR_PARAMSET_SIZE: s->paramset_size = v ? v : 1; return SQL_SUCCESS;
      case SQL_ATTR_PARAM_BIND_TYPE: s->param_bind_type = v; return SQL_SUCCESS;
      case SQL_ATTR_PARAM_BIND_OFFSET_PTR: s->param_bind_offset = static_cast<SQLULEN*>(value); return SQL_SUCCESS;
      case SQL_ATTR_PARAMS_PROCESSED_PTR: s->params_processed = static_cast<SQLULEN*>(value); return SQL_SUCCESS;
      case SQL_ATTR_PARAM_STATUS_PTR: s->param_status = static_cast<SQLUSMALLINT*>(value); return SQL_SUCCESS;
      case SQL_ATTR_CURSOR_TYPE:
        if (v != SQL_CURSOR_FORWARD_ONLY) { s->diag.add("01S02", "Cursor type changed to forward-only"); return SQL_SUCCESS_WITH_INFO; }
        return SQL_SUCCESS;
      case SQL_ATTR_CONCURRENCY:
        if (v != SQL_CONCUR_READ_ONLY) { s->diag.add("01S02", "Concurrency changed to read-only"); return SQL_SUCCESS_WITH_INFO; }
        return SQL_SUCCESS;
      case SQL_ATTR_CURSOR_SCROLLABLE:
        if (v != SQL_NONSCROLLABLE) throw OdbcError("HYC00", "Scrollable cursors are not supported");
        return SQL_SUCCESS;
      case SQL_ATTR_CURSOR_SENSITIVITY:
      case SQL_ATTR_SIMULATE_CURSOR:
      case SQL_ATTR_KEYSET_SIZE:
      case SQL_ATTR_ENABLE_AUTO_IPD:
        return SQL_SUCCESS;
      case SQL_ATTR_ASYNC_ENABLE:
        if (v != SQL_ASYNC_ENABLE_OFF) throw OdbcError("HYC00", "Asynchronous execution is not supported");
        return SQL_SUCCESS;
      case SQL_ATTR_USE_BOOKMARKS:
        if (v != SQL_UB_OFF) throw OdbcError("HYC00", "Bookmarks are not supported");
        return SQL_SUCCESS;
    }
    throw OdbcError("HYC00", "Statement attribute " + std::to_string(attr) + " is not supported");
  });
}

SQLRETURN GetStmtAttr(SQLHSTMT h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER, SQLINTEGER* len) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    switch (attr) {
      case SQL_ATTR_APP_ROW_DESC: write_ptr(value, &s->ard, len); return SQL_SUCCESS;
      case SQL_ATTR_APP_PARAM_DESC: write_ptr(value, &s->apd, len); return SQL_SUCCESS;
      case SQL_ATTR_IMP_ROW_DESC: write_ptr(value, &s->ird, len); return SQL_SUCCESS;
      case SQL_ATTR_IMP_PARAM_DESC: write_ptr(value, &s->ipd, len); return SQL_SUCCESS;
      case SQL_ATTR_ROW_ARRAY_SIZE: case SQL_ROWSET_SIZE: write_ulen(value, s->row_array_size, len); return SQL_SUCCESS;
      case SQL_ATTR_ROW_BIND_TYPE: write_ulen(value, s->row_bind_type, len); return SQL_SUCCESS;
      case SQL_ATTR_ROW_BIND_OFFSET_PTR: write_ptr(value, s->row_bind_offset, len); return SQL_SUCCESS;
      case SQL_ATTR_ROW_STATUS_PTR: write_ptr(value, s->row_status, len); return SQL_SUCCESS;
      case SQL_ATTR_ROWS_FETCHED_PTR: write_ptr(value, s->rows_fetched, len); return SQL_SUCCESS;
      case SQL_ATTR_MAX_ROWS: write_ulen(value, s->max_rows, len); return SQL_SUCCESS;
      case SQL_ATTR_QUERY_TIMEOUT: write_ulen(value, s->query_timeout, len); return SQL_SUCCESS;
      case SQL_ATTR_MAX_LENGTH: write_ulen(value, s->max_length, len); return SQL_SUCCESS;
      case SQL_ATTR_NOSCAN: write_ulen(value, s->noscan, len); return SQL_SUCCESS;
      case SQL_ATTR_RETRIEVE_DATA: write_ulen(value, s->retrieve_data, len); return SQL_SUCCESS;
      case SQL_ATTR_METADATA_ID: write_ulen(value, s->metadata_id, len); return SQL_SUCCESS;
      case SQL_ATTR_PARAMSET_SIZE: write_ulen(value, s->paramset_size, len); return SQL_SUCCESS;
      case SQL_ATTR_PARAM_BIND_TYPE: write_ulen(value, s->param_bind_type, len); return SQL_SUCCESS;
      case SQL_ATTR_PARAM_BIND_OFFSET_PTR: write_ptr(value, s->param_bind_offset, len); return SQL_SUCCESS;
      case SQL_ATTR_PARAMS_PROCESSED_PTR: write_ptr(value, s->params_processed, len); return SQL_SUCCESS;
      case SQL_ATTR_PARAM_STATUS_PTR: write_ptr(value, s->param_status, len); return SQL_SUCCESS;
      case SQL_ATTR_CURSOR_TYPE: write_ulen(value, SQL_CURSOR_FORWARD_ONLY, len); return SQL_SUCCESS;
      case SQL_ATTR_CONCURRENCY: write_ulen(value, SQL_CONCUR_READ_ONLY, len); return SQL_SUCCESS;
      case SQL_ATTR_CURSOR_SCROLLABLE: write_ulen(value, SQL_NONSCROLLABLE, len); return SQL_SUCCESS;
      case SQL_ATTR_CURSOR_SENSITIVITY: write_ulen(value, SQL_INSENSITIVE, len); return SQL_SUCCESS;
      case SQL_ATTR_ASYNC_ENABLE: write_ulen(value, SQL_ASYNC_ENABLE_OFF, len); return SQL_SUCCESS;
      case SQL_ATTR_USE_BOOKMARKS: write_ulen(value, SQL_UB_OFF, len); return SQL_SUCCESS;
      case SQL_ATTR_SIMULATE_CURSOR: write_ulen(value, SQL_SC_NON_UNIQUE, len); return SQL_SUCCESS;
      case SQL_ATTR_KEYSET_SIZE: write_ulen(value, 0, len); return SQL_SUCCESS;
      case SQL_ATTR_ENABLE_AUTO_IPD: write_ulen(value, SQL_FALSE, len); return SQL_SUCCESS;
      case SQL_ATTR_ROW_NUMBER: write_ulen(value, s->positioned ? s->row_number : 0, len); return SQL_SUCCESS;
    }
    throw OdbcError("HYC00", "Statement attribute " + std::to_string(attr) + " is not supported");
  });
}

// ====================================================================== prepare / execute

static void do_prepare(Stmt* s, const std::string& sql) {
  s->close_cursor();
  s->plan.reset();
  s->sql = sql;
  LOG_DEBUG("Prepare: " << sql);
  s->plan = build_plan(sql, s->conn->sf(), *s->conn->meta);
}

static SQLRETURN do_execute(Stmt* s) {
  if (!s->plan) throw OdbcError("HY010", "Function sequence error: statement not prepared");
  s->close_cursor();
  Conn& c = *s->conn;
  const QueryPlan& plan = *s->plan;

  std::vector<Lit> params;
  SQLLEN off = s->param_bind_offset ? static_cast<SQLLEN>(*s->param_bind_offset) : 0;
  for (int i = 0; i < plan.param_count; ++i) {
    if (i >= static_cast<int>(s->params.size()) || !s->params[i].bound)
      throw OdbcError("07002", "Parameter " + std::to_string(i + 1) + " is not bound");
    params.push_back(read_param(s->params[i], off));
  }
  if (s->paramset_size > 1 && plan.param_count > 0)
    s->diag.add("01000", "Parameter arrays are not supported for SELECT; only the first parameter set was used");
  if (s->params_processed) *s->params_processed = 1;
  if (s->param_status) s->param_status[0] = SQL_PARAM_SUCCESS;

  std::unique_ptr<Cursor> cur;
  if (plan.kind == QueryPlan::Constant) {
    Row row;
    for (auto& col : plan.columns) row.push_back(col.const_value);
    cur = std::make_unique<VectorCursor>(plan.columns, std::vector<Row>{row});
  } else if (plan.always_empty) {
    cur = std::make_unique<VectorCursor>(plan.columns, std::vector<Row>{});
  } else {
    std::string soql = render_soql(plan, params, s->max_rows);
    if (plan.kind == QueryPlan::Count) {
      json j = c.sf().query(soql);
      cur = std::make_unique<VectorCursor>(plan.columns,
                                           std::vector<Row>{Row{std::to_string(j.value("totalSize", 0LL))}});
    } else {
      cur = std::make_unique<SoqlCursor>(c.sf(), plan.columns, soql, &s->cancel, s->max_rows);
    }
  }
  s->set_cursor(std::move(cur));
  s->getdata.assign(s->cursor->columns().size() + 1, {});

  if (c.client && c.client->api_max() > 0 && c.client->api_used() * 10 >= c.client->api_max() * 9)
    s->diag.add("01000", "Salesforce API usage is at " + std::to_string(c.client->api_used()) + " of " +
                             std::to_string(c.client->api_max()) + " daily requests");
  return info_or_success(s->diag);
}

SQLRETURN Prepare(SQLHSTMT h, const std::string& sql) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN { do_prepare(s, sql); return SQL_SUCCESS; });
}

SQLRETURN Execute(SQLHSTMT h) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN { return do_execute(s); });
}

SQLRETURN ExecDirect(SQLHSTMT h, const std::string& sql) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    do_prepare(s, sql);
    return do_execute(s);
  });
}

SQLRETURN NumResultCols(SQLHSTMT h, SQLSMALLINT* n) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    auto* cols = s->columns();
    if (n) *n = cols ? static_cast<SQLSMALLINT>(cols->size()) : 0;
    return SQL_SUCCESS;
  });
}

SQLRETURN DescribeCol(SQLHSTMT h, SQLUSMALLINT col, bool wide, SQLPOINTER name, SQLSMALLINT cap,
                      SQLSMALLINT* name_len, SQLSMALLINT* type, SQLULEN* size, SQLSMALLINT* digits,
                      SQLSMALLINT* nullable) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    const ColumnInfo& c = column_at(s, col);
    if (write_str(wide, false, c.name, name, cap, name_len)) s->diag.add("01004", "Column name truncated");
    if (type) *type = reported_type(s, c.sql_type);
    if (size) *size = c.column_size;
    if (digits) *digits = c.decimal_digits;
    if (nullable) *nullable = c.nullable;
    return info_or_success(s->diag);
  });
}

SQLRETURN ColAttribute(SQLHSTMT h, SQLUSMALLINT col, SQLUSMALLINT field, bool wide, SQLPOINTER char_attr,
                       SQLSMALLINT cap, SQLSMALLINT* len, SQLLEN* num) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (field == SQL_DESC_COUNT || field == SQL_COLUMN_COUNT) {
      auto* cols = s->columns();
      if (num) *num = cols ? static_cast<SQLLEN>(cols->size()) : 0;
      return SQL_SUCCESS;
    }
    const ColumnInfo& c = column_at(s, col);
    auto text = [&](const std::string& v) {
      if (write_str(wide, true, v, char_attr, cap, len)) s->diag.add("01004", "String data, right truncated");
      return info_or_success(s->diag);
    };
    auto number = [&](SQLLEN v) {
      if (num) *num = v;
      return SQL_SUCCESS;
    };
    SQLSMALLINT t = c.sql_type;
    switch (field) {
      case SQL_DESC_NAME:
      case SQL_COLUMN_NAME:
      case SQL_DESC_LABEL: return text(c.name);
      case SQL_DESC_BASE_COLUMN_NAME: return text(c.base_column);
      case SQL_DESC_TABLE_NAME:
      case SQL_DESC_BASE_TABLE_NAME: return text(c.base_table);
      case SQL_DESC_SCHEMA_NAME:
      case SQL_DESC_CATALOG_NAME: return text("");
      case SQL_DESC_TYPE_NAME:
      case SQL_DESC_LOCAL_TYPE_NAME: return text(c.type_name);
      case SQL_DESC_LITERAL_PREFIX:
      case SQL_DESC_LITERAL_SUFFIX: return text(is_char_type(t) || is_datetime_type(t) ? "'" : "");
      case SQL_DESC_CONCISE_TYPE: return number(reported_type(s, t));
      case SQL_DESC_TYPE: return number(is_datetime_type(t) ? SQL_DATETIME : t);
      case SQL_DESC_LENGTH: return number(static_cast<SQLLEN>(c.column_size));
      case SQL_COLUMN_LENGTH:
      case SQL_DESC_OCTET_LENGTH: return number(octet_length(t, c.column_size));
      case SQL_DESC_PRECISION:
      case SQL_COLUMN_PRECISION:
        return number(t == SQL_TYPE_TIMESTAMP ? c.decimal_digits : static_cast<SQLLEN>(c.column_size));
      case SQL_DESC_SCALE:
      case SQL_COLUMN_SCALE: return number(c.decimal_digits);
      case SQL_DESC_DISPLAY_SIZE: return number(display_size(t, c.column_size));
      case SQL_DESC_NULLABLE:
      case SQL_COLUMN_NULLABLE: return number(c.nullable);
      case SQL_DESC_UNSIGNED: return number(is_numeric_type(t) ? SQL_FALSE : SQL_TRUE);
      case SQL_DESC_FIXED_PREC_SCALE: return number(SQL_FALSE);
      case SQL_DESC_UPDATABLE: return number(SQL_ATTR_READONLY);
      case SQL_DESC_AUTO_UNIQUE_VALUE: return number(c.auto_unique ? SQL_TRUE : SQL_FALSE);
      case SQL_DESC_CASE_SENSITIVE: return number(is_char_type(t) && c.case_sensitive ? SQL_TRUE : SQL_FALSE);
      case SQL_DESC_SEARCHABLE:
        return number(!c.searchable || t == SQL_WLONGVARCHAR ? SQL_PRED_NONE : SQL_PRED_SEARCHABLE);
      case SQL_DESC_UNNAMED: return number(c.name.empty() ? SQL_UNNAMED : SQL_NAMED);
      case SQL_DESC_NUM_PREC_RADIX: return number(is_numeric_type(t) ? 10 : 0);
    }
    throw OdbcError("HY091", "Invalid descriptor field identifier " + std::to_string(field));
  });
}

// ====================================================================== binding & fetch

SQLRETURN BindCol(SQLHSTMT h, SQLUSMALLINT col, SQLSMALLINT c_type, SQLPOINTER target, SQLLEN cap, SQLLEN* ind) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (col == 0) throw OdbcError("07009", "Bookmark columns are not supported");
    if (cap < 0) throw OdbcError("HY090", "Invalid buffer length");
    if (s->cols.size() <= col) s->cols.resize(col + 1);
    ColBinding& b = s->cols[col];
    if (!target && !ind) { b = ColBinding{}; return SQL_SUCCESS; }
    b.bound = true;
    b.c_type = c_type;
    b.target = target;
    b.buffer_len = cap;
    b.ind = ind;
    return SQL_SUCCESS;
  });
}

static SQLRETURN do_fetch(Stmt* s) {
  if (!s->cursor) throw OdbcError("24000", "Invalid cursor state: no open result set");
  const auto& cols = s->cursor->columns();
  SQLULEN n = s->row_array_size;
  s->rowset.clear();
  s->positioned = false;
  for (SQLULEN i = 0; i < n; ++i) {
    Row r;
    if (!s->cursor->next(r)) break;
    s->rowset.push_back(std::move(r));
  }
  SQLULEN got = s->rowset.size();
  if (s->rows_fetched) *s->rows_fetched = got;
  if (s->row_status)
    for (SQLULEN i = got; i < n; ++i) s->row_status[i] = SQL_ROW_NOROW;
  if (got == 0) return SQL_NO_DATA;

  s->row_number += got;
  s->positioned = true;
  s->getdata.assign(cols.size() + 1, {});

  SQLLEN off = s->row_bind_offset ? static_cast<SQLLEN>(*s->row_bind_offset) : 0;
  bool by_col = s->row_bind_type == SQL_BIND_BY_COLUMN;
  int errors = 0, infos = 0;
  for (SQLULEN i = 0; i < got; ++i) {
    SQLRETURN row_rc = SQL_SUCCESS;
    for (size_t k = 1; k < s->cols.size() && k <= cols.size(); ++k) {
      const ColBinding& b = s->cols[k];
      if (!b.bound) continue;
      SQLSMALLINT ct = b.c_type == SQL_C_DEFAULT ? default_c_type(cols[k - 1].sql_type) : b.c_type;
      SQLLEN elem = by_col ? (c_type_size(ct) ? c_type_size(ct) : b.buffer_len) : static_cast<SQLLEN>(s->row_bind_type);
      SQLLEN ind_elem = by_col ? static_cast<SQLLEN>(sizeof(SQLLEN)) : static_cast<SQLLEN>(s->row_bind_type);
      SQLPOINTER target = b.target ? static_cast<char*>(b.target) + off + static_cast<SQLLEN>(i) * elem : nullptr;
      SQLLEN* ind = b.ind ? reinterpret_cast<SQLLEN*>(reinterpret_cast<char*>(b.ind) + off + static_cast<SQLLEN>(i) * ind_elem)
                          : nullptr;
      SQLRETURN rc = get_value(s->diag, cols[k - 1], s->rowset[i][k - 1], ct, target, b.buffer_len, ind, nullptr);
      if (rc == SQL_ERROR) row_rc = SQL_ERROR;
      else if (rc == SQL_SUCCESS_WITH_INFO && row_rc == SQL_SUCCESS) row_rc = SQL_SUCCESS_WITH_INFO;
    }
    if (row_rc == SQL_ERROR) ++errors;
    if (row_rc == SQL_SUCCESS_WITH_INFO) ++infos;
    if (s->row_status)
      s->row_status[i] = row_rc == SQL_SUCCESS ? SQL_ROW_SUCCESS
                         : row_rc == SQL_ERROR ? SQL_ROW_ERROR : SQL_ROW_SUCCESS_WITH_INFO;
  }
  if (errors == static_cast<int>(got) && got == 1) return SQL_ERROR;
  return (errors || infos) ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

SQLRETURN Fetch(SQLHSTMT h) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN { return do_fetch(s); });
}

SQLRETURN FetchScroll(SQLHSTMT h, SQLSMALLINT orientation, SQLLEN) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (orientation != SQL_FETCH_NEXT) throw OdbcError("HY106", "Only SQL_FETCH_NEXT is supported (forward-only cursor)");
    return do_fetch(s);
  });
}

SQLRETURN GetData(SQLHSTMT h, SQLUSMALLINT col, SQLSMALLINT c_type, SQLPOINTER target, SQLLEN cap, SQLLEN* ind) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (!s->cursor || !s->positioned || s->rowset.empty())
      throw OdbcError("24000", "Invalid cursor state: no current row");
    const ColumnInfo& c = column_at(s, col);
    if (cap < 0) throw OdbcError("HY090", "Invalid buffer length");
    if (s->getdata.size() <= col) s->getdata.resize(col + 1);
    return get_value(s->diag, c, s->rowset[0][col - 1], c_type, target, cap, ind, &s->getdata[col]);
  });
}

SQLRETURN RowCount(SQLHSTMT h, SQLLEN* n) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (n) *n = s->cursor ? s->cursor->row_count() : -1;
    return SQL_SUCCESS;
  });
}

SQLRETURN MoreResults(SQLHSTMT h) {
  WITH_STMT(h);
  s->diag.clear();
  return SQL_NO_DATA;
}

SQLRETURN CloseCursor(SQLHSTMT h) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (!s->cursor) throw OdbcError("24000", "Invalid cursor state: no open cursor");
    s->close_cursor();
    return SQL_SUCCESS;
  });
}

SQLRETURN FreeStmt(SQLHSTMT h, SQLUSMALLINT option) {
  if (option == SQL_DROP) return FreeHandle(SQL_HANDLE_STMT, h);
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    switch (option) {
      case SQL_CLOSE: s->close_cursor(); return SQL_SUCCESS;
      case SQL_UNBIND: s->cols.clear(); return SQL_SUCCESS;
      case SQL_RESET_PARAMS: s->params.clear(); return SQL_SUCCESS;
    }
    throw OdbcError("HY092", "Invalid option");
  });
}

SQLRETURN Cancel(SQLHSTMT h) {
  Stmt* s = Stmt::from(h);
  if (!s) return SQL_INVALID_HANDLE;
  s->cancel = true;  // no lock: may be called from another thread during a fetch
  return SQL_SUCCESS;
}

// ====================================================================== parameters

SQLRETURN BindParameter(SQLHSTMT h, SQLUSMALLINT num, SQLSMALLINT io, SQLSMALLINT c_type, SQLSMALLINT sql_type,
                        SQLULEN size, SQLSMALLINT digits, SQLPOINTER value, SQLLEN cap, SQLLEN* ind) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (num == 0) throw OdbcError("07009", "Invalid parameter number");
    if (io != SQL_PARAM_INPUT) throw OdbcError("HYC00", "Only input parameters are supported");
    if (s->params.size() < num) s->params.resize(num);
    ParamBinding& b = s->params[num - 1];
    b.bound = true;
    b.io_type = io;
    b.c_type = c_type;
    b.sql_type = sql_type;
    b.column_size = size;
    b.decimal_digits = digits;
    b.value = value;
    b.buffer_len = cap;
    b.str_len_or_ind = ind;
    return SQL_SUCCESS;
  });
}

SQLRETURN NumParams(SQLHSTMT h, SQLSMALLINT* n) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (!s->plan) throw OdbcError("HY010", "Function sequence error: statement not prepared");
    if (n) *n = static_cast<SQLSMALLINT>(s->plan->param_count);
    return SQL_SUCCESS;
  });
}

SQLRETURN DescribeParam(SQLHSTMT h, SQLUSMALLINT num, SQLSMALLINT* type, SQLULEN* size, SQLSMALLINT* digits,
                        SQLSMALLINT* nullable) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (!s->plan) throw OdbcError("HY010", "Function sequence error: statement not prepared");
    if (num == 0 || num > s->plan->params.size()) throw OdbcError("07009", "Invalid parameter number");
    const ParamSlot& slot = s->plan->params[num - 1];
    FieldMeta f;
    f.type = slot.index < 0 ? "string" : slot.sf_type;
    f.length = 255;
    SqlType t = map_field_type(f);
    if (type) *type = reported_type(s, t.sql_type);
    if (size) *size = t.column_size;
    if (digits) *digits = t.decimal_digits;
    if (nullable) *nullable = SQL_NULLABLE;
    return SQL_SUCCESS;
  });
}

SQLRETURN GetCursorName(SQLHSTMT h, bool wide, SQLPOINTER out, SQLSMALLINT cap, SQLSMALLINT* len) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (s->cursor_name.empty()) {
      char b[40];
      std::snprintf(b, sizeof b, "SQL_CUR%" PRIxPTR, static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(s)));
      s->cursor_name = b;
    }
    if (write_str(wide, false, s->cursor_name, out, cap, len)) s->diag.add("01004", "String data, right truncated");
    return info_or_success(s->diag);
  });
}

SQLRETURN SetCursorName(SQLHSTMT h, const std::string& name) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    if (name.empty()) throw OdbcError("34000", "Invalid cursor name");
    s->cursor_name = name;
    return SQL_SUCCESS;
  });
}

// ====================================================================== catalog

SQLRETURN Tables(SQLHSTMT h, const OptStr& cat, const OptStr& schema, const OptStr& table, const OptStr& types) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    open_catalog(s, catalog_tables(*s->conn, s->metadata_id == SQL_TRUE, cat, schema, table, types));
    return SQL_SUCCESS;
  });
}

SQLRETURN Columns(SQLHSTMT h, const OptStr& cat, const OptStr& schema, const OptStr& table, const OptStr& column) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    open_catalog(s, catalog_columns(*s->conn, s->metadata_id == SQL_TRUE, cat, schema, table, column));
    return SQL_SUCCESS;
  });
}

SQLRETURN PrimaryKeys(SQLHSTMT h, const OptStr& cat, const OptStr& schema, const OptStr& table) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    open_catalog(s, catalog_primary_keys(*s->conn, cat, schema, table));
    return SQL_SUCCESS;
  });
}

SQLRETURN ForeignKeys(SQLHSTMT h, const OptStr& pk_table, const OptStr& fk_table) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    open_catalog(s, catalog_foreign_keys(*s->conn, pk_table, fk_table));
    return SQL_SUCCESS;
  });
}

SQLRETURN Statistics(SQLHSTMT h, const OptStr& table) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    open_catalog(s, catalog_statistics(*s->conn, table));
    return SQL_SUCCESS;
  });
}

SQLRETURN SpecialColumns(SQLHSTMT h, SQLUSMALLINT id_type, const OptStr& table) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    open_catalog(s, catalog_special_columns(*s->conn, id_type, table));
    return SQL_SUCCESS;
  });
}

SQLRETURN Procedures(SQLHSTMT h) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN { open_catalog(s, catalog_procedures()); return SQL_SUCCESS; });
}

SQLRETURN ProcedureColumns(SQLHSTMT h) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN { open_catalog(s, catalog_procedure_columns()); return SQL_SUCCESS; });
}

SQLRETURN GetTypeInfo(SQLHSTMT h, SQLSMALLINT type) {
  WITH_STMT(h);
  return guarded(s->diag, [&]() -> SQLRETURN {
    open_catalog(s, type_info(type, s->conn->env->odbc_version));
    return SQL_SUCCESS;
  });
}

// ====================================================================== diagnostics

static Diag* diag_for(SQLSMALLINT type, SQLHANDLE h) {
  switch (type) {
    case SQL_HANDLE_ENV: { auto* e = Env::from(h); return e ? &e->diag : nullptr; }
    case SQL_HANDLE_DBC: { auto* c = Conn::from(h); return c ? &c->diag : nullptr; }
    case SQL_HANDLE_STMT: { auto* s = Stmt::from(h); return s ? &s->diag : nullptr; }
    case SQL_HANDLE_DESC: { auto* d = Desc::from(h); return d ? &d->diag : nullptr; }
  }
  return nullptr;
}

SQLRETURN GetDiagRec(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT rec, bool wide, SQLPOINTER state,
                     SQLINTEGER* native, SQLPOINTER msg, SQLSMALLINT cap, SQLSMALLINT* msg_len) {
  Diag* d = diag_for(type, h);
  if (!d) return SQL_INVALID_HANDLE;
  if (rec < 1 || cap < 0) return SQL_ERROR;
  if (static_cast<size_t>(rec) > d->recs.size()) return SQL_NO_DATA;
  const DiagRec& r = d->recs[rec - 1];
  if (state) {
    SQLSMALLINT dummy;
    write_str(wide, false, r.state, state, 6, &dummy);
  }
  if (native) *native = r.native;
  bool trunc = write_str(wide, false, r.message, msg, cap, msg_len);
  return trunc ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

SQLRETURN GetDiagField(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT rec, SQLSMALLINT field, bool wide,
                       SQLPOINTER value, SQLSMALLINT cap, SQLSMALLINT* len) {
  Diag* d = diag_for(type, h);
  if (!d) return SQL_INVALID_HANDLE;
  auto put_int = [&](SQLINTEGER v) { if (value) std::memcpy(value, &v, sizeof v); return SQL_SUCCESS; };
  auto put_len = [&](SQLLEN v) { if (value) std::memcpy(value, &v, sizeof v); return SQL_SUCCESS; };
  auto put_text = [&](const std::string& v) {
    return write_str(wide, true, v, value, cap, len) ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
  };
  // Header fields
  switch (field) {
    case SQL_DIAG_NUMBER: return put_int(static_cast<SQLINTEGER>(d->recs.size()));
    case SQL_DIAG_RETURNCODE: { SQLRETURN r = d->last_return; if (value) std::memcpy(value, &r, sizeof r); return SQL_SUCCESS; }
    case SQL_DIAG_ROW_COUNT: return put_len(0);
    case SQL_DIAG_CURSOR_ROW_COUNT: {
      Stmt* s = type == SQL_HANDLE_STMT ? Stmt::from(h) : nullptr;
      return put_len(s && s->cursor ? s->cursor->row_count() : 0);
    }
    case SQL_DIAG_DYNAMIC_FUNCTION: return put_text("SELECT CURSOR");
    case SQL_DIAG_DYNAMIC_FUNCTION_CODE: return put_int(SQL_DIAG_SELECT_CURSOR);
  }
  if (rec < 1) return SQL_ERROR;
  if (static_cast<size_t>(rec) > d->recs.size()) return SQL_NO_DATA;
  const DiagRec& r = d->recs[rec - 1];
  std::string cls = r.state.substr(0, 2);
  bool odbc_class = cls == "HY" || cls == "IM";
  switch (field) {
    case SQL_DIAG_SQLSTATE: return put_text(r.state);
    case SQL_DIAG_NATIVE: return put_int(r.native);
    case SQL_DIAG_MESSAGE_TEXT: return put_text(r.message);
    case SQL_DIAG_CLASS_ORIGIN: return put_text(odbc_class ? "ODBC 3.0" : "ISO 9075");
    case SQL_DIAG_SUBCLASS_ORIGIN:
      return put_text(odbc_class || (r.state.size() > 2 && r.state[2] == 'S') ? "ODBC 3.0" : "ISO 9075");
    case SQL_DIAG_CONNECTION_NAME: return put_text("");
    case SQL_DIAG_SERVER_NAME: {
      Conn* c = type == SQL_HANDLE_DBC ? Conn::from(h) : type == SQL_HANDLE_STMT ? Stmt::from(h)->conn : nullptr;
      return put_text(c && c->client ? c->client->instance_url() : "");
    }
    case SQL_DIAG_COLUMN_NUMBER: return put_int(SQL_COLUMN_NUMBER_UNKNOWN);
    case SQL_DIAG_ROW_NUMBER: return put_len(SQL_ROW_NUMBER_UNKNOWN);
  }
  return SQL_ERROR;
}

}  // namespace sf::api
