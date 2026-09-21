// Capability reporting. BI tools generate SQL based on these answers, so they are deliberately
// conservative: claim only what the SOQL translator really supports.
#include "catalog.h"

namespace sf {

static InfoValue str(const std::string& s) { InfoValue v; v.kind = InfoValue::Str; v.s = s; return v; }
static InfoValue u16(uint32_t n) { InfoValue v; v.kind = InfoValue::U16; v.n = n; return v; }
static InfoValue u32(uint32_t n) { InfoValue v; v.kind = InfoValue::U32; v.n = n; return v; }

InfoValue get_info(Conn& c, SQLUSMALLINT t) {
  switch (t) {
    // ---- driver & data source
    case SQL_DRIVER_NAME: return str("libsfodbc");
    case SQL_DRIVER_VER: return str(SFODBC_DRIVER_VERSION);
    case SQL_DRIVER_ODBC_VER: return str("03.80");
    case SQL_ODBC_VER: return str("03.80.0000");
    case SQL_DBMS_NAME: return str("Salesforce");
    case SQL_DBMS_VER: return str(c.client ? c.client->api_version() : SFODBC_DEFAULT_API);
    case SQL_DATA_SOURCE_NAME: return str(c.dsn);
    case SQL_SERVER_NAME: return str(c.client ? c.client->instance_url() : "");
    case SQL_DATABASE_NAME: return str("");
    case SQL_USER_NAME: return str(c.client ? c.client->username() : "");
    case SQL_DATA_SOURCE_READ_ONLY: return str("Y");
    case SQL_ACCESSIBLE_TABLES: return str("Y");
    case SQL_ACCESSIBLE_PROCEDURES: return str("N");
    case SQL_XOPEN_CLI_YEAR: return str("1995");
    case SQL_ODBC_INTERFACE_CONFORMANCE: return u32(SQL_OIC_CORE);
    case SQL_ODBC_API_CONFORMANCE: return u16(SQL_OAC_LEVEL1);
    case SQL_ODBC_SQL_CONFORMANCE: return u16(SQL_OSC_MINIMUM);
    case SQL_ODBC_SAG_CLI_CONFORMANCE: return u16(SQL_OSCC_COMPLIANT);
    case SQL_SQL_CONFORMANCE: return u32(SQL_SC_SQL92_ENTRY);
    case SQL_STANDARD_CLI_CONFORMANCE: return u32(0);
    case SQL_ACTIVE_ENVIRONMENTS: return u16(0);
    case SQL_MAX_DRIVER_CONNECTIONS: return u16(0);
    case SQL_MAX_CONCURRENT_ACTIVITIES: return u16(0);
    case SQL_ASYNC_MODE: return u32(SQL_AM_NONE);
    case SQL_MAX_ASYNC_CONCURRENT_STATEMENTS: return u32(0);
#ifdef SQL_ASYNC_DBC_FUNCTIONS
    case SQL_ASYNC_DBC_FUNCTIONS: return u32(SQL_ASYNC_DBC_NOT_CAPABLE);
#endif
#ifdef SQL_DRIVER_AWARE_POOLING_SUPPORTED
    case SQL_DRIVER_AWARE_POOLING_SUPPORTED: return u32(SQL_DRIVER_AWARE_POOLING_NOT_CAPABLE);
#endif
#ifdef SQL_ASYNC_NOTIFICATION
    case SQL_ASYNC_NOTIFICATION: return u32(SQL_ASYNC_NOTIFICATION_NOT_CAPABLE);
#endif
    case SQL_FILE_USAGE: return u16(SQL_FILE_NOT_SUPPORTED);
    case SQL_GETDATA_EXTENSIONS: return u32(SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND);
    case SQL_DTC_TRANSITION_COST: return u32(0);

    // ---- naming
    case SQL_IDENTIFIER_QUOTE_CHAR: return str("\"");
    case SQL_IDENTIFIER_CASE: return u16(SQL_IC_MIXED);
    case SQL_QUOTED_IDENTIFIER_CASE: return u16(SQL_IC_MIXED);
    case SQL_CATALOG_NAME: return str("N");
    case SQL_CATALOG_NAME_SEPARATOR: return str(".");
    case SQL_CATALOG_TERM: return str("");
    case SQL_CATALOG_LOCATION: return u16(0);
    case SQL_CATALOG_USAGE: return u32(0);
    case SQL_SCHEMA_TERM: return str("");
    case SQL_SCHEMA_USAGE: return u32(0);
    case SQL_TABLE_TERM: return str("object");
    case SQL_PROCEDURE_TERM: return str("");
    case SQL_SEARCH_PATTERN_ESCAPE: return str("\\");
    case SQL_SPECIAL_CHARACTERS: return str("");
    case SQL_KEYWORDS: return str("");
    case SQL_LIKE_ESCAPE_CLAUSE: return str("N");
    case SQL_MAX_CATALOG_NAME_LEN: return u16(0);
    case SQL_MAX_SCHEMA_NAME_LEN: return u16(0);
    case SQL_MAX_TABLE_NAME_LEN: return u16(255);
    case SQL_MAX_COLUMN_NAME_LEN: return u16(255);
    case SQL_MAX_CURSOR_NAME_LEN: return u16(128);
    case SQL_MAX_IDENTIFIER_LEN: return u16(255);
    case SQL_MAX_USER_NAME_LEN: return u16(255);
    case SQL_MAX_PROCEDURE_NAME_LEN: return u16(0);

    // ---- transactions & cursors
    case SQL_TXN_CAPABLE: return u16(SQL_TC_NONE);
    case SQL_DEFAULT_TXN_ISOLATION: return u32(0);
    case SQL_TXN_ISOLATION_OPTION: return u32(0);
    case SQL_MULTIPLE_ACTIVE_TXN: return str("Y");
    case SQL_CURSOR_COMMIT_BEHAVIOR: return u16(SQL_CB_PRESERVE);
    case SQL_CURSOR_ROLLBACK_BEHAVIOR: return u16(SQL_CB_PRESERVE);
    case SQL_CURSOR_SENSITIVITY: return u32(SQL_INSENSITIVE);
    case SQL_SCROLL_OPTIONS: return u32(SQL_SO_FORWARD_ONLY);
    case SQL_SCROLL_CONCURRENCY: return u32(SQL_SCCO_READ_ONLY);
    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1: return u32(SQL_CA1_NEXT);
    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2: return u32(SQL_CA2_READ_ONLY_CONCURRENCY | SQL_CA2_MAX_ROWS_SELECT);
    case SQL_STATIC_CURSOR_ATTRIBUTES1:
    case SQL_STATIC_CURSOR_ATTRIBUTES2:
    case SQL_KEYSET_CURSOR_ATTRIBUTES1:
    case SQL_KEYSET_CURSOR_ATTRIBUTES2:
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES1:
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES2: return u32(0);
    case SQL_POS_OPERATIONS: return u32(0);
    case SQL_POSITIONED_STATEMENTS: return u32(0);
    case SQL_LOCK_TYPES: return u32(0);
    case SQL_STATIC_SENSITIVITY: return u32(0);
    case SQL_BOOKMARK_PERSISTENCE: return u32(0);
    case SQL_ROW_UPDATES: return str("N");
    case SQL_FETCH_DIRECTION: return u32(SQL_FD_FETCH_NEXT);
    case SQL_MULT_RESULT_SETS: return str("N");
    case SQL_BATCH_SUPPORT: return u32(0);
    case SQL_BATCH_ROW_COUNT: return u32(0);
    case SQL_PARAM_ARRAY_ROW_COUNTS: return u32(SQL_PARC_NO_BATCH);
    case SQL_PARAM_ARRAY_SELECTS: return u32(SQL_PAS_NO_SELECT);
    case SQL_DESCRIBE_PARAMETER: return str("Y");
    case SQL_NEED_LONG_DATA_LEN: return str("N");

    // ---- SQL capabilities (what the SOQL translator handles)
    case SQL_MAX_STATEMENT_LEN: return u32(100000);
    case SQL_MAX_ROW_SIZE: return u32(0);
    case SQL_MAX_ROW_SIZE_INCLUDES_LONG: return str("Y");
    case SQL_MAX_COLUMNS_IN_SELECT: return u16(0);
    case SQL_MAX_COLUMNS_IN_ORDER_BY: return u16(32);
    case SQL_MAX_COLUMNS_IN_GROUP_BY: return u16(0);
    case SQL_MAX_COLUMNS_IN_INDEX: return u16(0);
    case SQL_MAX_COLUMNS_IN_TABLE: return u16(0);
    case SQL_MAX_TABLES_IN_SELECT: return u16(1);
    case SQL_MAX_INDEX_SIZE: return u32(0);
    case SQL_MAX_BINARY_LITERAL_LEN: return u32(0);
    case SQL_MAX_CHAR_LITERAL_LEN: return u32(0);
    case SQL_COLUMN_ALIAS: return str("Y");
    case SQL_CORRELATION_NAME: return u16(SQL_CN_ANY);
    case SQL_EXPRESSIONS_IN_ORDERBY: return str("N");
    case SQL_ORDER_BY_COLUMNS_IN_SELECT: return str("N");
    case SQL_GROUP_BY: return u16(SQL_GB_NOT_SUPPORTED);
    case SQL_OUTER_JOINS: return str("N");
    case SQL_OJ_CAPABILITIES: return u32(0);
    case SQL_SQL92_RELATIONAL_JOIN_OPERATORS: return u32(0);
    case SQL_SUBQUERIES: return u32(0);
    case SQL_UNION: return u32(0);
    case SQL_NULL_COLLATION: return u16(SQL_NC_LOW);
    case SQL_CONCAT_NULL_BEHAVIOR: return u16(SQL_CB_NULL);
    case SQL_NON_NULLABLE_COLUMNS: return u16(SQL_NNC_NULL);
    case SQL_INTEGRITY: return str("N");
    case SQL_PROCEDURES: return str("N");
    case SQL_AGGREGATE_FUNCTIONS: return u32(SQL_AF_COUNT);
    case SQL_SQL92_PREDICATES:
      return u32(SQL_SP_COMPARISON | SQL_SP_IN | SQL_SP_ISNULL | SQL_SP_ISNOTNULL | SQL_SP_LIKE | SQL_SP_BETWEEN);
    case SQL_SQL92_VALUE_EXPRESSIONS: return u32(0);
    case SQL_SQL92_ROW_VALUE_CONSTRUCTOR: return u32(0);
    case SQL_SQL92_STRING_FUNCTIONS:
    case SQL_SQL92_NUMERIC_VALUE_FUNCTIONS:
    case SQL_SQL92_DATETIME_FUNCTIONS:
    case SQL_SQL92_GRANT:
    case SQL_SQL92_REVOKE:
    case SQL_SQL92_FOREIGN_KEY_DELETE_RULE:
    case SQL_SQL92_FOREIGN_KEY_UPDATE_RULE: return u32(0);
    case SQL_STRING_FUNCTIONS:
    case SQL_NUMERIC_FUNCTIONS:
    case SQL_TIMEDATE_FUNCTIONS:
    case SQL_SYSTEM_FUNCTIONS:
    case SQL_CONVERT_FUNCTIONS:
    case SQL_TIMEDATE_ADD_INTERVALS:
    case SQL_TIMEDATE_DIFF_INTERVALS: return u32(0);
    case SQL_DATETIME_LITERALS: return u32(SQL_DL_SQL92_DATE | SQL_DL_SQL92_TIME | SQL_DL_SQL92_TIMESTAMP);
    case SQL_CONVERT_BIGINT: case SQL_CONVERT_BINARY: case SQL_CONVERT_BIT: case SQL_CONVERT_CHAR:
    case SQL_CONVERT_DATE: case SQL_CONVERT_DECIMAL: case SQL_CONVERT_DOUBLE: case SQL_CONVERT_FLOAT:
    case SQL_CONVERT_INTEGER: case SQL_CONVERT_LONGVARCHAR: case SQL_CONVERT_NUMERIC: case SQL_CONVERT_REAL:
    case SQL_CONVERT_SMALLINT: case SQL_CONVERT_TIME: case SQL_CONVERT_TIMESTAMP: case SQL_CONVERT_TINYINT:
    case SQL_CONVERT_VARBINARY: case SQL_CONVERT_VARCHAR: case SQL_CONVERT_LONGVARBINARY:
    case SQL_CONVERT_WCHAR: case SQL_CONVERT_WVARCHAR: case SQL_CONVERT_WLONGVARCHAR:
    case SQL_CONVERT_INTERVAL_DAY_TIME: case SQL_CONVERT_INTERVAL_YEAR_MONTH: case SQL_CONVERT_GUID:
      return u32(0);

    // ---- DDL / DML: read-only
    case SQL_ALTER_DOMAIN: case SQL_ALTER_TABLE: case SQL_CREATE_ASSERTION: case SQL_CREATE_CHARACTER_SET:
    case SQL_CREATE_COLLATION: case SQL_CREATE_DOMAIN: case SQL_CREATE_SCHEMA: case SQL_CREATE_TABLE:
    case SQL_CREATE_TRANSLATION: case SQL_CREATE_VIEW: case SQL_DROP_ASSERTION: case SQL_DROP_CHARACTER_SET:
    case SQL_DROP_COLLATION: case SQL_DROP_DOMAIN: case SQL_DROP_SCHEMA: case SQL_DROP_TABLE:
    case SQL_DROP_TRANSLATION: case SQL_DROP_VIEW: case SQL_INDEX_KEYWORDS: case SQL_INSERT_STATEMENT:
    case SQL_INFO_SCHEMA_VIEWS:
      return u32(0);
    case SQL_DDL_INDEX: return u32(0);
    case SQL_COLLATION_SEQ: return str("");
  }
  throw OdbcError("HY096", "Information type " + std::to_string(t) + " is not supported");
}

bool driver_supports(SQLUSMALLINT f) {
  switch (f) {
    case SQL_API_SQLALLOCHANDLE: case SQL_API_SQLFREEHANDLE: case SQL_API_SQLCONNECT:
    case SQL_API_SQLDRIVERCONNECT: case SQL_API_SQLDISCONNECT: case SQL_API_SQLGETINFO:
    case SQL_API_SQLGETFUNCTIONS: case SQL_API_SQLGETTYPEINFO: case SQL_API_SQLSETENVATTR:
    case SQL_API_SQLGETENVATTR: case SQL_API_SQLSETCONNECTATTR: case SQL_API_SQLGETCONNECTATTR:
    case SQL_API_SQLSETSTMTATTR: case SQL_API_SQLGETSTMTATTR: case SQL_API_SQLPREPARE:
    case SQL_API_SQLEXECUTE: case SQL_API_SQLEXECDIRECT: case SQL_API_SQLNUMRESULTCOLS:
    case SQL_API_SQLDESCRIBECOL: case SQL_API_SQLCOLATTRIBUTE: case SQL_API_SQLBINDCOL:
    case SQL_API_SQLFETCH: case SQL_API_SQLFETCHSCROLL: case SQL_API_SQLGETDATA:
    case SQL_API_SQLROWCOUNT: case SQL_API_SQLMORERESULTS: case SQL_API_SQLCLOSECURSOR:
    case SQL_API_SQLFREESTMT: case SQL_API_SQLCANCEL: case SQL_API_SQLENDTRAN:
    case SQL_API_SQLGETDIAGREC: case SQL_API_SQLGETDIAGFIELD: case SQL_API_SQLBINDPARAMETER:
    case SQL_API_SQLNUMPARAMS: case SQL_API_SQLDESCRIBEPARAM: case SQL_API_SQLNATIVESQL:
    case SQL_API_SQLTABLES: case SQL_API_SQLCOLUMNS: case SQL_API_SQLPRIMARYKEYS:
    case SQL_API_SQLFOREIGNKEYS: case SQL_API_SQLSTATISTICS: case SQL_API_SQLSPECIALCOLUMNS:
    case SQL_API_SQLPROCEDURES: case SQL_API_SQLPROCEDURECOLUMNS: case SQL_API_SQLGETCURSORNAME:
    case SQL_API_SQLSETCURSORNAME:
      return true;
    default:
      return false;
  }
}

}  // namespace sf
