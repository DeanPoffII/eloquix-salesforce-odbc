#pragma once
#include "handles.h"

namespace sf {

using OptStr = std::optional<std::string>;

std::unique_ptr<Cursor> catalog_tables(Conn& c, bool metadata_id, const OptStr& cat, const OptStr& schema,
                                       const OptStr& table, const OptStr& types);
std::unique_ptr<Cursor> catalog_columns(Conn& c, bool metadata_id, const OptStr& cat, const OptStr& schema,
                                        const OptStr& table, const OptStr& column);
std::unique_ptr<Cursor> catalog_primary_keys(Conn& c, const OptStr& cat, const OptStr& schema, const OptStr& table);
std::unique_ptr<Cursor> catalog_foreign_keys(Conn& c, const OptStr& pk_table, const OptStr& fk_table);
std::unique_ptr<Cursor> catalog_statistics(Conn& c, const OptStr& table);
std::unique_ptr<Cursor> catalog_special_columns(Conn& c, SQLUSMALLINT id_type, const OptStr& table);
std::unique_ptr<Cursor> catalog_procedures();
std::unique_ptr<Cursor> catalog_procedure_columns();
std::unique_ptr<Cursor> type_info(SQLSMALLINT data_type, SQLINTEGER odbc_version);

// SQLGetInfo
struct InfoValue {
  enum Kind { Str, U16, U32 } kind = Str;
  std::string s;
  uint32_t n = 0;
};
InfoValue get_info(Conn& c, SQLUSMALLINT info_type);
bool driver_supports(SQLUSMALLINT function_id);

}  // namespace sf
