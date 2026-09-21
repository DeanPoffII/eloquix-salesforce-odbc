"""Value conversion: types, NULLs, Unicode, long text, precision."""
import datetime
import decimal


def test_python_types_match_salesforce_types(cur):
    row = cur.execute(
        "SELECT Id, Name, AnnualRevenue, NumberOfEmployees, IsActive__c, Rating__c, "
        "LastActivityDate, CreatedDate FROM Account WHERE Id = '001000000000000001'").fetchone()
    assert isinstance(row.Id, str) and len(row.Id) == 18
    assert isinstance(row.Name, str)
    assert isinstance(row.AnnualRevenue, decimal.Decimal)
    assert isinstance(row.NumberOfEmployees, int)
    assert isinstance(row.IsActive__c, bool)
    assert isinstance(row.LastActivityDate, datetime.date)
    assert isinstance(row.CreatedDate, datetime.datetime)


def test_currency_keeps_two_decimals(cur):
    value = cur.execute(
        "SELECT AnnualRevenue FROM Account WHERE Id = '001000000000000002'").fetchval()
    assert value == decimal.Decimal("250001")


def test_decimal_is_not_rounded_through_float(cur):
    value = cur.execute(
        "SELECT AnnualRevenue FROM Account WHERE Id = '001000000000000003'").fetchval()
    assert value == decimal.Decimal("375001.5")


def test_datetime_is_returned_in_utc(cur):
    value = cur.execute(
        "SELECT CreatedDate FROM Account WHERE Id = '001000000000000001'").fetchval()
    assert value == datetime.datetime(2023, 6, 14, 14, 30, 1)


def test_date_has_no_time_part(cur):
    value = cur.execute(
        "SELECT LastActivityDate FROM Account WHERE Id = '001000000000000001'").fetchval()
    assert value == datetime.date(2024, 1, 12)


def test_nulls_come_back_as_none(cur):
    rows = cur.execute(
        "SELECT Id, AnnualRevenue, NumberOfEmployees, Industry FROM Account ORDER BY Id").fetchall()
    assert any(r.AnnualRevenue is None for r in rows)
    assert any(r.NumberOfEmployees is None for r in rows)
    assert any(r.Industry is None for r in rows)


def test_unicode_round_trip(cur):
    names = [r.Name for r in cur.execute("SELECT Name FROM Account ORDER BY Id LIMIT 5").fetchall()]
    assert "Café Münch GmbH" in names
    assert "東京データ株式会社" in names
    assert "Rocket 🚀 Labs" in names          # outside the BMP: surrogate pair handling


def test_unicode_filter_round_trip(cur):
    rows = cur.execute("SELECT Id FROM Account WHERE Name = ?", "東京データ株式会社").fetchall()
    assert len(rows) == 1


def test_long_text_is_returned_whole(cur):
    value = cur.execute(
        "SELECT Description FROM Account WHERE Id = '001000000000000001'").fetchval()
    assert len(value) == len("Long description " * 600)
    assert value.endswith("Long description ")


def test_describe_reports_useful_column_metadata(cur):
    cur.execute("SELECT Id, Name, AnnualRevenue, IsActive__c, CreatedDate FROM Account LIMIT 1")
    by_name = {d[0]: d for d in cur.description}
    assert by_name["Id"][3] == 18                     # column size
    assert by_name["Name"][3] == 255
    assert by_name["AnnualRevenue"][4] == 18          # precision
    assert by_name["AnnualRevenue"][5] == 2           # scale
    assert by_name["Id"][6] is False                  # not nullable
    assert by_name["AnnualRevenue"][6] is True


def test_relationship_column_is_nullable_even_if_target_is_not(cur):
    cur.execute("SELECT Owner.Name FROM Account LIMIT 1")
    assert cur.description[0][6] is True


def test_literal_select_types(cur):
    row = cur.execute("SELECT 42, 'text', {d '2024-03-01'}").fetchone()
    assert row[0] == 42
    assert row[1] == "text"
    assert row[2] == datetime.date(2024, 3, 1)
