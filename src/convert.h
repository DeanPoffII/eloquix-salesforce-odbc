#pragma once
#include "sql.h"

namespace sf {

// Tracks chunked SQLGetData progress for one column.
struct GetDataState {
  SQLLEN offset = 0;  // bytes (char/binary) or characters (wchar) already returned
  bool done = false;
  bool active = false;
};

// Convert a column value into an application buffer.
// Returns SQL_SUCCESS / SQL_SUCCESS_WITH_INFO / SQL_NO_DATA / SQL_ERROR (diag populated).
SQLRETURN get_value(Diag& diag, const ColumnInfo& col, const Value& v, SQLSMALLINT c_type, SQLPOINTER target,
                    SQLLEN buffer_len, SQLLEN* str_len_or_ind, GetDataState* state);

// C type used for SQL_C_DEFAULT
SQLSMALLINT default_c_type(SQLSMALLINT sql_type);

// Size of an element of a fixed-length C type (0 for variable-length types).
SQLLEN c_type_size(SQLSMALLINT c_type);

struct ParamBinding {
  bool bound = false;
  SQLSMALLINT io_type = SQL_PARAM_INPUT;
  SQLSMALLINT c_type = SQL_C_DEFAULT;
  SQLSMALLINT sql_type = SQL_VARCHAR;
  SQLULEN column_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLPOINTER value = nullptr;
  SQLLEN buffer_len = 0;
  SQLLEN* str_len_or_ind = nullptr;
};

// Read a bound input parameter (row 0 of the parameter set) as a literal.
Lit read_param(const ParamBinding& b, SQLLEN bind_offset);

}  // namespace sf
