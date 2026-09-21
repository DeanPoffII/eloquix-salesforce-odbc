// Exported ODBC 3.8 entry points. ANSI functions take UTF-8; W functions take SQLWCHAR
// (UTF-16 on Windows/unixODBC, UTF-32 on iODBC). Both route into sf::api.
#include "api.h"

using namespace sf;

namespace {
OptStr optA(SQLCHAR* s, SQLSMALLINT len) {
  if (!s) return std::nullopt;
  return narrow_in(s, len);
}
OptStr optW(SQLWCHAR* s, SQLSMALLINT len) {
  if (!s) return std::nullopt;
  return wide_in(s, len);
}
}  // namespace

extern "C" {

// ---------------------------------------------------------------- handles & env

SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT t, SQLHANDLE in, SQLHANDLE* out) { return api::AllocHandle(t, in, out); }
SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT t, SQLHANDLE h) { return api::FreeHandle(t, h); }
SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT h, SQLUSMALLINT opt) { return api::FreeStmt(h, opt); }
SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER l) { return api::SetEnvAttr(h, a, v, l); }
SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER c, SQLINTEGER* l) {
  return api::GetEnvAttr(h, a, v, c, l);
}
SQLRETURN SQL_API SQLEndTran(SQLSMALLINT t, SQLHANDLE h, SQLSMALLINT c) { return api::EndTran(t, h, c); }

// ---------------------------------------------------------------- connection

SQLRETURN SQL_API SQLConnect(SQLHDBC h, SQLCHAR* dsn, SQLSMALLINT dl, SQLCHAR* uid, SQLSMALLINT ul, SQLCHAR* pwd,
                             SQLSMALLINT pl) {
  return api::Connect(h, narrow_in(dsn, dl), optA(uid, ul), optA(pwd, pl));
}
SQLRETURN SQL_API SQLConnectW(SQLHDBC h, SQLWCHAR* dsn, SQLSMALLINT dl, SQLWCHAR* uid, SQLSMALLINT ul, SQLWCHAR* pwd,
                              SQLSMALLINT pl) {
  return api::Connect(h, wide_in(dsn, dl), optW(uid, ul), optW(pwd, pl));
}

SQLRETURN SQL_API SQLDriverConnect(SQLHDBC h, SQLHWND, SQLCHAR* in, SQLSMALLINT il, SQLCHAR* out, SQLSMALLINT cap,
                                   SQLSMALLINT* ol, SQLUSMALLINT completion) {
  return api::DriverConnect(h, narrow_in(in, il), false, out, cap, ol, completion);
}
SQLRETURN SQL_API SQLDriverConnectW(SQLHDBC h, SQLHWND, SQLWCHAR* in, SQLSMALLINT il, SQLWCHAR* out, SQLSMALLINT cap,
                                    SQLSMALLINT* ol, SQLUSMALLINT completion) {
  return api::DriverConnect(h, wide_in(in, il), true, out, cap, ol, completion);
}

SQLRETURN SQL_API SQLDisconnect(SQLHDBC h) { return api::Disconnect(h); }

SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER l) {
  return api::SetConnectAttr(h, a, v, l, false);
}
SQLRETURN SQL_API SQLSetConnectAttrW(SQLHDBC h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER l) {
  return api::SetConnectAttr(h, a, v, l, true);
}
SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER c, SQLINTEGER* l) {
  return api::GetConnectAttr(h, a, v, c, l, false);
}
SQLRETURN SQL_API SQLGetConnectAttrW(SQLHDBC h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER c, SQLINTEGER* l) {
  return api::GetConnectAttr(h, a, v, c, l, true);
}

SQLRETURN SQL_API SQLGetInfo(SQLHDBC h, SQLUSMALLINT t, SQLPOINTER v, SQLSMALLINT c, SQLSMALLINT* l) {
  return api::GetInfo(h, t, v, c, l, false);
}
SQLRETURN SQL_API SQLGetInfoW(SQLHDBC h, SQLUSMALLINT t, SQLPOINTER v, SQLSMALLINT c, SQLSMALLINT* l) {
  return api::GetInfo(h, t, v, c, l, true);
}
SQLRETURN SQL_API SQLGetFunctions(SQLHDBC h, SQLUSMALLINT id, SQLUSMALLINT* s) { return api::GetFunctions(h, id, s); }

SQLRETURN SQL_API SQLNativeSql(SQLHDBC h, SQLCHAR* in, SQLINTEGER il, SQLCHAR* out, SQLINTEGER cap, SQLINTEGER* ol) {
  return api::NativeSql(h, narrow_in(in, il), false, out, cap, ol);
}
SQLRETURN SQL_API SQLNativeSqlW(SQLHDBC h, SQLWCHAR* in, SQLINTEGER il, SQLWCHAR* out, SQLINTEGER cap,
                                SQLINTEGER* ol) {
  return api::NativeSql(h, wide_in(in, il), true, out, cap, ol);
}

// ---------------------------------------------------------------- statements

SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER l) { return api::SetStmtAttr(h, a, v, l); }
SQLRETURN SQL_API SQLSetStmtAttrW(SQLHSTMT h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER l) { return api::SetStmtAttr(h, a, v, l); }
SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER c, SQLINTEGER* l) {
  return api::GetStmtAttr(h, a, v, c, l);
}
SQLRETURN SQL_API SQLGetStmtAttrW(SQLHSTMT h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER c, SQLINTEGER* l) {
  return api::GetStmtAttr(h, a, v, c, l);
}

SQLRETURN SQL_API SQLPrepare(SQLHSTMT h, SQLCHAR* sql, SQLINTEGER len) { return api::Prepare(h, narrow_in(sql, len)); }
SQLRETURN SQL_API SQLPrepareW(SQLHSTMT h, SQLWCHAR* sql, SQLINTEGER len) { return api::Prepare(h, wide_in(sql, len)); }
SQLRETURN SQL_API SQLExecute(SQLHSTMT h) { return api::Execute(h); }
SQLRETURN SQL_API SQLExecDirect(SQLHSTMT h, SQLCHAR* sql, SQLINTEGER len) {
  return api::ExecDirect(h, narrow_in(sql, len));
}
SQLRETURN SQL_API SQLExecDirectW(SQLHSTMT h, SQLWCHAR* sql, SQLINTEGER len) {
  return api::ExecDirect(h, wide_in(sql, len));
}

SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT h, SQLSMALLINT* n) { return api::NumResultCols(h, n); }

SQLRETURN SQL_API SQLDescribeCol(SQLHSTMT h, SQLUSMALLINT col, SQLCHAR* name, SQLSMALLINT cap, SQLSMALLINT* nl,
                                 SQLSMALLINT* type, SQLULEN* size, SQLSMALLINT* digits, SQLSMALLINT* nullable) {
  return api::DescribeCol(h, col, false, name, cap, nl, type, size, digits, nullable);
}
SQLRETURN SQL_API SQLDescribeColW(SQLHSTMT h, SQLUSMALLINT col, SQLWCHAR* name, SQLSMALLINT cap, SQLSMALLINT* nl,
                                  SQLSMALLINT* type, SQLULEN* size, SQLSMALLINT* digits, SQLSMALLINT* nullable) {
  return api::DescribeCol(h, col, true, name, cap, nl, type, size, digits, nullable);
}

SQLRETURN SQL_API SQLColAttribute(SQLHSTMT h, SQLUSMALLINT col, SQLUSMALLINT field, SQLPOINTER ca, SQLSMALLINT cap,
                                  SQLSMALLINT* len, SQLLEN* num) {
  return api::ColAttribute(h, col, field, false, ca, cap, len, num);
}
SQLRETURN SQL_API SQLColAttributeW(SQLHSTMT h, SQLUSMALLINT col, SQLUSMALLINT field, SQLPOINTER ca, SQLSMALLINT cap,
                                   SQLSMALLINT* len, SQLLEN* num) {
  return api::ColAttribute(h, col, field, true, ca, cap, len, num);
}

SQLRETURN SQL_API SQLBindCol(SQLHSTMT h, SQLUSMALLINT col, SQLSMALLINT t, SQLPOINTER v, SQLLEN c, SQLLEN* ind) {
  return api::BindCol(h, col, t, v, c, ind);
}
SQLRETURN SQL_API SQLFetch(SQLHSTMT h) { return api::Fetch(h); }
SQLRETURN SQL_API SQLFetchScroll(SQLHSTMT h, SQLSMALLINT o, SQLLEN off) { return api::FetchScroll(h, o, off); }
SQLRETURN SQL_API SQLGetData(SQLHSTMT h, SQLUSMALLINT col, SQLSMALLINT t, SQLPOINTER v, SQLLEN c, SQLLEN* ind) {
  return api::GetData(h, col, t, v, c, ind);
}
SQLRETURN SQL_API SQLRowCount(SQLHSTMT h, SQLLEN* n) { return api::RowCount(h, n); }
SQLRETURN SQL_API SQLMoreResults(SQLHSTMT h) { return api::MoreResults(h); }
SQLRETURN SQL_API SQLCloseCursor(SQLHSTMT h) { return api::CloseCursor(h); }
SQLRETURN SQL_API SQLCancel(SQLHSTMT h) { return api::Cancel(h); }

SQLRETURN SQL_API SQLBindParameter(SQLHSTMT h, SQLUSMALLINT n, SQLSMALLINT io, SQLSMALLINT ct, SQLSMALLINT st,
                                   SQLULEN size, SQLSMALLINT digits, SQLPOINTER v, SQLLEN cap, SQLLEN* ind) {
  return api::BindParameter(h, n, io, ct, st, size, digits, v, cap, ind);
}
SQLRETURN SQL_API SQLNumParams(SQLHSTMT h, SQLSMALLINT* n) { return api::NumParams(h, n); }
SQLRETURN SQL_API SQLDescribeParam(SQLHSTMT h, SQLUSMALLINT n, SQLSMALLINT* t, SQLULEN* s, SQLSMALLINT* d,
                                   SQLSMALLINT* nl) {
  return api::DescribeParam(h, n, t, s, d, nl);
}

SQLRETURN SQL_API SQLGetCursorName(SQLHSTMT h, SQLCHAR* out, SQLSMALLINT cap, SQLSMALLINT* len) {
  return api::GetCursorName(h, false, out, cap, len);
}
SQLRETURN SQL_API SQLGetCursorNameW(SQLHSTMT h, SQLWCHAR* out, SQLSMALLINT cap, SQLSMALLINT* len) {
  return api::GetCursorName(h, true, out, cap, len);
}
SQLRETURN SQL_API SQLSetCursorName(SQLHSTMT h, SQLCHAR* name, SQLSMALLINT len) {
  return api::SetCursorName(h, narrow_in(name, len));
}
SQLRETURN SQL_API SQLSetCursorNameW(SQLHSTMT h, SQLWCHAR* name, SQLSMALLINT len) {
  return api::SetCursorName(h, wide_in(name, len));
}

// ---------------------------------------------------------------- catalog

SQLRETURN SQL_API SQLTables(SQLHSTMT h, SQLCHAR* c, SQLSMALLINT cl, SQLCHAR* s, SQLSMALLINT sl, SQLCHAR* t,
                            SQLSMALLINT tl, SQLCHAR* ty, SQLSMALLINT tyl) {
  return api::Tables(h, optA(c, cl), optA(s, sl), optA(t, tl), optA(ty, tyl));
}
SQLRETURN SQL_API SQLTablesW(SQLHSTMT h, SQLWCHAR* c, SQLSMALLINT cl, SQLWCHAR* s, SQLSMALLINT sl, SQLWCHAR* t,
                             SQLSMALLINT tl, SQLWCHAR* ty, SQLSMALLINT tyl) {
  return api::Tables(h, optW(c, cl), optW(s, sl), optW(t, tl), optW(ty, tyl));
}

SQLRETURN SQL_API SQLColumns(SQLHSTMT h, SQLCHAR* c, SQLSMALLINT cl, SQLCHAR* s, SQLSMALLINT sl, SQLCHAR* t,
                             SQLSMALLINT tl, SQLCHAR* col, SQLSMALLINT coll) {
  return api::Columns(h, optA(c, cl), optA(s, sl), optA(t, tl), optA(col, coll));
}
SQLRETURN SQL_API SQLColumnsW(SQLHSTMT h, SQLWCHAR* c, SQLSMALLINT cl, SQLWCHAR* s, SQLSMALLINT sl, SQLWCHAR* t,
                              SQLSMALLINT tl, SQLWCHAR* col, SQLSMALLINT coll) {
  return api::Columns(h, optW(c, cl), optW(s, sl), optW(t, tl), optW(col, coll));
}

SQLRETURN SQL_API SQLPrimaryKeys(SQLHSTMT h, SQLCHAR* c, SQLSMALLINT cl, SQLCHAR* s, SQLSMALLINT sl, SQLCHAR* t,
                                 SQLSMALLINT tl) {
  return api::PrimaryKeys(h, optA(c, cl), optA(s, sl), optA(t, tl));
}
SQLRETURN SQL_API SQLPrimaryKeysW(SQLHSTMT h, SQLWCHAR* c, SQLSMALLINT cl, SQLWCHAR* s, SQLSMALLINT sl, SQLWCHAR* t,
                                  SQLSMALLINT tl) {
  return api::PrimaryKeys(h, optW(c, cl), optW(s, sl), optW(t, tl));
}

SQLRETURN SQL_API SQLForeignKeys(SQLHSTMT h, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR* pt,
                                 SQLSMALLINT ptl, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR* ft,
                                 SQLSMALLINT ftl) {
  return api::ForeignKeys(h, optA(pt, ptl), optA(ft, ftl));
}
SQLRETURN SQL_API SQLForeignKeysW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR* pt,
                                  SQLSMALLINT ptl, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR* ft,
                                  SQLSMALLINT ftl) {
  return api::ForeignKeys(h, optW(pt, ptl), optW(ft, ftl));
}

SQLRETURN SQL_API SQLStatistics(SQLHSTMT h, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR* t, SQLSMALLINT tl,
                                SQLUSMALLINT, SQLUSMALLINT) {
  return api::Statistics(h, optA(t, tl));
}
SQLRETURN SQL_API SQLStatisticsW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR* t,
                                 SQLSMALLINT tl, SQLUSMALLINT, SQLUSMALLINT) {
  return api::Statistics(h, optW(t, tl));
}

SQLRETURN SQL_API SQLSpecialColumns(SQLHSTMT h, SQLUSMALLINT id, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT,
                                    SQLCHAR* t, SQLSMALLINT tl, SQLUSMALLINT, SQLUSMALLINT) {
  return api::SpecialColumns(h, id, optA(t, tl));
}
SQLRETURN SQL_API SQLSpecialColumnsW(SQLHSTMT h, SQLUSMALLINT id, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT,
                                     SQLWCHAR* t, SQLSMALLINT tl, SQLUSMALLINT, SQLUSMALLINT) {
  return api::SpecialColumns(h, id, optW(t, tl));
}

SQLRETURN SQL_API SQLProcedures(SQLHSTMT h, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT) {
  return api::Procedures(h);
}
SQLRETURN SQL_API SQLProceduresW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT) {
  return api::Procedures(h);
}
SQLRETURN SQL_API SQLProcedureColumns(SQLHSTMT h, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT,
                                      SQLCHAR*, SQLSMALLINT) {
  return api::ProcedureColumns(h);
}
SQLRETURN SQL_API SQLProcedureColumnsW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*,
                                       SQLSMALLINT, SQLWCHAR*, SQLSMALLINT) {
  return api::ProcedureColumns(h);
}

SQLRETURN SQL_API SQLGetTypeInfo(SQLHSTMT h, SQLSMALLINT t) { return api::GetTypeInfo(h, t); }
SQLRETURN SQL_API SQLGetTypeInfoW(SQLHSTMT h, SQLSMALLINT t) { return api::GetTypeInfo(h, t); }

// ---------------------------------------------------------------- diagnostics

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT t, SQLHANDLE h, SQLSMALLINT r, SQLCHAR* st, SQLINTEGER* n, SQLCHAR* m,
                                SQLSMALLINT cap, SQLSMALLINT* ml) {
  return api::GetDiagRec(t, h, r, false, st, n, m, cap, ml);
}
SQLRETURN SQL_API SQLGetDiagRecW(SQLSMALLINT t, SQLHANDLE h, SQLSMALLINT r, SQLWCHAR* st, SQLINTEGER* n, SQLWCHAR* m,
                                 SQLSMALLINT cap, SQLSMALLINT* ml) {
  return api::GetDiagRec(t, h, r, true, st, n, m, cap, ml);
}
SQLRETURN SQL_API SQLGetDiagField(SQLSMALLINT t, SQLHANDLE h, SQLSMALLINT r, SQLSMALLINT f, SQLPOINTER v,
                                  SQLSMALLINT cap, SQLSMALLINT* l) {
  return api::GetDiagField(t, h, r, f, false, v, cap, l);
}
SQLRETURN SQL_API SQLGetDiagFieldW(SQLSMALLINT t, SQLHANDLE h, SQLSMALLINT r, SQLSMALLINT f, SQLPOINTER v,
                                   SQLSMALLINT cap, SQLSMALLINT* l) {
  return api::GetDiagField(t, h, r, f, true, v, cap, l);
}

}  // extern "C"
