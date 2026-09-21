"""Shared fixtures: starts the mock Salesforce server and points pyodbc at the built driver."""
import os
import pathlib
import socket
import subprocess
import sys
import time

import pyodbc
import pytest
import urllib.request

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parent
DRIVER = os.environ.get("SFODBC_DRIVER", str(ROOT / "build" / "libsfodbc.so"))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http(url, method="GET", timeout=10):
    req = urllib.request.Request(url, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()


@pytest.fixture(scope="session")
def keypair(tmp_path_factory):
    """RSA keypair for the JWT bearer flow."""
    d = tmp_path_factory.mktemp("keys")
    priv, pub = d / "server.key", d / "server.pub"
    subprocess.run(["openssl", "genrsa", "-out", str(priv), "2048"], check=True, capture_output=True)
    subprocess.run(["openssl", "rsa", "-in", str(priv), "-pubout", "-out", str(pub)],
                   check=True, capture_output=True)
    return {"private": str(priv), "public": str(pub)}


@pytest.fixture(scope="session")
def server(keypair):
    port = free_port()
    env = dict(os.environ, MOCK_PAGE_SIZE="10", MOCK_JWT_PUBLIC_KEY=keypair["public"])
    proc = subprocess.Popen([sys.executable, str(HERE / "mock_salesforce.py"), str(port)],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{port}"
    for _ in range(100):
        try:
            http(base + "/__stats")
            break
        except Exception:
            time.sleep(0.05)
    else:
        proc.kill()
        raise RuntimeError("mock server did not start: " + proc.stdout.read().decode())
    yield base
    proc.terminate()
    proc.wait(timeout=10)


@pytest.fixture(autouse=True)
def reset(server):
    http(server + "/__reset", method="POST")
    yield


@pytest.fixture
def conn_str(server):
    def make(**extra):
        parts = {
            "DRIVER": DRIVER,
            "AuthType": "Password",
            "LoginUrl": server,
            "ClientId": "cid",
            "ClientSecret": "csecret",
            "UID": "dean@example.com",
            "PWD": "secret",
            "SecurityToken": "TOKEN123",
        }
        parts.update(extra)
        return ";".join(f"{k}={v}" for k, v in parts.items() if v is not None)
    return make


@pytest.fixture
def cn(conn_str):
    c = pyodbc.connect(conn_str())
    yield c
    c.close()


@pytest.fixture
def cur(cn):
    with cn.cursor() as c:
        yield c


@pytest.fixture
def soql(server):
    """SOQL statements the server has received during this test."""
    import json

    def get():
        return json.loads(http(server + "/__soql"))
    return get


@pytest.fixture
def stats(server):
    import json

    def get():
        return json.loads(http(server + "/__stats"))
    return get


@pytest.fixture
def fail(server):
    """Make the next N API calls fail with the given status."""
    def inject(status=503, count=1):
        http(f"{server}/__fail?status={status}&count={count}", method="POST")
    return inject
