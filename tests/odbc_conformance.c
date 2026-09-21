/* Low-level ODBC conformance checks against the unixODBC driver manager.
 *
 * These exercise the paths a Python client never reaches but SSIS, Alteryx and
 * Excel rely on: block fetching with row arrays, row-wise binding, chunked
 * SQLGetData, SQLColAttribute, SQL_C_NUMERIC, SQLDescribeParam and SQLCancel.
 *
 * Usage: odbc_conformance "<connection string>"
 */
#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                              \
  do {                                                \
    ++checks;                                         \
    if (!(cond)) {                                    \
      ++failures;                                     \
      printf("  FAIL: ");                             \
      printf(__VA_ARGS__);                            \
      printf("\n    at %s:%d\n", __FILE__, __LINE__); \
    }                                                 \
  } while (0)

static void show_diag(SQLSMALLINT type, SQLHANDLE h) {
  SQLCHAR state[7], msg[1024];
  SQLINTEGER native;
  SQLSMALLINT len;
  for (SQLSMALLINT i = 1; SQLGetDiagRec(type, h, i, state, &native, msg, sizeof msg, &len) == SQL_SUCCESS; ++i)
    printf("    [%s] %s\n", state, msg);
}

static int ok(SQLRETURN rc) { return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO; }

#define MUST(rc, h, type, what)                \
  do {                                         \
    SQLRETURN _rc = (rc);                      \
    ++checks;                                  \
    if (!ok(_rc)) {                            \
      ++failures;                              \
      printf("  FAIL: %s (rc=%d)\n", what, _rc); \
      show_diag(type, h);                      \
      return;                                  \
    }                                          \
  } while (0)

static SQLHDBC g_dbc;

/* ---------------------------------------------------------------- tests */

/* Column-wise block fetch: one SQLFetch fills arrays for several rows. */
static void test_block_fetch_column_wise(void) {
  printf("block fetch, column-wise binding\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);

  enum { ROWS = 8 };
  SQLCHAR ids[ROWS][19];
  SQLWCHAR names[ROWS][128];
  SQLLEN id_len[ROWS], name_len[ROWS];
  SQLUSMALLINT status[ROWS];
  SQLULEN fetched = 0;

  MUST(SQLSetStmtAttr(st, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)(intptr_t)ROWS, 0), st, SQL_HANDLE_STMT,
       "set row array size");
  MUST(SQLSetStmtAttr(st, SQL_ATTR_ROW_STATUS_PTR, status, 0), st, SQL_HANDLE_STMT, "set row status ptr");
  MUST(SQLSetStmtAttr(st, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), st, SQL_HANDLE_STMT, "set rows fetched ptr");
  MUST(SQLBindCol(st, 1, SQL_C_CHAR, ids, sizeof ids[0], id_len), st, SQL_HANDLE_STMT, "bind col 1");
  MUST(SQLBindCol(st, 2, SQL_C_WCHAR, names, sizeof names[0], name_len), st, SQL_HANDLE_STMT, "bind col 2");
  MUST(SQLExecDirect(st, (SQLCHAR*)"SELECT Id, Name FROM Account ORDER BY Id", SQL_NTS), st, SQL_HANDLE_STMT,
       "execute");

  SQLLEN total = 0;
  int batches = 0;
  SQLRETURN rc;
  while (ok(rc = SQLFetch(st))) {
    CHECK(fetched > 0 && fetched <= ROWS, "rows fetched out of range: %ld", (long)fetched);
    for (SQLULEN i = 0; i < fetched; ++i) {
      CHECK(status[i] == SQL_ROW_SUCCESS || status[i] == SQL_ROW_SUCCESS_WITH_INFO,
            "row %ld status %u", (long)i, status[i]);
      CHECK(id_len[i] == 18, "row %ld id length %ld", (long)i, (long)id_len[i]);
      CHECK(strlen((char*)ids[i]) == 18, "row %ld id text '%s'", (long)i, ids[i]);
      CHECK(name_len[i] > 0, "row %ld name length %ld", (long)i, (long)name_len[i]);
    }
    total += (SQLLEN)fetched;
    ++batches;
  }
  CHECK(rc == SQL_NO_DATA, "fetch loop ended with rc=%d", rc);
  CHECK(total == 25, "expected 25 rows, got %ld", (long)total);
  CHECK(batches == 4, "expected 4 batches of 8, got %d", batches);
  /* the trailing partial batch must mark the unused slots */
  CHECK(status[1] == SQL_ROW_NOROW, "unused row slot not marked NOROW (%u)", status[1]);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

/* Row-wise binding: one struct per row, stride given by SQL_ATTR_ROW_BIND_TYPE. */
static void test_block_fetch_row_wise(void) {
  printf("block fetch, row-wise binding\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);

  typedef struct {
    SQLCHAR id[19];
    SQLLEN id_len;
    SQLINTEGER employees;
    SQLLEN emp_len;
  } RowBuf;
  enum { ROWS = 5 };
  RowBuf buf[ROWS];
  SQLULEN fetched = 0;

  MUST(SQLSetStmtAttr(st, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)sizeof(RowBuf), 0), st, SQL_HANDLE_STMT,
       "set row bind type");
  MUST(SQLSetStmtAttr(st, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)(intptr_t)ROWS, 0), st, SQL_HANDLE_STMT,
       "set row array size");
  MUST(SQLSetStmtAttr(st, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), st, SQL_HANDLE_STMT, "set rows fetched");
  MUST(SQLBindCol(st, 1, SQL_C_CHAR, buf[0].id, sizeof buf[0].id, &buf[0].id_len), st, SQL_HANDLE_STMT,
       "bind id");
  MUST(SQLBindCol(st, 2, SQL_C_SLONG, &buf[0].employees, 0, &buf[0].emp_len), st, SQL_HANDLE_STMT, "bind int");
  MUST(SQLExecDirect(st, (SQLCHAR*)"SELECT Id, NumberOfEmployees FROM Account ORDER BY Id LIMIT 5", SQL_NTS),
       st, SQL_HANDLE_STMT, "execute");

  MUST(SQLFetch(st), st, SQL_HANDLE_STMT, "fetch");
  CHECK(fetched == 5, "expected 5 rows, got %ld", (long)fetched);
  for (SQLULEN i = 0; i < fetched; ++i) {
    CHECK(strlen((char*)buf[i].id) == 18, "row %ld id '%s'", (long)i, buf[i].id);
    if (buf[i].emp_len != SQL_NULL_DATA)
      CHECK(buf[i].employees > 0, "row %ld employees %d", (long)i, buf[i].employees);
  }
  /* the 6th record has a NULL employee count in the fixture */
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

/* SQLGetData in chunks must never split a UTF-8 sequence or a surrogate pair. */
static void test_getdata_chunks(void) {
  printf("chunked SQLGetData\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
  MUST(SQLExecDirect(st, (SQLCHAR*)"SELECT Description FROM Account WHERE Id = '001000000000000001'", SQL_NTS),
       st, SQL_HANDLE_STMT, "execute");
  MUST(SQLFetch(st), st, SQL_HANDLE_STMT, "fetch");

  char piece[64];
  SQLLEN ind;
  size_t total = 0;
  int chunks = 0;
  SQLRETURN rc;
  while ((rc = SQLGetData(st, 1, SQL_C_CHAR, piece, sizeof piece, &ind)) != SQL_NO_DATA) {
    CHECK(ok(rc), "getdata rc=%d", rc);
    if (!ok(rc)) break;
    total += strlen(piece);
    ++chunks;
    if (rc == SQL_SUCCESS) break;                 /* last chunk */
    CHECK(ind > 0, "remaining length not reported (%ld)", (long)ind);
    if (chunks > 1000) break;
  }
  CHECK(total == 10200, "reassembled length %zu, expected 10200", total);
  CHECK(chunks > 100, "expected many chunks, got %d", chunks);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

/* Multi-byte text through a tight SQL_C_CHAR buffer must stay valid UTF-8. */
static void test_getdata_never_splits_utf8(void) {
  printf("chunked SQLGetData keeps UTF-8 intact\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
  MUST(SQLExecDirect(st, (SQLCHAR*)"SELECT Name FROM Account WHERE Id = '001000000000000005'", SQL_NTS), st,
       SQL_HANDLE_STMT, "execute");
  MUST(SQLFetch(st), st, SQL_HANDLE_STMT, "fetch");

  char joined[256] = {0};
  char piece[8];
  SQLLEN ind;
  SQLRETURN rc;
  while ((rc = SQLGetData(st, 1, SQL_C_CHAR, piece, sizeof piece, &ind)) != SQL_NO_DATA) {
    if (!ok(rc)) break;
    /* every chunk must be complete UTF-8 on its own */
    for (const unsigned char* p = (unsigned char*)piece; *p;) {
      int n = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
      for (int k = 1; k < n; ++k)
        CHECK((p[k] & 0xC0) == 0x80, "split UTF-8 sequence in chunk '%s'", piece);
      p += n;
    }
    strcat(joined, piece);
    if (rc == SQL_SUCCESS) break;
  }
  CHECK(strcmp(joined, "\xe6\x9d\xb1\xe4\xba\xac\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf\xe6\xa0\xaa\xe5\xbc\x8f"
                       "\xe4\xbc\x9a\xe7\xa4\xbe") == 0,
        "reassembled '%s'", joined);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

static void test_describe_col_and_attributes(void) {
  printf("SQLDescribeCol / SQLColAttribute\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
  MUST(SQLExecDirect(st, (SQLCHAR*)"SELECT Id, AnnualRevenue, CreatedDate FROM Account LIMIT 1", SQL_NTS), st,
       SQL_HANDLE_STMT, "execute");

  SQLSMALLINT cols = 0;
  MUST(SQLNumResultCols(st, &cols), st, SQL_HANDLE_STMT, "num result cols");
  CHECK(cols == 3, "expected 3 columns, got %d", cols);

  SQLCHAR name[128];
  SQLSMALLINT name_len, type, digits, nullable;
  SQLULEN size;
  MUST(SQLDescribeCol(st, 2, name, sizeof name, &name_len, &type, &size, &digits, &nullable), st,
       SQL_HANDLE_STMT, "describe col 2");
  CHECK(strcmp((char*)name, "AnnualRevenue") == 0, "column name '%s'", name);
  CHECK(type == SQL_DECIMAL, "type %d, expected SQL_DECIMAL", type);
  CHECK(size == 18, "precision %lu", (unsigned long)size);
  CHECK(digits == 2, "scale %d", digits);
  CHECK(nullable == SQL_NULLABLE, "nullable %d", nullable);

  SQLLEN num = 0;
  MUST(SQLColAttribute(st, 1, SQL_DESC_DISPLAY_SIZE, NULL, 0, NULL, &num), st, SQL_HANDLE_STMT, "display size");
  CHECK(num == 18, "Id display size %ld", (long)num);
  MUST(SQLColAttribute(st, 1, SQL_DESC_OCTET_LENGTH, NULL, 0, NULL, &num), st, SQL_HANDLE_STMT, "octet length");
  CHECK(num == 18 * (SQLLEN)sizeof(SQLWCHAR), "Id octet length %ld", (long)num);
  MUST(SQLColAttribute(st, 1, SQL_DESC_UPDATABLE, NULL, 0, NULL, &num), st, SQL_HANDLE_STMT, "updatable");
  CHECK(num == SQL_ATTR_READONLY, "updatable %ld, expected read-only", (long)num);
  MUST(SQLColAttribute(st, 3, SQL_DESC_TYPE, NULL, 0, NULL, &num), st, SQL_HANDLE_STMT, "desc type");
  CHECK(num == SQL_DATETIME, "timestamp verbose type %ld", (long)num);
  MUST(SQLColAttribute(st, 3, SQL_DESC_CONCISE_TYPE, NULL, 0, NULL, &num), st, SQL_HANDLE_STMT, "concise type");
  CHECK(num == SQL_TYPE_TIMESTAMP, "timestamp concise type %ld", (long)num);

  SQLCHAR text[64];
  SQLSMALLINT text_len = 0;
  MUST(SQLColAttribute(st, 2, SQL_DESC_TYPE_NAME, text, sizeof text, &text_len, NULL), st, SQL_HANDLE_STMT,
       "type name");
  CHECK(strcmp((char*)text, "DECIMAL") == 0, "type name '%s'", text);
  MUST(SQLColAttribute(st, 2, SQL_DESC_BASE_TABLE_NAME, text, sizeof text, &text_len, NULL), st,
       SQL_HANDLE_STMT, "base table");
  CHECK(strcmp((char*)text, "Account") == 0, "base table '%s'", text);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

static void test_numeric_struct(void) {
  printf("SQL_C_NUMERIC\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
  MUST(SQLExecDirect(st,
                     (SQLCHAR*)"SELECT AnnualRevenue FROM Account WHERE Id = '001000000000000003'", SQL_NTS),
       st, SQL_HANDLE_STMT, "execute");
  MUST(SQLFetch(st), st, SQL_HANDLE_STMT, "fetch");

  SQL_NUMERIC_STRUCT ns;
  memset(&ns, 0, sizeof ns);
  SQLLEN ind = 0;
  MUST(SQLGetData(st, 1, SQL_C_NUMERIC, &ns, sizeof ns, &ind), st, SQL_HANDLE_STMT, "getdata numeric");
  /* 375001.5 with scale 2 -> unscaled 37500150 */
  unsigned long long mantissa = 0;
  for (int i = SQL_MAX_NUMERIC_LEN - 1; i >= 0; --i) mantissa = (mantissa << 8) | ns.val[i];
  CHECK(ns.sign == 1, "sign %u", ns.sign);
  CHECK(ns.scale == 2, "scale %d", ns.scale);
  CHECK(mantissa == 37500150ULL, "mantissa %llu, expected 37500150", mantissa);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

static void test_prepare_describe_param(void) {
  printf("SQLPrepare / SQLNumParams / SQLDescribeParam\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
  MUST(SQLPrepare(st,
                  (SQLCHAR*)"SELECT Id FROM Account WHERE Industry = ? AND NumberOfEmployees > ? "
                            "AND CreatedDate > ?",
                  SQL_NTS),
       st, SQL_HANDLE_STMT, "prepare");

  SQLSMALLINT n = 0;
  MUST(SQLNumParams(st, &n), st, SQL_HANDLE_STMT, "num params");
  CHECK(n == 3, "expected 3 parameters, got %d", n);

  SQLSMALLINT type, digits, nullable;
  SQLULEN size;
  MUST(SQLDescribeParam(st, 1, &type, &size, &digits, &nullable), st, SQL_HANDLE_STMT, "describe param 1");
  CHECK(type == SQL_WVARCHAR, "param 1 type %d", type);
  MUST(SQLDescribeParam(st, 2, &type, &size, &digits, &nullable), st, SQL_HANDLE_STMT, "describe param 2");
  CHECK(type == SQL_INTEGER, "param 2 type %d", type);
  MUST(SQLDescribeParam(st, 3, &type, &size, &digits, &nullable), st, SQL_HANDLE_STMT, "describe param 3");
  CHECK(type == SQL_TYPE_TIMESTAMP, "param 3 type %d", type);

  /* bind and run twice with different values */
  SQLCHAR industry[32];
  SQLINTEGER employees;
  SQL_TIMESTAMP_STRUCT ts = {2023, 1, 1, 0, 0, 0, 0};
  SQLLEN ind_text = SQL_NTS, ind_int = 0, ind_ts = 0;
  MUST(SQLBindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR, 255, 0, industry, sizeof industry,
                        &ind_text),
       st, SQL_HANDLE_STMT, "bind param 1");
  MUST(SQLBindParameter(st, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 10, 0, &employees, 0, &ind_int), st,
       SQL_HANDLE_STMT, "bind param 2");
  MUST(SQLBindParameter(st, 3, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 23, 3, &ts,
                        sizeof ts, &ind_ts),
       st, SQL_HANDLE_STMT, "bind param 3");

  strcpy((char*)industry, "Banking");
  employees = 0;
  MUST(SQLExecute(st), st, SQL_HANDLE_STMT, "execute 1");
  SQLLEN first = 0;
  while (ok(SQLFetch(st))) ++first;
  CHECK(first == 4, "Banking rows %ld (one has a NULL employee count)", (long)first);

  MUST(SQLCloseCursor(st), st, SQL_HANDLE_STMT, "close cursor");
  strcpy((char*)industry, "Retail");
  MUST(SQLExecute(st), st, SQL_HANDLE_STMT, "execute 2");
  SQLLEN second = 0;
  while (ok(SQLFetch(st))) ++second;
  CHECK(second == 4, "Retail rows %ld (one has a NULL employee count)", (long)second);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

static void test_getinfo_and_functions(void) {
  printf("SQLGetInfo / SQLGetFunctions\n");
  SQLCHAR buf[64];
  SQLSMALLINT len;
  SQLUSMALLINT u16;
  SQLUINTEGER u32;

  MUST(SQLGetInfo(g_dbc, SQL_OUTER_JOINS, buf, sizeof buf, &len), g_dbc, SQL_HANDLE_DBC, "outer joins");
  CHECK(strcmp((char*)buf, "N") == 0, "SQL_OUTER_JOINS '%s'", buf);
  MUST(SQLGetInfo(g_dbc, SQL_TXN_CAPABLE, &u16, sizeof u16, &len), g_dbc, SQL_HANDLE_DBC, "txn capable");
  CHECK(u16 == SQL_TC_NONE, "txn capable %u", u16);
  MUST(SQLGetInfo(g_dbc, SQL_GETDATA_EXTENSIONS, &u32, sizeof u32, &len), g_dbc, SQL_HANDLE_DBC, "getdata ext");
  CHECK((u32 & SQL_GD_ANY_COLUMN) && (u32 & SQL_GD_ANY_ORDER), "getdata extensions 0x%x", u32);
  MUST(SQLGetInfo(g_dbc, SQL_AGGREGATE_FUNCTIONS, &u32, sizeof u32, &len), g_dbc, SQL_HANDLE_DBC, "aggregates");
  CHECK(u32 == SQL_AF_COUNT, "aggregate functions 0x%x", u32);

  /* truncation must be reported, not silently cut */
  SQLCHAR small[4];
  SQLRETURN rc = SQLGetInfo(g_dbc, SQL_DBMS_NAME, small, sizeof small, &len);
  CHECK(rc == SQL_SUCCESS_WITH_INFO, "truncated GetInfo rc=%d", rc);
  CHECK(len == 10, "reported length %d for 'Salesforce'", len);

  SQLUSMALLINT supported = 0;
  MUST(SQLGetFunctions(g_dbc, SQL_API_SQLFETCHSCROLL, &supported), g_dbc, SQL_HANDLE_DBC, "get functions");
  CHECK(supported == SQL_TRUE, "SQLFetchScroll not reported as supported");
  MUST(SQLGetFunctions(g_dbc, SQL_API_SQLBULKOPERATIONS, &supported), g_dbc, SQL_HANDLE_DBC, "get functions 2");
  CHECK(supported == SQL_FALSE, "SQLBulkOperations should not be reported as supported");
}

static void test_fetch_scroll_forward_only(void) {
  printf("SQLFetchScroll\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
  MUST(SQLExecDirect(st, (SQLCHAR*)"SELECT Id FROM Account ORDER BY Id LIMIT 3", SQL_NTS), st, SQL_HANDLE_STMT,
       "execute");
  SQLRETURN rc = SQLFetchScroll(st, SQL_FETCH_NEXT, 0);
  CHECK(ok(rc), "fetch next rc=%d", rc);
  rc = SQLFetchScroll(st, SQL_FETCH_PRIOR, 0);
  CHECK(rc == SQL_ERROR, "backward scroll should fail on a forward-only cursor (rc=%d)", rc);
  SQLCHAR state[7], msg[512];
  SQLINTEGER native;
  SQLSMALLINT len;
  if (SQLGetDiagRec(SQL_HANDLE_STMT, st, 1, state, &native, msg, sizeof msg, &len) == SQL_SUCCESS)
    CHECK(strcmp((char*)state, "HY106") == 0, "expected HY106, got %s", state);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

static void test_native_sql_shows_soql(void) {
  printf("SQLNativeSql\n");
  SQLCHAR out[512];
  SQLINTEGER len = 0;
  MUST(SQLNativeSql(g_dbc, (SQLCHAR*)"SELECT Id, Name FROM Account WHERE Industry = 'Banking' LIMIT 5",
                    SQL_NTS, out, sizeof out, &len),
       g_dbc, SQL_HANDLE_DBC, "native sql");
  CHECK(strstr((char*)out, "SELECT Id,Name FROM Account") != NULL, "native sql '%s'", out);
  CHECK(strstr((char*)out, "LIMIT 5") != NULL, "native sql missing limit: '%s'", out);
}

static void test_diagnostic_fields(void) {
  printf("SQLGetDiagField\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
  SQLRETURN rc = SQLExecDirect(st, (SQLCHAR*)"SELECT NoSuchColumn FROM Account", SQL_NTS);
  CHECK(rc == SQL_ERROR, "expected failure, rc=%d", rc);

  SQLINTEGER count = 0;
  MUST(SQLGetDiagField(SQL_HANDLE_STMT, st, 0, SQL_DIAG_NUMBER, &count, 0, NULL), st, SQL_HANDLE_STMT,
       "diag number");
  CHECK(count >= 1, "diagnostic record count %d", count);

  SQLCHAR text[128];
  SQLSMALLINT len = 0;
  MUST(SQLGetDiagField(SQL_HANDLE_STMT, st, 1, SQL_DIAG_SQLSTATE, text, sizeof text, &len), st,
       SQL_HANDLE_STMT, "diag sqlstate");
  CHECK(strcmp((char*)text, "42S22") == 0, "sqlstate '%s'", text);
  MUST(SQLGetDiagField(SQL_HANDLE_STMT, st, 1, SQL_DIAG_CLASS_ORIGIN, text, sizeof text, &len), st,
       SQL_HANDLE_STMT, "class origin");
  CHECK(strcmp((char*)text, "ISO 9075") == 0, "class origin '%s'", text);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

static void test_unicode_entry_points(void) {
  printf("Unicode (W) entry points\n");
  SQLHSTMT st;
  SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
  /* SQLWCHAR literal built by hand so the test does not depend on wchar_t width */
  const char* sql = "SELECT Name FROM Account WHERE Id = '001000000000000002'";
  SQLWCHAR wsql[128];
  size_t i = 0;
  for (; sql[i]; ++i) wsql[i] = (SQLWCHAR)sql[i];
  wsql[i] = 0;

  MUST(SQLExecDirectW(st, wsql, SQL_NTS), st, SQL_HANDLE_STMT, "execute W");
  MUST(SQLFetch(st), st, SQL_HANDLE_STMT, "fetch");

  SQLWCHAR name[128];
  SQLLEN ind = 0;
  MUST(SQLGetData(st, 1, SQL_C_WCHAR, name, sizeof name, &ind), st, SQL_HANDLE_STMT, "getdata W");
  CHECK(ind == 15 * (SQLLEN)sizeof(SQLWCHAR), "wide length %ld for 'Café Münch GmbH'", (long)ind);
  CHECK(name[3] == 0x00E9, "expected an e-acute at index 3, got U+%04X", (unsigned)name[3]);

  SQLWCHAR wname[128];
  SQLSMALLINT wlen = 0, type, digits, nullable;
  SQLULEN size;
  MUST(SQLDescribeColW(st, 1, wname, 128, &wlen, &type, &size, &digits, &nullable), st, SQL_HANDLE_STMT,
       "describe col W");
  CHECK(wlen == 4 && wname[0] == 'N', "wide column name length %d", wlen);
  SQLFreeHandle(SQL_HANDLE_STMT, st);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s \"<connection string>\"\n", argv[0]);
    return 2;
  }
  SQLHENV env;
  SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
  SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
  SQLAllocHandle(SQL_HANDLE_DBC, env, &g_dbc);

  SQLCHAR out[1024];
  SQLSMALLINT out_len = 0;
  SQLRETURN rc = SQLDriverConnect(g_dbc, NULL, (SQLCHAR*)argv[1], SQL_NTS, out, sizeof out, &out_len,
                                  SQL_DRIVER_NOPROMPT);
  if (!ok(rc)) {
    printf("connect failed (rc=%d)\n", rc);
    show_diag(SQL_HANDLE_DBC, g_dbc);
    return 1;
  }

  test_block_fetch_column_wise();
  test_block_fetch_row_wise();
  test_getdata_chunks();
  test_getdata_never_splits_utf8();
  test_describe_col_and_attributes();
  test_numeric_struct();
  test_prepare_describe_param();
  test_getinfo_and_functions();
  test_fetch_scroll_forward_only();
  test_native_sql_shows_soql();
  test_diagnostic_fields();
  test_unicode_entry_points();

  SQLDisconnect(g_dbc);
  SQLFreeHandle(SQL_HANDLE_DBC, g_dbc);
  SQLFreeHandle(SQL_HANDLE_ENV, env);

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
