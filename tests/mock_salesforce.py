#!/usr/bin/env python3
"""Mock Salesforce REST API for driver tests.

Implements just enough of Salesforce to exercise the driver end to end:
OAuth token endpoint (JWT bearer with real RS256 verification, refresh token, password,
client credentials), describeGlobal, sObject describe, Composite Batch, query/queryMore
with a small SOQL WHERE evaluator, relationship fields, COUNT(), error payloads, and
fault injection (503s, expired sessions).

Test-control endpoints (not part of Salesforce):
  GET  /__soql          -> list of SOQL statements received
  GET  /__stats         -> request counters
  POST /__reset         -> clear logs and counters
  POST /__fail?status=503&count=1   -> next N API calls return that status
"""
import base64
import json
import os
import re
import sys
import threading
from datetime import date, datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

API = "v64.0"
PAGE_SIZE = int(os.environ.get("MOCK_PAGE_SIZE", "10"))
PUBLIC_KEY_FILE = os.environ.get("MOCK_JWT_PUBLIC_KEY")
VALID_TOKENS = set()
LOCK = threading.Lock()
STATE = {"soql": [], "token_requests": 0, "query_requests": 0, "describe_requests": 0,
         "batch_requests": 0, "fail_status": None, "fail_count": 0, "cursors": {}}


# ---------------------------------------------------------------- schema

def f(name, ftype, label=None, length=0, precision=0, scale=0, digits=0, nillable=True,
      reference_to=None, relationship_name=None, filterable=True):
    return {"name": name, "type": ftype, "label": label or name, "length": length,
            "precision": precision, "scale": scale, "digits": digits, "nillable": nillable,
            "referenceTo": reference_to or [], "relationshipName": relationship_name,
            "filterable": filterable, "sortable": filterable, "calculated": False,
            "unique": False, "caseSensitive": False, "autoNumber": False,
            "updateable": True, "createable": True}


SCHEMA = {
    "Account": {
        "label": "Account",
        "fields": [
            f("Id", "id", "Account ID", length=18, nillable=False),
            f("Name", "string", "Account Name", length=255, nillable=False),
            f("Industry", "picklist", length=255),
            f("AnnualRevenue", "currency", "Annual Revenue", precision=18, scale=2),
            f("NumberOfEmployees", "int", "Employees", digits=8),
            f("IsActive__c", "boolean", "Active", nillable=False),
            f("Rating__c", "percent", "Rating", precision=5, scale=1),
            f("BillingAddress", "address", "Billing Address"),
            f("BillingCity", "string", "Billing City", length=40),
            f("Description", "textarea", "Description", length=32000, filterable=False),
            f("LastActivityDate", "date", "Last Activity"),
            f("CreatedDate", "datetime", "Created Date", nillable=False),
            f("OwnerId", "reference", "Owner ID", length=18, reference_to=["User"],
              relationship_name="Owner", nillable=False),
            f("ParentId", "reference", "Parent Account ID", length=18, reference_to=["Account"],
              relationship_name="Parent"),
            f("SystemModstamp", "datetime", "System Modstamp", nillable=False),
        ],
        "childRelationships": [
            {"childSObject": "Contact", "field": "AccountId", "relationshipName": "Contacts"},
            {"childSObject": "Account", "field": "ParentId", "relationshipName": "ChildAccounts"},
            {"childSObject": "AccountShare", "field": "AccountId", "relationshipName": "Shares"},
        ],
    },
    "Contact": {
        "label": "Contact",
        "fields": [
            f("Id", "id", "Contact ID", length=18, nillable=False),
            f("FirstName", "string", length=40),
            f("LastName", "string", length=80, nillable=False),
            f("Email", "email", length=80),
            f("AccountId", "reference", "Account ID", length=18, reference_to=["Account"],
              relationship_name="Account"),
            f("Birthdate", "date"),
            f("SystemModstamp", "datetime", nillable=False),
        ],
        "childRelationships": [],
    },
    "User": {
        "label": "User",
        "fields": [
            f("Id", "id", length=18, nillable=False),
            f("Name", "string", length=121, nillable=False),
            f("Email", "email", length=80, nillable=False),
            f("SystemModstamp", "datetime", nillable=False),
        ],
        "childRelationships": [],
    },
}
NON_QUERYABLE = ["AccountShare"]


def sf_id(prefix, n):
    return f"{prefix}{n:015d}"


def build_data():
    users = [{"Id": sf_id("005", i), "Name": n, "Email": f"user{i}@example.com",
              "SystemModstamp": "2024-01-01T00:00:00.000+0000"}
             for i, n in enumerate(["Dana Owner", "Riley Rep"], start=1)]
    industries = ["Technology", "Banking", "Retail", None, "Healthcare"]
    names = ["Acme Corp", "Café Münch GmbH", "O'Brien & Sons", "Rocket 🚀 Labs", "東京データ株式会社"]
    accounts = []
    for i in range(1, 26):
        base = names[(i - 1) % len(names)]
        accounts.append({
            "Id": sf_id("001", i),
            "Name": base if i <= 5 else f"{base} {i}",
            "Industry": industries[i % len(industries)],
            "AnnualRevenue": None if i % 7 == 0 else round(i * 125000.5, 2),
            "NumberOfEmployees": None if i % 6 == 0 else i * 10,
            "IsActive__c": i % 3 != 0,
            "Rating__c": round(i * 3.7 % 100, 1),
            "BillingCity": "Kansas City" if i % 2 else "Überlingen",
            "Description": ("Long description " * 600) if i == 1 else None,
            "LastActivityDate": (date(2024, 1, 1) + timedelta(days=i * 11)).isoformat(),
            "CreatedDate": (datetime(2023, 6, 1, 14, 30, tzinfo=timezone.utc)
                            + timedelta(days=i * 13, seconds=i)).strftime("%Y-%m-%dT%H:%M:%S.000+0000"),
            "OwnerId": users[i % 2]["Id"],
            "ParentId": sf_id("001", 1) if i > 1 and i % 4 == 0 else None,
            "SystemModstamp": "2024-06-01T12:00:00.000+0000",
        })
    contacts = []
    for i in range(1, 13):
        contacts.append({
            "Id": sf_id("003", i), "FirstName": ["Ann", "José", None][i % 3],
            "LastName": f"Contact{i}", "Email": f"c{i}@example.com",
            "AccountId": sf_id("001", i) if i % 5 else None,
            "Birthdate": f"19{80 + i}-0{1 + i % 9}-15", "SystemModstamp": "2024-06-01T12:00:00.000+0000",
        })
    return {"User": users, "Account": accounts, "Contact": contacts}


DATA = build_data()


def describe(name):
    s = SCHEMA[name]
    return {"name": name, "label": s["label"], "queryable": True, "custom": False,
            "fields": s["fields"], "childRelationships": s["childRelationships"]}


def field_meta(obj, name):
    for fld in SCHEMA[obj]["fields"]:
        if fld["name"].lower() == name.lower():
            return fld
    return None


# ---------------------------------------------------------------- SOQL engine (subset)

class SoqlError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<str>'(?:\\.|[^'\\])*')
  | (?P<dt>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:?\d{2}))
  | (?P<date>\d{4}-\d{2}-\d{2})
  | (?P<time>\d{2}:\d{2}:\d{2}(?:\.\d+)?Z)
  | (?P<num>-?\d+(?:\.\d+)?)
  | (?P<op><=|>=|!=|=|<|>|\(|\)|,)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_.]*)
""", re.X)


def tokenize(s):
    pos, out = 0, []
    while pos < len(s):
        m = TOKEN_RE.match(s, pos)
        if not m:
            raise SoqlError("MALFORMED_QUERY", f"unexpected token at: {s[pos:pos+20]}")
        pos = m.end()
        kind = m.lastgroup
        if kind != "ws":
            out.append((kind, m.group()))
    return out


def unescape(lit):
    body = lit[1:-1]
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "r": "\r", "t": "\t"}.get(m.group(1), m.group(1)), body)


def resolve(record, obj, path):
    cur, cur_obj = record, obj
    parts = path.split(".")
    for i, p in enumerate(parts):
        if i < len(parts) - 1:
            rel = next((x for x in SCHEMA[cur_obj]["fields"]
                        if (x["relationshipName"] or "").lower() == p.lower()), None)
            if not rel:
                raise SoqlError("INVALID_FIELD", f"No such relation '{p}' on entity '{cur_obj}'")
            target = rel["referenceTo"][0]
            ref_id = cur.get(rel["name"])
            cur = next((r for r in DATA[target] if r["Id"] == ref_id), None)
            cur_obj = target
            if cur is None:
                return None, field_meta(target, parts[-1]) or {"type": "string"}
        else:
            meta = field_meta(cur_obj, p)
            if not meta or meta["type"] == "address":
                raise SoqlError("INVALID_FIELD", f"No such column '{p}' on entity '{cur_obj}'")
            return cur.get(meta["name"]), meta


class WhereEval:
    def __init__(self, tokens, obj):
        self.t, self.i, self.obj = tokens, 0, obj

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else (None, None)

    def kw(self, word):
        k, v = self.peek()
        if k == "ident" and v.upper() == word:
            self.i += 1
            return True
        return False

    def expect(self, value):
        k, v = self.peek()
        if v != value:
            raise SoqlError("MALFORMED_QUERY", f"expected {value}, got {v}")
        self.i += 1

    def parse(self):
        node = self.parse_or()
        if self.i != len(self.t):
            raise SoqlError("MALFORMED_QUERY", f"unexpected {self.peek()[1]}")
        return node

    def parse_or(self):
        left = self.parse_and()
        while self.kw("OR"):
            right = self.parse_and()
            left = ("or", left, right)
        return left

    def parse_and(self):
        left = self.parse_not()
        while self.kw("AND"):
            right = self.parse_not()
            left = ("and", left, right)
        return left

    def parse_not(self):
        if self.kw("NOT"):
            return ("not", self.parse_not())
        return self.parse_pred()

    def parse_value(self):
        k, v = self.peek()
        self.i += 1
        if k == "str":
            return ("str", unescape(v))
        if k in ("num", "date", "dt", "time"):
            return (k, v)
        if k == "ident" and v.lower() in ("true", "false", "null"):
            return (v.lower(), v.lower())
        raise SoqlError("MALFORMED_QUERY", f"expected literal, got {v}")

    def parse_pred(self):
        k, v = self.peek()
        if v == "(":
            self.i += 1
            node = self.parse_or()
            self.expect(")")
            return node
        if k != "ident":
            raise SoqlError("MALFORMED_QUERY", f"expected field, got {v}")
        self.i += 1
        field = v
        if self.kw("NOT"):
            if self.kw("IN"):
                return ("not", self.parse_in(field))
            raise SoqlError("MALFORMED_QUERY", "expected IN after NOT")
        if self.kw("IN"):
            return self.parse_in(field)
        if self.kw("LIKE"):
            return ("like", field, self.parse_value())
        k, op = self.peek()
        if k != "op" or op not in ("=", "!=", "<", "<=", ">", ">="):
            raise SoqlError("MALFORMED_QUERY", f"unexpected token {op}")
        self.i += 1
        return ("cmp", field, op, self.parse_value())

    def parse_in(self, field):
        self.expect("(")
        vals = [self.parse_value()]
        while self.peek()[1] == ",":
            self.i += 1
            vals.append(self.parse_value())
        self.expect(")")
        return ("in", field, vals)


def coerce(value, meta, lit):
    kind, text = lit
    t = meta["type"]
    if kind == "null":
        return None
    if t in ("int", "double", "currency", "percent"):
        if kind != "num":
            raise SoqlError("INVALID_QUERY_FILTER_OPERATOR",
                            f"value of filter criterion for field '{meta['name']}' must be numeric")
        return float(text)
    if t == "boolean":
        if kind not in ("true", "false"):
            raise SoqlError("INVALID_QUERY_FILTER_OPERATOR", f"boolean expected for {meta['name']}")
        return kind == "true"
    if t == "date":
        if kind != "date":
            raise SoqlError("INVALID_QUERY_FILTER_OPERATOR",
                            f"value of filter criterion for field '{meta['name']}' must be of type date "
                            "and should not be enclosed in quotes")
        return text
    if t == "datetime":
        if kind != "dt":
            raise SoqlError("INVALID_QUERY_FILTER_OPERATOR",
                            f"value of filter criterion for field '{meta['name']}' must be of type dateTime "
                            "and should not be enclosed in quotes")
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    if kind != "str":
        raise SoqlError("INVALID_QUERY_FILTER_OPERATOR",
                        f"value of filter criterion for field '{meta['name']}' must be a quoted string")
    return text


def norm(value, meta):
    if value is None:
        return None
    t = meta["type"]
    if t in ("int", "double", "currency", "percent"):
        return float(value)
    if t == "datetime":
        return datetime.strptime(value, "%Y-%m-%dT%H:%M:%S.000+0000").replace(tzinfo=timezone.utc)
    return value


def evaluate(node, record, obj):
    op = node[0]
    if op == "and":
        return evaluate(node[1], record, obj) and evaluate(node[2], record, obj)
    if op == "or":
        return evaluate(node[1], record, obj) or evaluate(node[2], record, obj)
    if op == "not":
        return not evaluate(node[1], record, obj)
    raw, meta = resolve(record, obj, node[1])
    if not meta.get("filterable", True):
        raise SoqlError("INVALID_FIELD", f"field '{meta['name']}' can not be filtered in a query call")
    val = norm(raw, meta)
    if op == "cmp":
        target = coerce(raw, meta, node[3])
        cmp = node[2]
        if target is None:
            return (val is None) if cmp == "=" else (val is not None)
        if val is None:
            return cmp == "!="
        if isinstance(val, str) and isinstance(target, str):
            val, target = val.lower(), target.lower()
        return {"=": val == target, "!=": val != target, "<": val < target, "<=": val <= target,
                ">": val > target, ">=": val >= target}[cmp]
    if op == "in":
        targets = [coerce(raw, meta, v) for v in node[2]]
        if isinstance(val, str):
            return val.lower() in [str(t).lower() for t in targets]
        return val in targets
    if op == "like":
        if meta["type"] not in ("string", "picklist", "email", "textarea", "id", "reference"):
            raise SoqlError("INVALID_QUERY_FILTER_OPERATOR", "LIKE only valid on text fields")
        pattern = "^" + re.escape(node[2][1]).replace("%", ".*").replace("_", ".") + "$"
        return val is not None and re.match(pattern, val, re.I | re.S) is not None
    raise SoqlError("MALFORMED_QUERY", "bad node")


SOQL_RE = re.compile(
    r"^SELECT\s+(?P<fields>.+?)\s+FROM\s+(?P<obj>\w+)"
    r"(?:\s+WHERE\s+(?P<where>.+?))?"
    r"(?:\s+ORDER\s+BY\s+(?P<order>.+?))?"
    r"(?:\s+LIMIT\s+(?P<limit>\d+))?"
    r"(?:\s+OFFSET\s+(?P<offset>\d+))?\s*$", re.I | re.S)


def run_query(soql):
    m = SOQL_RE.match(soql)
    if not m:
        raise SoqlError("MALFORMED_QUERY", f"unexpected token in: {soql[:60]}")
    obj_name = next((o for o in SCHEMA if o.lower() == m["obj"].lower()), None)
    if not obj_name:
        raise SoqlError("INVALID_TYPE", f"sObject type '{m['obj']}' is not supported.")
    fields = [x.strip() for x in m["fields"].split(",")]
    rows = list(DATA[obj_name])
    if m["where"]:
        tree = WhereEval(tokenize(m["where"]), obj_name).parse()
        rows = [r for r in rows if evaluate(tree, r, obj_name)]
    if m["order"]:
        for key in reversed([k.strip() for k in m["order"].split(",")]):
            parts = key.split()
            desc = len(parts) > 1 and parts[1].upper() == "DESC"
            nulls_last = "LAST" in key.upper() if "NULLS" in key.upper() else desc

            def sort_key(r, fld=parts[0], nl=nulls_last):
                v, meta = resolve(r, obj_name, fld)
                v = norm(v, meta)
                return (v is None) != nl, v if v is not None else 0
            rows.sort(key=sort_key, reverse=desc)
    if m["offset"]:
        rows = rows[int(m["offset"]):]
    if m["limit"]:
        rows = rows[:int(m["limit"])]
    if len(fields) == 1 and fields[0].upper() == "COUNT()":
        return {"totalSize": len(rows), "done": True, "records": []}
    out = []
    for r in rows:
        rec = {"attributes": {"type": obj_name, "url": f"/services/data/{API}/sobjects/{obj_name}/{r['Id']}"}}
        for fld in fields:
            if "." in fld:
                parts = fld.split(".")
                val, _ = resolve(r, obj_name, fld)
                # build nested object, null parent when lookup is empty
                rel = next(x for x in SCHEMA[obj_name]["fields"]
                           if (x["relationshipName"] or "").lower() == parts[0].lower())
                parent = next((p for p in DATA[rel["referenceTo"][0]] if p["Id"] == r.get(rel["name"])), None)
                if parent is None:
                    rec[rel["relationshipName"]] = None
                else:
                    node = rec.setdefault(rel["relationshipName"],
                                          {"attributes": {"type": rel["referenceTo"][0]}})
                    node[parts[-1]] = val
            else:
                val, meta = resolve(r, obj_name, fld)
                rec[meta["name"]] = val
        out.append(rec)
    return {"totalSize": len(out), "done": True, "records": out}


# ---------------------------------------------------------------- JWT

def verify_jwt(assertion):
    try:
        header_b64, claims_b64, sig_b64 = assertion.split(".")

        def pad(s):
            return s + "=" * (-len(s) % 4)
        header = json.loads(base64.urlsafe_b64decode(pad(header_b64)))
        claims = json.loads(base64.urlsafe_b64decode(pad(claims_b64)))
        if header.get("alg") != "RS256":
            return None, "alg must be RS256"
        for k in ("iss", "sub", "aud", "exp"):
            if k not in claims:
                return None, f"missing claim {k}"
        if claims["exp"] < datetime.now(timezone.utc).timestamp():
            return None, "expired"
        if PUBLIC_KEY_FILE:
            from cryptography.hazmat.primitives import hashes, serialization
            from cryptography.hazmat.primitives.asymmetric import padding
            with open(PUBLIC_KEY_FILE, "rb") as fh:
                key = serialization.load_pem_public_key(fh.read())
            key.verify(base64.urlsafe_b64decode(pad(sig_b64)), f"{header_b64}.{claims_b64}".encode(),
                       padding.PKCS1v15(), hashes.SHA256())
        return claims, None
    except Exception as exc:  # noqa: BLE001
        return None, f"invalid assertion: {exc}"


# ---------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        if os.environ.get("MOCK_VERBOSE"):
            super().log_message(*args)

    def send_json(self, status, body, extra=None):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json;charset=UTF-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Sforce-Limit-Info",
                         f"api-usage={STATE['query_requests'] + STATE['describe_requests']}/15000")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n).decode() if n else ""

    def authorized(self):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer ") or auth[7:] not in VALID_TOKENS:
            self.send_json(401, [{"message": "Session expired or invalid", "errorCode": "INVALID_SESSION_ID"}])
            return False
        return True

    def injected_failure(self):
        with LOCK:
            if STATE["fail_count"] > 0:
                STATE["fail_count"] -= 1
                status = STATE["fail_status"]
            else:
                return False
        if status == 401:
            VALID_TOKENS.clear()  # expire every session
            self.send_json(401, [{"message": "Session expired or invalid", "errorCode": "INVALID_SESSION_ID"}])
        else:
            self.send_json(status, [{"message": "Server Unavailable", "errorCode": "SERVER_UNAVAILABLE"}],
                           {"Retry-After": "0"} if status == 429 else None)
        return True

    # ---- routes
    def do_POST(self):
        url = urlparse(self.path)
        if url.path == "/__reset":
            with LOCK:
                STATE.update({"soql": [], "token_requests": 0, "query_requests": 0, "describe_requests": 0,
                              "batch_requests": 0, "fail_status": None, "fail_count": 0})
            return self.send_json(200, {"ok": True})
        if url.path == "/__fail":
            q = parse_qs(url.query)
            with LOCK:
                STATE["fail_status"] = int(q.get("status", ["503"])[0])
                STATE["fail_count"] = int(q.get("count", ["1"])[0])
            return self.send_json(200, {"ok": True})
        if url.path == "/services/oauth2/token":
            return self.token()
        if url.path == f"/services/data/{API}/composite/batch":
            if self.injected_failure() or not self.authorized():
                return
            req = json.loads(self.body())
            STATE["batch_requests"] += 1
            results = []
            for sub in req["batchRequests"]:
                m = re.match(rf"{API}/sobjects/(\w+)/describe", sub["url"])
                if m and m.group(1) in SCHEMA:
                    results.append({"statusCode": 200, "result": describe(m.group(1))})
                else:
                    results.append({"statusCode": 404,
                                    "result": [{"errorCode": "NOT_FOUND", "message": "not found"}]})
            return self.send_json(200, {"hasErrors": any(r["statusCode"] != 200 for r in results),
                                        "results": results})
        self.send_json(404, [{"errorCode": "NOT_FOUND", "message": self.path}])

    def token(self):
        form = {k: v[0] for k, v in parse_qs(self.body()).items()}
        STATE["token_requests"] += 1
        grant = form.get("grant_type")
        ok, err = False, "unsupported_grant_type"
        if grant == "urn:ietf:params:oauth:grant-type:jwt-bearer":
            claims, problem = verify_jwt(form.get("assertion", ""))
            ok = claims is not None and claims["sub"] == "dean@example.com"
            err = problem or "user hasn't approved this consumer"
        elif grant == "refresh_token":
            ok = form.get("refresh_token") == "good-refresh"
            err = "expired access/refresh token"
        elif grant == "password":
            ok = form.get("username") == "dean@example.com" and form.get("password") == "secretTOKEN123"
            err = "authentication failure"
        elif grant == "client_credentials":
            ok = form.get("client_id") == "cid" and form.get("client_secret") == "csecret"
            err = "invalid client credentials"
        if not ok:
            return self.send_json(400, {"error": "invalid_grant", "error_description": err})
        token = f"00DMOCK!{os.urandom(8).hex()}"
        VALID_TOKENS.add(token)
        host = self.headers.get("Host")
        self.send_json(200, {"access_token": token, "instance_url": f"http://{host}", "token_type": "Bearer",
                             "id": f"http://{host}/id/00D/005000000000000001"})

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/__soql":
            return self.send_json(200, STATE["soql"])
        if url.path == "/__stats":
            return self.send_json(200, {k: v for k, v in STATE.items() if k not in ("soql", "cursors")})
        if self.injected_failure() or not self.authorized():
            return
        base = f"/services/data/{API}"
        if url.path == f"{base}/sobjects":
            STATE["describe_requests"] += 1
            objs = [{"name": n, "label": s["label"], "queryable": True, "custom": False}
                    for n, s in SCHEMA.items()]
            objs += [{"name": n, "label": n, "queryable": False, "custom": False} for n in NON_QUERYABLE]
            return self.send_json(200, {"encoding": "UTF-8", "maxBatchSize": 200, "sobjects": objs})
        m = re.match(rf"{base}/sobjects/(\w+)/describe$", url.path)
        if m:
            STATE["describe_requests"] += 1
            if m.group(1) not in SCHEMA:
                return self.send_json(404, [{"errorCode": "NOT_FOUND",
                                             "message": "The requested resource does not exist"}])
            return self.send_json(200, describe(m.group(1)))
        if url.path == f"{base}/query":
            STATE["query_requests"] += 1
            soql = parse_qs(url.query).get("q", [""])[0]
            STATE["soql"].append(soql)
            try:
                result = run_query(soql)
            except SoqlError as e:
                return self.send_json(400, [{"errorCode": e.code, "message": str(e)}])
            return self.send_json(200, self.page(result["records"], result["totalSize"], 0))
        m = re.match(rf"{base}/query/(\w+)-(\d+)$", url.path)
        if m:
            STATE["query_requests"] += 1
            records = STATE["cursors"].get(m.group(1))
            if records is None:
                return self.send_json(400, [{"errorCode": "INVALID_QUERY_LOCATOR",
                                             "message": "invalid query locator"}])
            return self.send_json(200, self.page(records, len(records), int(m.group(2))))
        self.send_json(404, [{"errorCode": "NOT_FOUND", "message": self.path}])

    def page(self, records, total, start):
        chunk = records[start:start + PAGE_SIZE]
        body = {"totalSize": total, "done": start + PAGE_SIZE >= len(records), "records": chunk}
        if not body["done"]:
            locator = os.urandom(6).hex()
            STATE["cursors"][locator] = records
            body["nextRecordsUrl"] = f"/services/data/{API}/query/{locator}-{start + PAGE_SIZE}"
        return body


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"mock salesforce on http://127.0.0.1:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
