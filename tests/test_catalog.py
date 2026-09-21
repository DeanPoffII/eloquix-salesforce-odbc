"""Catalog functions: SQLTables, SQLColumns, keys, statistics, type info.

BI tools call these constantly, so they must be complete, correctly shaped, and cheap.
"""
import pyodbc
import pytest

SQL_WVARCHAR = -9
SQL_WLONGVARCHAR = -10
SQL_BIT = -7
SQL_INTEGER = 4
SQL_BIGINT = -5
SQL_DECIMAL = 3
SQL_TYPE_DATE = 91
SQL_TYPE_TIMESTAMP = 93


def test_tables_lists_queryable_objects(cur):
    rows = cur.tables().fetchall()
    names = [r.table_name for r in rows]
    assert {"Account", "Contact", "User"} <= set(names)
    assert "AccountShare" not in names        # not queryable
    assert all(r.table_type == "TABLE" for r in rows)
    assert all(r.table_cat is None and r.table_schem is None for r in rows)


def test_tables_returns_labels_as_remarks(cur):
    row = next(r for r in cur.tables(table="Account").fetchall())
    assert row.remarks == "Account"


def test_tables_name_pattern(cur):
    assert [r.table_name for r in cur.tables(table="Acc%").fetchall()] == ["Account"]


def test_tables_type_filter(cur):
    assert cur.tables(tableType="VIEW").fetchall() == []
    assert len(cur.tables(tableType="TABLE").fetchall()) == 3


def test_table_type_probe(cur):
    """SQLTables("","","","%") must list the supported table types."""
    rows = cur.tables(catalog="", schema="", table="", tableType="%").fetchall()
    assert [r.table_type for r in rows] == ["TABLE"]


def test_catalog_and_schema_probes_are_empty(cur):
    assert cur.tables(catalog="%", schema="", table="").fetchall() == []
    assert cur.tables(catalog="", schema="%", table="").fetchall() == []


def test_include_non_queryable_option(conn_str):
    with pyodbc.connect(conn_str(IncludeNonQueryable="true")) as cn:
        names = [r.table_name for r in cn.cursor().tables().fetchall()]
    assert "AccountShare" in names


def test_columns_shape_and_ordinals(cur):
    rows = cur.columns(table="Account").fetchall()
    assert [r.column_name for r in rows][:3] == ["Id", "Name", "Industry"]
    assert [r.ordinal_position for r in rows] == list(range(1, len(rows) + 1))
    assert all(r.table_name == "Account" for r in rows)


def test_columns_type_mapping(cur):
    by_name = {r.column_name: r for r in cur.columns(table="Account").fetchall()}
    assert by_name["Id"].data_type == SQL_WVARCHAR and by_name["Id"].column_size == 18
    assert by_name["Name"].data_type == SQL_WVARCHAR and by_name["Name"].column_size == 255
    assert by_name["IsActive__c"].data_type == SQL_BIT
    assert by_name["NumberOfEmployees"].data_type == SQL_INTEGER
    assert by_name["AnnualRevenue"].data_type == SQL_DECIMAL
    assert by_name["AnnualRevenue"].decimal_digits == 2
    assert by_name["LastActivityDate"].data_type == SQL_TYPE_DATE
    assert by_name["CreatedDate"].data_type == SQL_TYPE_TIMESTAMP
    assert by_name["Description"].data_type == SQL_WLONGVARCHAR   # 32000 chars
    assert "BillingAddress" not in by_name                        # compound field hidden


def test_columns_nullability(cur):
    by_name = {r.column_name: r for r in cur.columns(table="Account").fetchall()}
    assert by_name["Name"].nullable == 0 and by_name["Name"].is_nullable == "NO"
    assert by_name["Industry"].nullable == 1 and by_name["Industry"].is_nullable == "YES"


def test_columns_octet_length_counts_utf16_bytes(cur):
    row = next(r for r in cur.columns(table="Account").fetchall() if r.column_name == "Name")
    assert row.char_octet_length == 255 * 2


def test_columns_column_pattern(cur):
    rows = cur.columns(table="Account", column="Billing%").fetchall()
    assert [r.column_name for r in rows] == ["BillingCity"]


def test_columns_for_all_tables(cur):
    rows = cur.columns().fetchall()
    assert len({r.table_name for r in rows}) == 3


def test_columns_uses_one_batched_describe(cur, stats):
    """All objects are described in a single Composite Batch call."""
    before = stats()
    cur.columns().fetchall()
    after = stats()
    assert after["batch_requests"] - before["batch_requests"] <= 1


def test_primary_keys(cur):
    rows = cur.primaryKeys("Account").fetchall()
    assert len(rows) == 1
    assert rows[0].column_name == "Id" and rows[0].key_seq == 1


def test_foreign_keys_from_child(cur):
    """Which parents does Account point at? Power BI builds relationships from this."""
    rows = cur.foreignKeys(foreignTable="Account").fetchall()
    pairs = {(r.pktable_name, r.fkcolumn_name) for r in rows}
    assert ("User", "OwnerId") in pairs
    assert ("Account", "ParentId") in pairs
    assert all(r.pkcolumn_name == "Id" for r in rows)


def test_foreign_keys_from_parent(cur):
    """Which children point at Account?"""
    rows = cur.foreignKeys(table="Account").fetchall()
    pairs = {(r.fktable_name, r.fkcolumn_name) for r in rows}
    assert ("Contact", "AccountId") in pairs
    assert ("Account", "ParentId") in pairs
    assert not any(child == "AccountShare" for child, _ in pairs)   # not queryable


def test_foreign_keys_both_sides_narrows(cur):
    rows = cur.foreignKeys(table="User", foreignTable="Account").fetchall()
    assert {r.fkcolumn_name for r in rows} == {"OwnerId"}


def test_statistics_reports_the_id_index(cur):
    rows = cur.statistics("Account").fetchall()
    assert rows[0].column_name == "Id"
    assert rows[0].non_unique == 0


def test_special_columns_best_rowid(cur):
    rows = cur.rowIdColumns("Account").fetchall()
    assert [r.column_name for r in rows] == ["Id"]


def test_special_columns_row_version(cur):
    rows = cur.rowVerColumns("Account").fetchall()
    assert [r.column_name for r in rows] == ["SystemModstamp"]


def test_procedures_are_empty(cur):
    assert cur.procedures().fetchall() == []
    assert cur.procedureColumns().fetchall() == []


def test_get_type_info_is_sorted_by_data_type(cur):
    rows = cur.getTypeInfo().fetchall()
    codes = [r.data_type for r in rows]
    assert codes == sorted(codes)
    names = {r.type_name for r in rows}
    assert {"NVARCHAR", "BIT", "INTEGER", "BIGINT", "DECIMAL", "DATE", "TIMESTAMP"} <= names


def test_get_type_info_for_one_type(cur):
    rows = cur.getTypeInfo(SQL_WVARCHAR).fetchall()
    assert [r.type_name for r in rows] == ["NVARCHAR"]
    assert rows[0].literal_prefix == "'"


def test_catalog_results_are_describable(cur):
    """Some tools call SQLDescribeCol on catalog result sets before fetching."""
    cur.tables(table="Account")
    assert [d[0] for d in cur.description] == [
        "table_cat", "table_schem", "table_name", "table_type", "remarks"]


def test_unknown_table_yields_no_rows_not_an_error(cur):
    assert cur.primaryKeys("NoSuchObject").fetchall() == []
    assert cur.columns(table="NoSuchObject").fetchall() == []
