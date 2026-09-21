"""Authentication flows, connection attributes, and connection-level errors."""
import pyodbc
import pytest


def test_password_flow(cn):
    assert cn.getinfo(pyodbc.SQL_DBMS_NAME) == "Salesforce"
    assert cn.getinfo(pyodbc.SQL_DATA_SOURCE_READ_ONLY) is True  # pyodbc maps "Y" to bool


def test_jwt_bearer_flow(conn_str, keypair, stats):
    cs = conn_str(AuthType="JWT", PWD=None, SecurityToken=None, ClientSecret=None,
                  PrivateKeyFile=keypair["private"], JwtAudience="https://login.salesforce.com")
    with pyodbc.connect(cs) as cn:
        assert cn.cursor().execute("SELECT COUNT(*) FROM Account").fetchval() == 25
    assert stats()["token_requests"] == 1


def test_jwt_rejects_wrong_user(conn_str, keypair):
    cs = conn_str(AuthType="JWT", UID="someone@else.com", PWD=None, SecurityToken=None,
                  ClientSecret=None, PrivateKeyFile=keypair["private"])
    with pytest.raises(pyodbc.Error) as e:
        pyodbc.connect(cs)
    assert e.value.args[0] == "28000"


def test_jwt_requires_readable_key(conn_str):
    cs = conn_str(AuthType="JWT", PWD=None, PrivateKeyFile="/nonexistent/key.pem")
    with pytest.raises(pyodbc.Error) as e:
        pyodbc.connect(cs)
    assert e.value.args[0] == "28000"
    assert "PrivateKeyFile" in str(e.value)


def test_refresh_token_flow(conn_str):
    cs = conn_str(AuthType="RefreshToken", PWD=None, SecurityToken=None, RefreshToken="good-refresh")
    with pyodbc.connect(cs) as cn:
        assert cn.cursor().execute("SELECT COUNT(*) FROM Contact").fetchval() == 12


def test_refresh_token_rejected(conn_str):
    cs = conn_str(AuthType="RefreshToken", PWD=None, RefreshToken="stale")
    with pytest.raises(pyodbc.Error) as e:
        pyodbc.connect(cs)
    assert e.value.args[0] == "28000"


def test_client_credentials_flow(conn_str):
    cs = conn_str(AuthType="ClientCredentials", UID=None, PWD=None, SecurityToken=None)
    with pyodbc.connect(cs) as cn:
        assert cn.cursor().execute("SELECT COUNT(*) FROM Account").fetchval() == 25


def test_bad_password(conn_str):
    with pytest.raises(pyodbc.Error) as e:
        pyodbc.connect(conn_str(PWD="wrong"))
    assert e.value.args[0] == "28000"


def test_missing_credentials_is_reported(conn_str):
    cs = conn_str(AuthType=None, UID=None, PWD=None, SecurityToken=None, ClientSecret=None)
    with pytest.raises(pyodbc.Error) as e:
        pyodbc.connect(cs)
    assert e.value.args[0] == "28000"


def test_unknown_auth_type(conn_str):
    with pytest.raises(pyodbc.Error) as e:
        pyodbc.connect(conn_str(AuthType="magic"))
    assert e.value.args[0] == "HY024"


def test_plain_http_is_refused(conn_str):
    """Only loopback may use http; a real host must be https."""
    with pytest.raises(pyodbc.Error) as e:
        pyodbc.connect(conn_str(LoginUrl="http://login.salesforce.com"))
    assert e.value.args[0] == "08001"


def test_autocommit_and_read_only(cn):
    assert cn.autocommit is False or cn.autocommit is True  # attribute readable
    cn.autocommit = True
    assert cn.autocommit is True
    cn.commit()   # no-op: Salesforce has no transactions
    cn.rollback()


def test_getinfo_is_conservative(cn):
    """BI tools decide what SQL to emit from these answers."""
    assert cn.getinfo(pyodbc.SQL_TXN_CAPABLE) == 0             # SQL_TC_NONE
    assert cn.getinfo(pyodbc.SQL_MAX_TABLES_IN_SELECT) == 1    # no joins
    assert cn.getinfo(pyodbc.SQL_SUBQUERIES) == 0  # SQL_OUTER_JOINS is covered in the C harness
    assert cn.getinfo(pyodbc.SQL_UNION) == 0
    assert cn.getinfo(pyodbc.SQL_GROUP_BY) == 0                # SQL_GB_NOT_SUPPORTED
    assert cn.getinfo(pyodbc.SQL_IDENTIFIER_QUOTE_CHAR) == '"'
    assert cn.getinfo(pyodbc.SQL_CATALOG_NAME) is False  # "N": no catalogs exposed
    assert cn.getinfo(pyodbc.SQL_DESCRIBE_PARAMETER) is True
    assert cn.getinfo(pyodbc.SQL_SEARCH_PATTERN_ESCAPE) == "\\"


def test_driver_reports_its_identity(cn):
    assert cn.getinfo(pyodbc.SQL_DRIVER_NAME) == "libsfodbc"
    assert cn.getinfo(pyodbc.SQL_DRIVER_ODBC_VER) == "03.80"
    assert cn.getinfo(pyodbc.SQL_USER_NAME) == "dean@example.com"


def test_connection_string_braces_and_semicolons(conn_str):
    """Passwords with ; or } must survive the connection-string parser."""
    cs = conn_str(PWD="{se;cret}}x}", SecurityToken=None)
    with pytest.raises(pyodbc.Error) as e:
        pyodbc.connect(cs)
    assert e.value.args[0] == "28000"  # parsed and sent, rejected by the server (not a parse error)


def test_statement_requires_open_connection(conn_str):
    cn = pyodbc.connect(conn_str())
    cn.close()
    with pytest.raises(pyodbc.Error):
        cn.cursor()


def test_metadata_cache_is_shared_between_connections(conn_str, stats):
    """The second connection should not re-describe the same objects."""
    with pyodbc.connect(conn_str()) as a:
        a.cursor().execute("SELECT Id FROM Account LIMIT 1").fetchall()
    first = stats()["describe_requests"]
    with pyodbc.connect(conn_str()) as b:
        b.cursor().execute("SELECT Id FROM Account LIMIT 1").fetchall()
    assert stats()["describe_requests"] == first


def test_cache_ttl_zero_forces_refresh(conn_str, stats):
    with pyodbc.connect(conn_str(MetadataCacheTTL="0")) as a:
        a.cursor().execute("SELECT Id FROM Account LIMIT 1").fetchall()
        first = stats()["describe_requests"]
        a.cursor().execute("SELECT Id FROM Account LIMIT 1").fetchall()
    assert stats()["describe_requests"] > first
