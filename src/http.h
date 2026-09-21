#pragma once
#include "util.h"

#include <map>

namespace sf {

struct HttpOptions {
  long timeout_seconds = 120;
  long connect_timeout_seconds = 30;
  bool verify_ssl = true;
  std::string ca_bundle;
  std::string proxy_url;
  std::string proxy_user;
  std::string proxy_password;
};

struct HttpResponse {
  long status = 0;
  std::string body;
  std::map<std::string, std::string, CiLess> headers;
};

// Thin libcurl wrapper. One instance per connection (reuses TCP/TLS sessions).
// Throws OdbcError("08S01") on transport failure; HTTP errors are returned, not thrown.
class HttpClient {
 public:
  explicit HttpClient(HttpOptions opts);
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  HttpResponse send(const std::string& method, const std::string& url,
                    const std::vector<std::string>& headers, const std::string& body = "");

  // Transport errors that are safe to retry (connection reset, timeout...).
  static bool is_retryable_transport(int curl_code);

 private:
  HttpOptions opts_;
  void* curl_ = nullptr;
};

// Rejects plain http:// unless the host is loopback (used by the test harness).
void require_secure_url(const std::string& url);

}  // namespace sf
