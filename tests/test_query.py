"""SQL-to-SOQL translation: projection, filters, ordering, paging, parameters."""
import pyodbc
import pytest


def only_soql(stmts):
    """The single SOQL statement the driver sent for this test."""
    assert len(stmts) == 1, stmts
    return stmts[0]


# ---------------------------------------------------------------- projection

def test_select_star_lists_fields_and_skips_compound(cur, soql):
    cur.execute("SELECT * FROM Account LIMIT 1")
    names = [d[0] for d in cur.description]
    assert "Name" in names and "BillingCity" in names
    assert "BillingAddress" not in names   # compound field replaced by its components
    assert "*" not in only_soql(soql())


def test_column_list_is_pushed_down(cur, soql):
    cur.execute("SELECT Id, Name FROM Account LIMIT 2")
    assert only_soql(soql()).startswith("SELECT Id,Name FROM Account")


def test_column_alias(cur):
    cur.execute("SELECT Name AS company FROM Account LIMIT 1")
    assert cur.description[0][0] == "company"


def test_quoted_identifiers(cur):
    for sql in ['SELECT "Name" FROM "Account" LIMIT 1',
                "SELECT [Name] FROM [Account] LIMIT 1",
                "SELECT `Name` FROM `Account` LIMIT 1"]:
        assert cur.execute(sql).fetchone() is not None


def test_object_and_column_names_are_case_insensitive(cur, soql):
    cur.execute("select id, name from account limit 1")
    assert "FROM Account" in only_soql(soql())   # canonical API names are sent


def test_table_alias(cur, soql):
    cur.execute("SELECT a.Name FROM Account a WHERE a.Industry = 'Banking' LIMIT 1")
    assert "a." not in only_soql(soql())


def test_relationship_path(cur, soql):
    cur.execute("SELECT Name, Owner.Name, Owner.Email FROM Account LIMIT 3")
    assert "Owner.Name" in only_soql(soql())
    rows = cur.fetchall()
    assert all(r[1] in ("Dana Owner", "Riley Rep") for r in rows)


def test_relationship_null_parent_is_null(cur):
    """Accounts without a parent must yield NULL, not an error."""
    rows = cur.execute("SELECT Id, Parent.Name FROM Account ORDER BY Id").fetchall()
    assert any(r[1] is None for r in rows)
    assert any(r[1] is not None for r in rows)


def test_unknown_column_is_42S22(cur):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT Nope FROM Account")
    assert e.value.args[0] == "42S22"


def test_unknown_table_is_42S02(cur):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT Id FROM Nope__c")
    assert e.value.args[0] == "42S02"


# ---------------------------------------------------------------- filters

def test_string_filter_is_quoted(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE Industry = 'Banking'")
    assert "Industry = 'Banking'" in only_soql(soql())


def test_string_literal_escaping(cur, soql):
    rows = cur.execute("SELECT Id, Name FROM Account WHERE Name = 'O''Brien & Sons'").fetchall()
    assert len(rows) == 1
    assert "\\'" in only_soql(soql())


def test_date_literal_is_unquoted(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE LastActivityDate > '2024-06-01'")
    sent = only_soql(soql())
    assert "LastActivityDate > 2024-06-01" in sent  # SOQL dates are never quoted


def test_odbc_date_escape(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE LastActivityDate >= {d '2024-06-01'}")
    assert "2024-06-01" in only_soql(soql())


def test_datetime_is_converted_to_utc(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE CreatedDate > {ts '2024-01-01 12:00:00'}")
    assert "CreatedDate > 2024-01-01T12:00:00Z" in only_soql(soql())


def test_boolean_literal_is_bare(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE IsActive__c = 1")
    assert "IsActive__c = true" in only_soql(soql())


def test_invalid_date_literal_is_22007(cur):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT Id FROM Account WHERE LastActivityDate > 'not-a-date'")
    assert e.value.args[0] == "22007"


def test_invalid_numeric_literal_is_22018(cur):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT Id FROM Account WHERE NumberOfEmployees > 'many'")
    assert e.value.args[0] == "22018"


def test_and_or_not_and_parens(cur, soql):
    rows = cur.execute(
        "SELECT Id FROM Account WHERE (Industry = 'Banking' OR Industry = 'Retail') "
        "AND NOT IsActive__c = 0").fetchall()
    sent = only_soql(soql())
    assert " OR " in sent and " AND " in sent and "NOT" in sent
    assert len(rows) > 0


def test_in_list(cur, soql):
    rows = cur.execute("SELECT Id FROM Account WHERE Industry IN ('Banking', 'Retail')").fetchall()
    assert "IN (" in only_soql(soql())
    assert len(rows) > 0


def test_not_in_list(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE Industry NOT IN ('Banking')")
    assert "NOT IN (" in only_soql(soql())


def test_between_becomes_range(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE NumberOfEmployees BETWEEN 50 AND 100")
    sent = only_soql(soql())
    assert ">= 50" in sent and "<= 100" in sent


def test_like_pattern(cur, soql):
    rows = cur.execute("SELECT Name FROM Account WHERE Name LIKE 'Acme%'").fetchall()
    assert "LIKE 'Acme%'" in only_soql(soql())
    assert all(r[0].startswith("Acme") for r in rows)


def test_is_null_and_is_not_null(cur, soql):
    nulls = cur.execute("SELECT Id FROM Account WHERE AnnualRevenue IS NULL").fetchall()
    assert "AnnualRevenue = null" in only_soql(soql())
    assert len(nulls) == 3   # every 7th of 25 records


def test_literal_only_where_never_calls_the_api(cur, soql, stats):
    """Power BI and Tableau probe metadata with WHERE 1=0."""
    before = stats()["query_requests"]
    cur.execute("SELECT Id, Name FROM Account WHERE 1=0")
    assert cur.fetchall() == []
    assert soql() == []
    assert stats()["query_requests"] == before
    assert [d[0] for d in cur.description] == ["Id", "Name"]


def test_always_true_predicate_is_folded_away(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE 1=1 AND Industry = 'Banking'")
    sent = only_soql(soql())
    assert "1" not in sent.split("WHERE")[1]


def test_select_without_from_is_a_connection_test(cur, soql, stats):
    before = stats()["query_requests"]
    assert cur.execute("SELECT 1").fetchval() == 1
    assert stats()["query_requests"] == before
    assert soql() == []


def test_comparing_two_columns_is_rejected(cur):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT Id FROM Account WHERE Name = Industry")
    assert e.value.args[0] == "42000"


# ---------------------------------------------------------------- ordering and paging

def test_order_by_direction_and_nulls(cur, soql):
    cur.execute("SELECT Id, Name FROM Account ORDER BY Name DESC NULLS LAST")
    assert "ORDER BY Name DESC NULLS LAST" in only_soql(soql())


def test_order_by_ordinal(cur, soql):
    cur.execute("SELECT Name, Industry FROM Account ORDER BY 2")
    assert "ORDER BY Industry ASC" in only_soql(soql())


def test_order_by_relationship_field(cur, soql):
    cur.execute("SELECT Id FROM Account ORDER BY Owner.Name")
    assert "ORDER BY Owner.Name ASC" in only_soql(soql())


def test_limit_and_offset(cur, soql):
    rows = cur.execute("SELECT Id FROM Account ORDER BY Id LIMIT 5 OFFSET 2").fetchall()
    assert len(rows) == 5
    sent = only_soql(soql())
    assert sent.endswith("LIMIT 5 OFFSET 2")


def test_top_n(cur, soql):
    rows = cur.execute("SELECT TOP 3 Id FROM Account").fetchall()
    assert len(rows) == 3
    assert only_soql(soql()).endswith("LIMIT 3")


def test_fetch_first_rows_only(cur, soql):
    rows = cur.execute("SELECT Id FROM Account ORDER BY Id FETCH FIRST 4 ROWS ONLY").fetchall()
    assert len(rows) == 4


def test_count_star(cur, soql):
    assert cur.execute("SELECT COUNT(*) FROM Account").fetchval() == 25
    assert only_soql(soql()).startswith("SELECT COUNT() FROM Account")


def test_count_star_with_where(cur):
    assert cur.execute("SELECT COUNT(*) FROM Account WHERE Industry = 'Banking'").fetchval() == 5


def test_comments_are_ignored(cur):
    sql = """-- leading comment
             SELECT Id /* inline */ FROM Account LIMIT 1"""
    assert cur.execute(sql).fetchone() is not None


# ---------------------------------------------------------------- parameters

def test_parameter_is_typed_from_the_field(cur, soql):
    rows = cur.execute("SELECT Id FROM Account WHERE Industry = ?", "Banking").fetchall()
    assert "Industry = 'Banking'" in only_soql(soql())
    assert len(rows) == 5


def test_numeric_parameter_is_not_quoted(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE NumberOfEmployees > ?", 100)
    assert "NumberOfEmployees > 100" in only_soql(soql())


def test_date_parameter(cur, soql):
    import datetime
    cur.execute("SELECT Id FROM Account WHERE LastActivityDate > ?", datetime.date(2024, 6, 1))
    assert "LastActivityDate > 2024-06-01" in only_soql(soql())


def test_datetime_parameter(cur, soql):
    import datetime
    cur.execute("SELECT Id FROM Account WHERE CreatedDate > ?", datetime.datetime(2024, 1, 1, 8, 30))
    assert "CreatedDate > 2024-01-01T08:30:00Z" in only_soql(soql())


def test_boolean_parameter(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE IsActive__c = ?", True)
    assert "IsActive__c = true" in only_soql(soql())


def test_several_parameters_keep_their_order(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE Industry = ? AND NumberOfEmployees > ?", "Banking", 20)
    sent = only_soql(soql())
    assert "Industry = 'Banking'" in sent and "NumberOfEmployees > 20" in sent


def test_prepared_statement_reruns_with_new_values(cur, soql):
    for industry in ("Banking", "Retail"):
        cur.execute("SELECT Id FROM Account WHERE Industry = ?", industry).fetchall()
    sent = soql()
    assert len(sent) == 2
    assert "'Banking'" in sent[0] and "'Retail'" in sent[1]


def test_parameter_in_like(cur, soql):
    rows = cur.execute("SELECT Name FROM Account WHERE Name LIKE ?", "Acme%").fetchall()
    assert len(rows) > 0


def test_parameter_in_in_list(cur, soql):
    rows = cur.execute("SELECT Id FROM Account WHERE Industry IN (?, ?)", "Banking", "Retail").fetchall()
    assert len(rows) > 0


def test_null_parameter(cur, soql):
    cur.execute("SELECT Id FROM Account WHERE Industry = ?", None)
    assert "Industry = null" in only_soql(soql())


# ---------------------------------------------------------------- unsupported SQL

@pytest.mark.parametrize("sql, fragment", [
    ("SELECT a.Id FROM Account a JOIN Contact c ON c.AccountId = a.Id", "Joins"),
    ("SELECT Id FROM Account WHERE Id IN (SELECT AccountId FROM Contact)", "subquery"),
    ("SELECT Industry, COUNT(*) FROM Account GROUP BY Industry", "GROUP BY"),
    ("SELECT Id FROM Account UNION SELECT Id FROM Contact", "UNION"),
    ("SELECT DISTINCT Industry FROM Account", "DISTINCT"),
    ("SELECT UPPER(Name) FROM Account", "UPPER"),
    ("WITH x AS (SELECT Id FROM Account) SELECT * FROM x", "WITH"),
])
def test_unsupported_sql_is_rejected_clearly(cur, sql, fragment):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute(sql)
    assert e.value.args[0] == "42000"
    assert fragment.lower() in str(e.value).lower()


@pytest.mark.parametrize("sql", [
    "INSERT INTO Account (Name) VALUES ('x')",
    "UPDATE Account SET Name = 'x' WHERE Id = '1'",
    "DELETE FROM Account WHERE Id = '1'",
])
def test_writes_are_refused_in_this_release(cur, sql):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute(sql)
    assert "read-only" in str(e.value)


def test_syntax_error_reports_a_position(cur):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT FROM WHERE")
    assert e.value.args[0] == "42000"
    assert "position" in str(e.value)
