"""Runs the C conformance harness, which exercises ODBC paths pyodbc cannot reach.

Block fetching with row arrays, row-wise binding, chunked SQLGetData, SQL_C_NUMERIC,
SQLDescribeParam and the Unicode entry points are what SSIS, Alteryx and Excel use.
"""
import pathlib
import subprocess

import pytest

ROOT = pathlib.Path(__file__).parent.parent
HARNESS = ROOT / "build" / "odbc_conformance"


@pytest.mark.skipif(not HARNESS.exists(), reason="conformance harness not built")
def test_odbc_conformance(conn_str):
    proc = subprocess.run([str(HARNESS), conn_str()], capture_output=True, text=True, timeout=180)
    print(proc.stdout)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "0 failures" in proc.stdout
