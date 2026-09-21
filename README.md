# Salesforce ODBC Driver — Eloquix Labs

A commercial-grade ODBC 3.8 driver for Salesforce, written in C++ with no SDK royalties.
Runs on Windows, macOS and Linux. **This release (Phase 1) is read-only.**

The driver translates a useful subset of SQL into SOQL and pushes filters, ordering and
row limits down to Salesforce, so BI tools transfer only the rows they asked for.

---

## Status

| Area | State |
|---|---|
| Read queries against a single object | Working |
| Relationship fields (`Account.Owner.Name`) | Working |
| Catalog functions, cached metadata | Working |
| OAuth: JWT bearer, refresh token, client credentials, access token, password | Working |
| Unicode (W) entry points | Working |
| Block fetching, chunked `SQLGetData`, parameters | Working |
| Writes (INSERT/UPDATE/DELETE) | Phase 4 |
| Joins, GROUP BY, subqueries | Later phase (local execution engine) |
| DSN configuration dialog | Not yet — configure via connection string or `odbc.ini` |

Verified with 137 Python tests, 561 low-level ODBC conformance checks, and a clean
valgrind run against a mock Salesforce server. Not yet verified against a live org or
against Excel, Power BI, Tableau, SSIS or Alteryx.

---

## Building

Requirements: CMake 3.16+, a C++17 compiler, libcurl, OpenSSL, nlohmann-json,
and an ODBC driver manager (unixODBC on Linux, iODBC or unixODBC on macOS).

```bash
# Debian / Ubuntu
sudo apt-get install cmake g++ unixodbc-dev libcurl4-openssl-dev libssl-dev nlohmann-json3-dev

# macOS
brew install cmake unixodbc curl openssl nlohmann-json

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The result is `build/libsfodbc.so` (Linux), `build/libsfodbc.dylib` (macOS) or
`build/EloquixSalesforceODBC.dll` (Windows).

On Windows, build both 32-bit and 64-bit: 32-bit Excel is still common.

```powershell
cmake -S . -B build64 -A x64   ; cmake --build build64 --config Release
cmake -S . -B build32 -A Win32 ; cmake --build build32 --config Release
```

---

## Installing

```bash
sudo packaging/linux/install.sh build/libsfodbc.so     # Linux
sudo packaging/macos/install.sh build/libsfodbc.dylib  # macOS
powershell -File packaging\windows\register.ps1        # Windows (as Administrator)
```

Each script registers the driver with the local driver manager. To do it by hand, add to
`odbcinst.ini`:

```ini
[Eloquix Salesforce ODBC Driver]
Description = Salesforce ODBC driver (Eloquix Labs)
Driver      = /usr/local/lib/libsfodbc.so
Threading   = 1
```

---

## Connecting

### JWT bearer (recommended for servers and scheduled refreshes)

No password, no interactive login, and it survives password rotation — the right choice
for a Power BI gateway or an SSIS job.

```
Driver={Eloquix Salesforce ODBC Driver};
AuthType=JWT;
LoginUrl=https://login.salesforce.com;
ClientId=<consumer key of your External Client App>;
Username=integration.user@example.com;
PrivateKeyFile=/etc/eloquix/salesforce.key
```

Set `LoginUrl=https://test.salesforce.com` for a sandbox. The certificate matching the
private key must be uploaded to the connected app, and the user must be pre-authorised.

### Refresh token

```
Driver={Eloquix Salesforce ODBC Driver};AuthType=RefreshToken;
ClientId=...;ClientSecret=...;RefreshToken=...
```

### Client credentials

```
Driver={Eloquix Salesforce ODBC Driver};AuthType=ClientCredentials;
LoginUrl=https://yourdomain.my.salesforce.com;ClientId=...;ClientSecret=...
```

### Username and password

Supported, but Salesforce is retiring this flow. Append the security token, or set it
separately with `SecurityToken=`.

```
Driver={Eloquix Salesforce ODBC Driver};AuthType=Password;
ClientId=...;ClientSecret=...;UID=user@example.com;PWD=...;SecurityToken=...
```

### All connection options

| Key | Default | Meaning |
|---|---|---|
| `AuthType` | inferred | `JWT`, `RefreshToken`, `ClientCredentials`, `AccessToken`, `Password` |
| `LoginUrl` | `https://login.salesforce.com` | Login or My Domain URL |
| `InstanceUrl` | from login | Required only with `AuthType=AccessToken` |
| `ClientId`, `ClientSecret` | | Connected app credentials |
| `Username` / `UID` | | Salesforce user |
| `Password` / `PWD`, `SecurityToken` | | Password flow |
| `RefreshToken`, `AccessToken` | | Token flows |
| `PrivateKeyFile`, `PrivateKeyPassword` | | JWT signing key (PEM) |
| `JwtAudience` | login host | Override the JWT `aud` claim |
| `ApiVersion` | `64.0` | Salesforce REST API version |
| `BatchSize` | `2000` | Rows per query page (200–2000) |
| `Timeout` | `120` | HTTP timeout, seconds |
| `MaxRetries` | `4` | Retries for 429/5xx and transport errors |
| `MetadataCacheTTL` | `3600` | Seconds to cache object metadata |
| `IncludeNonQueryable` | `false` | Also list objects Salesforce marks non-queryable |
| `VerifySSL`, `CABundle` | `true` | TLS verification |
| `ProxyUrl`, `ProxyUser`, `ProxyPassword` | | HTTP proxy |
| `LogFile`, `LogLevel` | off | `1` error, `2` info, `3` debug (logs SOQL) |

Secrets are redacted in the log and never written back into the output connection string.

---

## Supported SQL

```sql
SELECT [TOP n] * | column [AS alias], Relationship.Field, COUNT(*)
FROM Object [alias]
[WHERE conditions]
[ORDER BY column [ASC|DESC] [NULLS FIRST|LAST], ...]
[LIMIT n [OFFSET m]] | [OFFSET m ROWS] | [FETCH FIRST n ROWS ONLY]
```

Conditions: `= != <> < <= > >=`, `LIKE`, `IN`, `NOT IN`, `BETWEEN`, `IS [NOT] NULL`,
`AND`, `OR`, `NOT`, parentheses, `?` parameters, and ODBC escapes `{d '...'}`,
`{t '...'}`, `{ts '...'}`.

Identifiers may be quoted with `"…"`, `[…]` or `` `…` ``, and are matched
case-insensitively against the org's API names.

Everything is pushed to Salesforce. Nothing is filtered locally, so `LIMIT 10` on a
million-row object transfers ten rows.

**Not supported in this release** — each fails immediately with a clear message rather
than silently returning wrong results: joins, subqueries, `GROUP BY`, `HAVING`, `UNION`,
`DISTINCT`, scalar functions, arithmetic, and any write statement.

### Useful behaviours

- `SELECT ... WHERE 1=0`, which Power BI and Tableau use to probe columns, returns the
  column list without calling Salesforce at all.
- `SELECT 1` works without a `FROM`, so connection tests succeed.
- `SQLNativeSql` returns the SOQL a query would send — the quickest way to see what is
  being pushed down.

### Type mapping

| Salesforce | ODBC |
|---|---|
| id, reference | `SQL_WVARCHAR(18)` |
| string, picklist, email, phone, url | `SQL_WVARCHAR(length)` |
| textarea, long text > 4000 | `SQL_WLONGVARCHAR` |
| boolean | `SQL_BIT` |
| int | `SQL_INTEGER` / `SQL_BIGINT` |
| double, currency, percent | `SQL_DECIMAL(precision, scale)` |
| date / datetime / time | `SQL_TYPE_DATE` / `_TIMESTAMP` / `_TIME` |

Datetimes are returned in UTC. Compound fields (`BillingAddress`, geolocations) are
hidden in favour of their components (`BillingCity`, …), which is what you can actually
query in SOQL.

---

## Testing

```bash
pip install pyodbc pytest
cmake --build build -j
gcc -o build/odbc_conformance tests/odbc_conformance.c -lodbc
python3 -m pytest tests/ -q
```

Tests run against `tests/mock_salesforce.py`, a mock org that implements the OAuth
endpoints, describe calls, Composite Batch, query/queryMore paging, a small SOQL
evaluator, and fault injection for 429/503/expired sessions. No Salesforce org needed.

Before a release, also run the suite against a real developer org by setting a live
connection string, and capture ODBC traces from each client tool to catch calls the
mock does not make.

---

## Roadmap

1. **Phase 1 (this release)** — read-only core.
2. **Phase 2** — Excel and Power BI Import: 32- and 64-bit builds, signed installers,
   gateway support, DSN dialog, license activation.
3. **Phase 3** — Power Query custom connector (`.mez`, DirectQuery, MS certification)
   and a Tableau `.taco`.
4. **Phase 4** — write-back for SSIS and Alteryx via parameter arrays mapped to
   sObject Collections and Bulk API 2.0, with per-row error reporting.
5. **Later** — a local execution engine (DuckDB or Calcite) for joins and aggregates,
   and Bulk API 2.0 for very large extracts.

## Known limitations

- Very wide `SELECT *` queries on objects with hundreds of fields can exceed the REST
  URL limit; the driver reports this clearly. Bulk API 2.0 in a later phase removes it.
- Forward-only cursors. Tools that ask to scroll backwards get `HY106`.
- No browser-based login flow yet; JWT or a pre-issued refresh token covers servers.
- The Windows and macOS builds compile from the same source but have not been exercised
  on those platforms here.

## Naming

Use "for Salesforce", never "Salesforce ODBC Driver" as a product name, to stay clear of
trademark trouble. Salesforce is a trademark of Salesforce, Inc.; Eloquix Labs is not
affiliated with or endorsed by Salesforce.
