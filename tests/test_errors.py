"""Error mapping, retries, and session recovery."""
import pyodbc
import pytest


def test_server_error_maps_to_sqlstate(cur):
    """A Salesforce INVALID_FIELD becomes 42S22 even when it comes from the server."""
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT Id FROM Account WHERE Description LIKE 'x%'").fetchall()
    # Description is not filterable in this org, so the server rejects the filter.
    assert e.value.args[0] == "42S22"
    assert "Description" in str(e.value)


def test_messages_carry_the_vendor_prefix(cur):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT Nope FROM Account")
    assert "[Eloquix][Salesforce ODBC]" in str(e.value)


def test_transient_503_is_retried(cur, fail, stats):
    fail(status=503, count=2)
    rows = cur.execute("SELECT Id FROM Account LIMIT 1").fetchall()
    assert len(rows) == 1


def test_429_is_retried_with_retry_after(cur, fail):
    fail(status=429, count=1)
    assert cur.execute("SELECT COUNT(*) FROM Account").fetchval() == 25


def test_expired_session_is_reauthenticated(cur, fail, stats):
    before = stats()["token_requests"]
    fail(status=401, count=1)
    rows = cur.execute("SELECT Id FROM Account LIMIT 1").fetchall()
    assert len(rows) == 1
    assert stats()["token_requests"] == before + 1


def test_retries_give_up_and_report(conn_str, fail):
    with pyodbc.connect(conn_str(MaxRetries="1")) as cn:
        fail(status=503, count=5)
        with pytest.raises(pyodbc.Error) as e:
            cn.cursor().execute("SELECT Id FROM Account LIMIT 1")
        assert e.value.args[0] == "HY000"


def test_diagnostics_survive_a_failed_statement(cur):
    with pytest.raises(pyodbc.Error) as e:
        cur.execute("SELECT Id FROM Account WHERE NumberOfEmployees > 'x'")
    assert e.value.args[0] == "22018"
    # the connection is still usable
    assert cur.execute("SELECT COUNT(*) FROM Account").fetchval() == 25
