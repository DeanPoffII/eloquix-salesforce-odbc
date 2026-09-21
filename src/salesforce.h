#pragma once
#include "config.h"
#include "http.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>

namespace sf {

using json = nlohmann::json;

enum class AuthType { JwtBearer, RefreshToken, ClientCredentials, AccessToken, Password };

// Salesforce REST client: OAuth, retries with backoff, session refresh, API-limit tracking.
class SalesforceClient {
 public:
  explicit SalesforceClient(const ConnConfig& cfg);

  void authenticate();

  json get(const std::string& path_or_url);
  json post(const std::string& path, const json& body);

  json describe_global();
  json describe_sobject(const std::string& name);
  // Uses the Composite Batch API (25 requests per call) to describe many objects cheaply.
  std::vector<std::pair<std::string, json>> describe_many(const std::vector<std::string>& names);

  json query(const std::string& soql);
  json query_more(const std::string& next_records_url);

  const std::string& instance_url() const { return instance_url_; }
  const std::string& api_version() const { return api_version_; }
  std::string username() const { return cfg_.get("Username"); }
  std::string cache_key() const;  // identifies org + user for shared metadata cache

  // From the Sforce-Limit-Info header, e.g. "api-usage=1234/15000"
  long long api_used() const { return api_used_; }
  long long api_max() const { return api_max_; }

 private:
  enum class Body { None, Json };
  json request(const std::string& method, const std::string& path_or_url, const std::string& body,
               bool is_query);
  std::string resolve_url(const std::string& path_or_url) const;
  std::string api_base() const { return "/services/data/v" + api_version_; }
  void track_limits(const HttpResponse& r);
  [[noreturn]] void raise_api_error(const HttpResponse& r) const;

  std::string token_request(const std::string& form_body);
  std::string build_jwt_assertion() const;

  ConnConfig cfg_;
  AuthType auth_type_;
  std::unique_ptr<HttpClient> http_;
  std::string login_url_;
  std::string instance_url_;
  std::string access_token_;
  std::string api_version_;
  int max_retries_;
  int batch_size_;
  long long api_used_ = -1;
  long long api_max_ = -1;
};

AuthType parse_auth_type(const ConnConfig& cfg);

}  // namespace sf
