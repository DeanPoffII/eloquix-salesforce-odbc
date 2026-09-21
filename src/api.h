#pragma once
// Internal implementations of the ODBC API. Exports (odbc_exports.cpp) convert string
// arguments (UTF-8 for ANSI, UTF-16/32 for W) and pass `wide` so output strings are written
// in the caller's encoding.
#include "catalog.h"

namespace sf::api {

SQLRETURN AllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE* out);
SQLRETURN FreeHandle(SQLSMALLINT type, SQLHANDLE h);
SQLRETURN SetEnvAttr(SQLHENV h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER len);
SQLRETURN GetEnvAttr(SQLHENV h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER cap, SQLINTEGER* len);

SQLRETURN Connect(SQLHDBC h, const std::string& dsn, const OptStr& uid, const OptStr& pwd);
SQLRETURN DriverConnect(SQLHDBC h, const std::string& in, bool wide, SQLPOINTER out, SQLSMALLINT cap,
                        SQLSMALLINT* out_len, SQLUSMALLINT completion);
SQLRETURN Disconnect(SQLHDBC h);
SQLRETURN SetConnectAttr(SQLHDBC h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER len, bool wide);
SQLRETURN GetConnectAttr(SQLHDBC h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER cap, SQLINTEGER* len, bool wide);
SQLRETURN GetInfo(SQLHDBC h, SQLUSMALLINT type, SQLPOINTER value, SQLSMALLINT cap, SQLSMALLINT* len, bool wide);
SQLRETURN GetFunctions(SQLHDBC h, SQLUSMALLINT id, SQLUSMALLINT* supported);
SQLRETURN NativeSql(SQLHDBC h, const std::string& in, bool wide, SQLPOINTER out, SQLINTEGER cap, SQLINTEGER* len);
SQLRETURN EndTran(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT completion);

SQLRETURN SetStmtAttr(SQLHSTMT h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER len);
SQLRETURN GetStmtAttr(SQLHSTMT h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER cap, SQLINTEGER* len);
SQLRETURN Prepare(SQLHSTMT h, const std::string& sql);
SQLRETURN Execute(SQLHSTMT h);
SQLRETURN ExecDirect(SQLHSTMT h, const std::string& sql);
SQLRETURN NumResultCols(SQLHSTMT h, SQLSMALLINT* n);
SQLRETURN DescribeCol(SQLHSTMT h, SQLUSMALLINT col, bool wide, SQLPOINTER name, SQLSMALLINT cap,
                      SQLSMALLINT* name_len, SQLSMALLINT* type, SQLULEN* size, SQLSMALLINT* digits,
                      SQLSMALLINT* nullable);
SQLRETURN ColAttribute(SQLHSTMT h, SQLUSMALLINT col, SQLUSMALLINT field, bool wide, SQLPOINTER char_attr,
                       SQLSMALLINT cap, SQLSMALLINT* len, SQLLEN* num_attr);
SQLRETURN BindCol(SQLHSTMT h, SQLUSMALLINT col, SQLSMALLINT c_type, SQLPOINTER target, SQLLEN cap, SQLLEN* ind);
SQLRETURN Fetch(SQLHSTMT h);
SQLRETURN FetchScroll(SQLHSTMT h, SQLSMALLINT orientation, SQLLEN offset);
SQLRETURN GetData(SQLHSTMT h, SQLUSMALLINT col, SQLSMALLINT c_type, SQLPOINTER target, SQLLEN cap, SQLLEN* ind);
SQLRETURN RowCount(SQLHSTMT h, SQLLEN* n);
SQLRETURN MoreResults(SQLHSTMT h);
SQLRETURN CloseCursor(SQLHSTMT h);
SQLRETURN FreeStmt(SQLHSTMT h, SQLUSMALLINT option);
SQLRETURN Cancel(SQLHSTMT h);
SQLRETURN BindParameter(SQLHSTMT h, SQLUSMALLINT num, SQLSMALLINT io, SQLSMALLINT c_type, SQLSMALLINT sql_type,
                        SQLULEN size, SQLSMALLINT digits, SQLPOINTER value, SQLLEN cap, SQLLEN* ind);
SQLRETURN NumParams(SQLHSTMT h, SQLSMALLINT* n);
SQLRETURN DescribeParam(SQLHSTMT h, SQLUSMALLINT num, SQLSMALLINT* type, SQLULEN* size, SQLSMALLINT* digits,
                        SQLSMALLINT* nullable);
SQLRETURN GetCursorName(SQLHSTMT h, bool wide, SQLPOINTER out, SQLSMALLINT cap, SQLSMALLINT* len);
SQLRETURN SetCursorName(SQLHSTMT h, const std::string& name);

SQLRETURN Tables(SQLHSTMT h, const OptStr& cat, const OptStr& schema, const OptStr& table, const OptStr& types);
SQLRETURN Columns(SQLHSTMT h, const OptStr& cat, const OptStr& schema, const OptStr& table, const OptStr& column);
SQLRETURN PrimaryKeys(SQLHSTMT h, const OptStr& cat, const OptStr& schema, const OptStr& table);
SQLRETURN ForeignKeys(SQLHSTMT h, const OptStr& pk_table, const OptStr& fk_table);
SQLRETURN Statistics(SQLHSTMT h, const OptStr& table);
SQLRETURN SpecialColumns(SQLHSTMT h, SQLUSMALLINT id_type, const OptStr& table);
SQLRETURN Procedures(SQLHSTMT h);
SQLRETURN ProcedureColumns(SQLHSTMT h);
SQLRETURN GetTypeInfo(SQLHSTMT h, SQLSMALLINT type);

SQLRETURN GetDiagRec(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT rec, bool wide, SQLPOINTER state,
                     SQLINTEGER* native, SQLPOINTER msg, SQLSMALLINT cap, SQLSMALLINT* msg_len);
SQLRETURN GetDiagField(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT rec, SQLSMALLINT field, bool wide,
                       SQLPOINTER value, SQLSMALLINT cap, SQLSMALLINT* len);

}  // namespace sf::api
