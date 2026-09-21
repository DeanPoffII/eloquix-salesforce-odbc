#include "salesforce.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <random>
#include <thread>

namespace sf {

AuthType parse_auth_type(const ConnConfig& cfg) {
  std::string t = to_lower(cfg.get("AuthType"));
  if (t.empty()) {
    // Infer from supplied credentials, most secure first.
    if (cfg.has("PrivateKeyFile")) return AuthType::JwtBearer;
    if (cfg.has("RefreshToken")) return AuthType::RefreshToken;
    if (cfg.has("AccessToken")) return AuthType::AccessToken;
    if (cfg.has("Password")) return AuthType::Password;
    if (cfg.has("ClientSecret")) return AuthType::ClientCredentials;
    throw OdbcError("28000", "No credentials supplied. Set AuthType and the matching credential keys.");
  }
  if (t == "jwt" || t == "jwtbearer") return AuthType::JwtBearer;
  if (t == "refreshtoken") return AuthType::RefreshToken;
  if (t == "clientcredentials") return AuthType::ClientCredentials;
  if (t == "accesstoken") return AuthType::AccessToken;
  if (t == "password") return AuthType::Password;
  throw OdbcError("HY024", "Unknown AuthType '" + cfg.get("AuthType") +
                               "'. Use JWT, RefreshToken, ClientCredentials, AccessToken or Password.");
}

static std::string strip_slash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

SalesforceClient::SalesforceClient(const ConnConfig& cfg) : cfg_(cfg) {
  auth_type_ = parse_auth_type(cfg_);
  HttpOptions o;
  o.timeout_seconds = static_cast<long>(cfg_.get_int("Timeout", 120));
  o.verify_ssl = cfg_.get_bool("VerifySSL", true);
  o.ca_bundle = cfg_.get("CABundle");
  o.proxy_url = cfg_.get("ProxyUrl");
  o.proxy_user = cfg_.get("ProxyUser");
  o.proxy_password = cfg_.get("ProxyPassword");
  http_ = std::make_unique<HttpClient>(o);

  login_url_ = strip_slash(cfg_.get("LoginUrl", "https://login.salesforce.com"));
  api_version_ = cfg_.get("ApiVersion", SFODBC_DEFAULT_API);
  if (starts_with_ci(api_version_, "v")) api_version_ = api_version_.substr(1);
  max_retries_ = static_cast<int>(cfg_.get_int("MaxRetries", 4));
  batch_size_ = static_cast<int>(std::clamp<long long>(cfg_.get_int("BatchSize", 2000), 200, 2000));
  require_secure_url(login_url_);
}

std::string SalesforceClient::cache_key() const {
  std::string who = cfg_.get("Username", cfg_.get("ClientId"));
  if (who.empty()) who = std::to_string(std::hash<std::string>{}(access_token_));
  return to_lower(instance_url_) + "|" + to_lower(who) + "|" + api_version_;
}

// ---------------------------------------------------------------- OAuth

std::string SalesforceClient::build_jwt_assertion() const {
  std::string key_file = cfg_.get("PrivateKeyFile");
  std::string client_id = cfg_.get("ClientId");
  std::string user = cfg_.get("Username");
  if (client_id.empty() || user.empty())
    throw OdbcError("28000", "JWT authentication requires ClientId, Username and PrivateKeyFile.");

  // Audience is login.salesforce.com / test.salesforce.com, not the My Domain URL.
  std::string aud = cfg_.get("JwtAudience");
  if (aud.empty()) aud = login_url_.find("test.salesforce.com") != std::string::npos
                             ? "https://test.salesforce.com"
                             : "https://login.salesforce.com";

  long long exp = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count() + 180;
  json claims = {{"iss", client_id}, {"sub", user}, {"aud", aud}, {"exp", exp}};
  std::string signing_input = base64url(R"({"alg":"RS256"})") + "." + base64url(claims.dump());

  BIO* bio = BIO_new_file(key_file.c_str(), "r");
  if (!bio) throw OdbcError("28000", "Cannot open PrivateKeyFile: " + key_file);
  std::string pass = cfg_.get("PrivateKeyPassword");
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr,
                                           pass.empty() ? nullptr : const_cast<char*>(pass.c_str()));
  BIO_free(bio);
  if (!pkey) throw OdbcError("28000", "PrivateKeyFile is not a readable PEM private key (wrong password?)");

  std::string sig;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  size_t len = 0;
  bool ok = ctx && EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
            EVP_DigestSignUpdate(ctx, signing_input.data(), signing_input.size()) == 1 &&
            EVP_DigestSignFinal(ctx, nullptr, &len) == 1;
  if (ok) {
    sig.resize(len);
    ok = EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(&sig[0]), &len) == 1;
    sig.resize(len);
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  if (!ok) throw OdbcError("28000", "Failed to sign JWT assertion with the supplied private key");
  return signing_input + "." + base64url(sig);
}

std::string SalesforceClient::token_request(const std::string& form) {
  std::string url = login_url_ + "/services/oauth2/token";
  HttpResponse r;
  for (int attempt = 0;; ++attempt) {
    r = http_->send("POST", url, {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json"},
                    form);
    if (r.status >= 500 && attempt < 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
      continue;
    }
    break;
  }
  json j = json::parse(r.body, nullptr, false);
  if (r.status != 200 || j.is_discarded() || !j.contains("access_token")) {
    std::string msg = "Salesforce login failed (HTTP " + std::to_string(r.status) + ")";
    if (!j.is_discarded() && j.is_object()) {
      msg += ": " + j.value("error", std::string()) + " - " + j.value("error_description", std::string());
    }
    throw OdbcError("28000", msg, static_cast<int>(r.status));
  }
  instance_url_ = strip_slash(j.value("instance_url", login_url_));
  return j["access_token"].get<std::string>();
}

void SalesforceClient::authenticate() {
  std::string cid = url_encode(cfg_.get("ClientId"));
  std::string secret = url_encode(cfg_.get("ClientSecret"));
  switch (auth_type_) {
    case AuthType::AccessToken:
      access_token_ = cfg_.get("AccessToken");
      instance_url_ = strip_slash(cfg_.get("InstanceUrl"));
      if (access_token_.empty() || instance_url_.empty())
        throw OdbcError("28000", "AccessToken authentication requires AccessToken and InstanceUrl.");
      break;
    case AuthType::JwtBearer:
      access_token_ = token_request("grant_type=" + url_encode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
                                    "&assertion=" + build_jwt_assertion());
      break;
    case AuthType::RefreshToken: {
      if (cfg_.get("ClientId").empty() || !cfg_.has("RefreshToken"))
        throw OdbcError("28000", "RefreshToken authentication requires ClientId and RefreshToken.");
      std::string form = "grant_type=refresh_token&client_id=" + cid +
                         "&refresh_token=" + url_encode(cfg_.get("RefreshToken"));
      if (!secret.empty()) form += "&client_secret=" + secret;
      access_token_ = token_request(form);
      break;
    }
    case AuthType::ClientCredentials:
      if (cid.empty() || secret.empty())
        throw OdbcError("28000", "ClientCredentials authentication requires ClientId and ClientSecret "
                                 "(and LoginUrl set to your My Domain URL).");
      access_token_ = token_request("grant_type=client_credentials&client_id=" + cid + "&client_secret=" + secret);
      break;
    case AuthType::Password:
      LOG_INFO("Using legacy username-password OAuth flow; Salesforce is retiring this flow.");
      access_token_ = token_request("grant_type=password&client_id=" + cid + "&client_secret=" + secret +
                                    "&username=" + url_encode(cfg_.get("Username")) + "&password=" +
                                    url_encode(cfg_.get("Password") + cfg_.get("SecurityToken")));
      break;
  }
  require_secure_url(instance_url_);
  LOG_INFO("Authenticated to " << instance_url_ << " (API v" << api_version_ << ")");
}

// ---------------------------------------------------------------- REST

std::string SalesforceClient::resolve_url(const std::string& p) const {
  if (starts_with_ci(p, "https://") || starts_with_ci(p, "http://")) return p;
  return instance_url_ + p;
}

void SalesforceClient::track_limits(const HttpResponse& r) {
  auto it = r.headers.find("Sforce-Limit-Info");
  if (it == r.headers.end()) return;
  size_t eq = it->second.find("api-usage=");
  if (eq == std::string::npos) return;
  std::string v = it->second.substr(eq + 10);
  size_t slash = v.find('/');
  if (slash == std::string::npos) return;
  try {
    api_used_ = std::stoll(v.substr(0, slash));
    api_max_ = std::stoll(v.substr(slash + 1));
  } catch (...) {
  }
}

void SalesforceClient::raise_api_error(const HttpResponse& r) const {
  std::string code, message;
  json j = json::parse(r.body, nullptr, false);
  if (!j.is_discarded()) {
    const json& e = j.is_array() && !j.empty() ? j[0] : j;
    if (e.is_object()) {
      code = e.value("errorCode", e.value("error", std::string()));
      message = e.value("message", e.value("error_description", std::string()));
    }
  }
  if (message.empty()) message = r.body.substr(0, 500);
  std::string state = "HY000";
  if (code == "INVALID_FIELD") state = "42S22";
  else if (code == "INVALID_TYPE" || code == "NOT_FOUND") state = "42S02";
  else if (code == "MALFORMED_QUERY" || code == "INVALID_QUERY_FILTER_OPERATOR" || code == "INSUFFICIENT_ACCESS")
    state = "42000";
  else if (code == "INVALID_SESSION_ID" || code == "API_DISABLED_FOR_ORG") state = "28000";
  else if (code == "QUERY_TIMEOUT") state = "HYT00";
  else if (code == "REQUEST_LIMIT_EXCEEDED")
    message = "Salesforce API request limit exceeded for this org. " + message;
  throw OdbcError(state, (code.empty() ? "" : code + ": ") + message, static_cast<int>(r.status));
}

json SalesforceClient::request(const std::string& method, const std::string& path, const std::string& body,
                               bool is_query) {
  std::string url = resolve_url(path);
  if (url.size() > 16000)
    throw OdbcError("42000", "Query is too long for the REST API (select fewer columns instead of *).");
  bool reauthed = false;
  std::mt19937 rng{std::random_device{}()};

  for (int attempt = 0;; ++attempt) {
    std::vector<std::string> headers = {"Authorization: Bearer " + access_token_, "Accept: application/json"};
    if (!body.empty()) headers.push_back("Content-Type: application/json");
    if (is_query) headers.push_back("Sforce-Query-Options: batchSize=" + std::to_string(batch_size_));

    auto backoff = [&](long retry_after_s) {
      long ms = retry_after_s > 0 ? retry_after_s * 1000 : std::min(30000L, 500L << std::min(attempt, 6));
      ms += std::uniform_int_distribution<long>(0, 250)(rng);
      LOG_INFO("Retrying " << method << " in " << ms << "ms (attempt " << attempt + 1 << ")");
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };

    HttpResponse r;
    try {
      LOG_DEBUG(method << " " << url);
      r = http_->send(method, url, headers, body);
    } catch (const OdbcError& e) {
      if (attempt < max_retries_ && HttpClient::is_retryable_transport(e.native())) {
        backoff(0);
        continue;
      }
      throw;
    }
    track_limits(r);

    if (r.status >= 200 && r.status < 300) {
      if (r.body.empty()) return json();
      json j = json::parse(r.body, nullptr, false);
      if (j.is_discarded()) throw OdbcError("HY000", "Salesforce returned an unparseable response");
      return j;
    }
    if (r.status == 401 && !reauthed && auth_type_ != AuthType::AccessToken) {
      LOG_INFO("Session expired; re-authenticating");
      authenticate();
      reauthed = true;
      --attempt;
      continue;
    }
    if ((r.status == 429 || r.status == 502 || r.status == 503 || r.status == 504) && attempt < max_retries_) {
      long retry_after = 0;
      auto it = r.headers.find("Retry-After");
      if (it != r.headers.end()) try { retry_after = std::stol(it->second); } catch (...) {}
      backoff(retry_after);
      continue;
    }
    raise_api_error(r);
  }
}

json SalesforceClient::get(const std::string& p) { return request("GET", p, "", false); }
json SalesforceClient::post(const std::string& p, const json& b) { return request("POST", p, b.dump(), false); }

json SalesforceClient::describe_global() { return get(api_base() + "/sobjects"); }

json SalesforceClient::describe_sobject(const std::string& name) {
  return get(api_base() + "/sobjects/" + url_encode(name) + "/describe");
}

std::vector<std::pair<std::string, json>> SalesforceClient::describe_many(const std::vector<std::string>& names) {
  std::vector<std::pair<std::string, json>> out;
  for (size_t i = 0; i < names.size(); i += 25) {
    size_t end = std::min(names.size(), i + 25);
    if (end - i == 1) {
      out.emplace_back(names[i], describe_sobject(names[i]));
      continue;
    }
    json reqs = json::array();
    for (size_t k = i; k < end; ++k)
      reqs.push_back({{"method", "GET"}, {"url", "v" + api_version_ + "/sobjects/" + names[k] + "/describe"}});
    json resp = post(api_base() + "/composite/batch", {{"batchRequests", reqs}});
    const json& results = resp["results"];
    for (size_t k = i; k < end; ++k) {
      const json& r = results.at(k - i);
      if (r.value("statusCode", 0) == 200) out.emplace_back(names[k], r["result"]);
      else LOG_ERROR("Describe failed for " << names[k] << ": " << r.dump());
    }
  }
  return out;
}

// Query strings: keep commas and parentheses readable to stay under the URI limit.
static std::string encode_query(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || std::strchr("-_.~,()*:!", c)) out += static_cast<char>(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

json SalesforceClient::query(const std::string& soql) {
  LOG_INFO("SOQL: " << soql);
  return request("GET", api_base() + "/query?q=" + encode_query(soql), "", true);
}

json SalesforceClient::query_more(const std::string& next_url) { return request("GET", next_url, "", true); }

}  // namespace sf
