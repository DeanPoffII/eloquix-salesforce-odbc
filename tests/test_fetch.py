"""Fetching: paging through queryMore, row counts, max rows, cursor lifecycle."""
import pyodbc
import pytest


def test_paging_fetches_every_row(cur, stats):
    """The mock server pages every 10 records; 25 accounts means 3 requests."""
    rows = cur.execute("SELECT Id FROM Account ORDER BY Id").fetchall()
    assert len(rows) == 25
    assert len({r.Id for r in rows}) == 25
    assert stats()["query_requests"] == 3   # 1 query + 2 queryMore


def test_rows_are_in_order_across_pages(cur):
    rows = cur.execute("SELECT Id FROM Account ORDER BY Id").fetchall()
    assert [r.Id for r in rows] == sorted(r.Id for r in rows)


def test_fetchmany_spans_pages(cur):
    cur.execute("SELECT Id FROM Account ORDER BY Id")
    seen = []
    while True:
        batch = cur.fetchmany(7)
        if not batch:
            break
        seen.extend(batch)
    assert len(seen) == 25


def test_iteration_protocol(cur):
    count = sum(1 for _ in cur.execute("SELECT Id FROM Account"))
    assert count == 25


def test_rowcount_reports_total_size(cur):
    cur.execute("SELECT Id FROM Account")
    assert cur.rowcount == 25


def test_empty_result_set(cur):
    rows = cur.execute("SELECT Id FROM Account WHERE Industry = 'Nonexistent'").fetchall()
    assert rows == []


def test_max_rows_limits_the_query(cur, soql):
    cur.execute("SELECT Id FROM Account")           # prepare/execute path
    cur2 = cur.connection.cursor()
    cur2.execute("SELECT Id FROM Account ORDER BY Id LIMIT 4")
    assert len(cur2.fetchall()) == 4


def test_limit_smaller_than_page(cur, stats):
    rows = cur.execute("SELECT Id FROM Account ORDER BY Id LIMIT 3").fetchall()
    assert len(rows) == 3
    assert stats()["query_requests"] == 1        # no queryMore needed


def test_reexecute_resets_the_cursor(cur):
    cur.execute("SELECT Id FROM Account ORDER BY Id LIMIT 2")
    first = cur.fetchall()
    cur.execute("SELECT Id FROM Account ORDER BY Id LIMIT 2")
    assert [r.Id for r in cur.fetchall()] == [r.Id for r in first]


def test_cursor_can_be_reused_for_a_different_object(cur):
    cur.execute("SELECT Id FROM Account LIMIT 1").fetchall()
    rows = cur.execute("SELECT Id, LastName FROM Contact LIMIT 2").fetchall()
    assert len(rows) == 2


def test_two_cursors_on_one_connection(cn):
    a, b = cn.cursor(), cn.cursor()
    a.execute("SELECT Id FROM Account ORDER BY Id")
    b.execute("SELECT Id FROM Contact ORDER BY Id")
    assert len(a.fetchall()) == 25
    assert len(b.fetchall()) == 12


def test_fetch_after_exhaustion_returns_none(cur):
    cur.execute("SELECT Id FROM Account LIMIT 1")
    assert cur.fetchone() is not None
    assert cur.fetchone() is None


def test_nextset_reports_no_more_results(cur):
    cur.execute("SELECT Id FROM Account LIMIT 1")
    assert cur.nextset() is False


def test_batch_size_option_is_accepted(conn_str):
    with pyodbc.connect(conn_str(BatchSize="200")) as cn:
        assert len(cn.cursor().execute("SELECT Id FROM Account").fetchall()) == 25


def test_query_after_error_still_works(cur):
    with pytest.raises(pyodbc.Error):
        cur.execute("SELECT Nope FROM Account")
    assert cur.execute("SELECT COUNT(*) FROM Account").fetchval() == 25
