#include "http.h"

#include <curl/curl.h>

namespace sf {

namespace {
struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};
CurlGlobal& curl_global() {
  static CurlGlobal g;
  return g;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* resp = static_cast<HttpResponse*>(userdata);
  std::string line(buffer, size * nitems);
  if (starts_with_ci(line, "HTTP/")) resp->headers.clear();  // new response after redirect/100-continue
  size_t colon = line.find(':');
  if (colon != std::string::npos) resp->headers[trim(line.substr(0, colon))] = trim(line.substr(colon + 1));
  return size * nitems;
}
}  // namespace

HttpClient::HttpClient(HttpOptions opts) : opts_(std::move(opts)) {
  curl_global();
  curl_ = curl_easy_init();
  if (!curl_) throw OdbcError("HY001", "Unable to initialise HTTP client");
}

HttpClient::~HttpClient() {
  if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

bool HttpClient::is_retryable_transport(int code) {
  switch (code) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
      return true;
    default:
      return false;
  }
}

HttpResponse HttpClient::send(const std::string& method, const std::string& url,
                              const std::vector<std::string>& headers, const std::string& body) {
  CURL* c = static_cast<CURL*>(curl_);
  curl_easy_reset(c);
  HttpResponse resp;

  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, opts_.timeout_seconds);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, opts_.connect_timeout_seconds);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate: big win on large query pages
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &resp);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "EloquixSalesforceODBC/" SFODBC_DRIVER_VERSION);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, opts_.verify_ssl ? 1L : 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, opts_.verify_ssl ? 2L : 0L);
  if (!opts_.ca_bundle.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, opts_.ca_bundle.c_str());
  if (!opts_.proxy_url.empty()) {
    curl_easy_setopt(c, CURLOPT_PROXY, opts_.proxy_url.c_str());
    if (!opts_.proxy_user.empty()) {
      curl_easy_setopt(c, CURLOPT_PROXYUSERNAME, opts_.proxy_user.c_str());
      curl_easy_setopt(c, CURLOPT_PROXYPASSWORD, opts_.proxy_password.c_str());
    }
  }

  struct curl_slist* hdrs = nullptr;
  for (auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

  if (method == "POST") {
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
  } else if (method != "GET") {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (!body.empty()) {
      curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
      curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    }
  }

  CURLcode rc = curl_easy_perform(c);
  curl_slist_free_all(hdrs);
  if (rc != CURLE_OK) {
    throw OdbcError("08S01", std::string("Network error contacting Salesforce: ") + curl_easy_strerror(rc),
                    static_cast<int>(rc));
  }
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
  return resp;
}

void require_secure_url(const std::string& url) {
  if (starts_with_ci(url, "https://")) return;
  if (starts_with_ci(url, "http://127.0.0.1") || starts_with_ci(url, "http://localhost")) return;
  throw OdbcError("08001", "Insecure URL rejected (https required): " + url);
}

}  // namespace sf
