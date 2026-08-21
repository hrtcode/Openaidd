// http.cpp - WinHTTP 实现
#include "http.hpp"
#include "version.hpp"
#include <windows.h>
#include <winhttp.h>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

namespace http {

static std::string last_error_msg(const char* what) {
  DWORD e = GetLastError();
  LPSTR buf = nullptr;
  DWORD n = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0, nullptr);
  std::string msg = what;
  msg += ": ";
  if (n && buf) {
    msg += std::string(buf, n);
    LocalFree(buf);
  } else {
    msg += "code " + std::to_string(e);
  }
  return msg;
}

struct UrlParts {
  std::wstring host;
  std::wstring path;
  int port = 0;
  bool secure = true;
};

static bool parse_url(const std::string& url, UrlParts& out) {
  std::wstring w = std::wstring(url.begin(), url.end());
  size_t pos = w.find(L"://");
  if (pos == std::wstring::npos) return false;
  std::wstring scheme = w.substr(0, pos);
  out.secure = (scheme == L"https");
  size_t hostStart = pos + 3;
  size_t pathStart = w.find(L'/', hostStart);
  std::wstring hostFull;
  if (pathStart == std::wstring::npos) {
    hostFull = w.substr(hostStart);
    out.path = L"/";
  } else {
    hostFull = w.substr(hostStart, pathStart - hostStart);
    out.path = w.substr(pathStart);
  }
  // 拆分主机与端口（IPv6 带 [] 的情况暂不支持）
  size_t colon = hostFull.find(L':');
  if (colon != std::wstring::npos) {
    out.host = hostFull.substr(0, colon);
    std::wstring port = hostFull.substr(colon + 1);
    if (!port.empty()) {
      int p = 0;
      for (wchar_t c : port) {
        if (c < L'0' || c > L'9') { p = 0; break; }
        p = p * 10 + (c - L'0');
      }
      out.port = p;
    }
  } else {
    out.host = hostFull;
  }
  if (out.host.empty()) return false;
  if (out.port == 0) out.port = out.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
  return true;
}

static std::wstring build_headers(const Headers& headers) {
  std::wstring h;
  for (auto& kv : headers) {
    std::wstring key(kv.first.begin(), kv.first.end());
    std::wstring val(kv.second.begin(), kv.second.end());
    h += key + L": " + val + L"\r\n";
  }
  return h;
}

// 通用请求核心：method = "POST" / "GET" 等
static bool request(
    const char* method,
    const std::string& url,
    const Headers& headers,
    const std::string& body,
    const std::function<void(const std::string& chunk)>& on_chunk,
    std::string& err,
    long* out_status_code) {

  UrlParts parts;
  if (!parse_url(url, parts)) { err = "invalid URL: " + url; return false; }

  HINTERNET hSession = WinHttpOpen(
      L"openaidd/0.0.0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) { err = last_error_msg("WinHttpOpen"); return false; }

  // 流式：接收超时设为 0（无限等待，适配长生成）
  WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 0);

  HINTERNET hConnect = WinHttpConnect(
      hSession, parts.host.c_str(), (INTERNET_PORT)parts.port, 0);
  if (!hConnect) { err = last_error_msg("WinHttpConnect"); WinHttpCloseHandle(hSession); return false; }

  DWORD flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
  std::wstring wmethod(method, method + strlen(method));
  HINTERNET hReq = WinHttpOpenRequest(
      hConnect, wmethod.c_str(), parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!hReq) { err = last_error_msg("WinHttpOpenRequest"); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

  // 关闭自动重定向跟随可能需要的头；不请求压缩
  std::map<std::string, std::string> h = headers;
  if (h.find("Accept-Encoding") == h.end()) h["Accept-Encoding"] = "identity";

  std::wstring wh = build_headers(h);
  if (!WinHttpSendRequest(hReq, wh.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0,
                          (DWORD)body.size(), 0)) {
    err = last_error_msg("WinHttpSendRequest");
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return false;
  }

  if (!body.empty()) {
    DWORD written = 0;
    if (!WinHttpWriteData(hReq, body.data(), (DWORD)body.size(), &written)) {
      err = last_error_msg("WinHttpWriteData");
      WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
      return false;
    }
  }

  if (!WinHttpReceiveResponse(hReq, nullptr)) {
    err = last_error_msg("WinHttpReceiveResponse");
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return false;
  }

  if (out_status_code) {
    DWORD code = 0, len = sizeof(code);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, nullptr)) {
      *out_status_code = (long)code;
    }
  }

  char buf[8192];
  DWORD read = 0;
  while (WinHttpReadData(hReq, buf, sizeof(buf), &read) && read > 0) {
    if (on_chunk) on_chunk(std::string(buf, (size_t)read));
  }

  WinHttpCloseHandle(hReq);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return true;
}

bool post_stream(
    const std::string& url,
    const Headers& headers,
    const std::string& body,
    const std::function<void(const std::string& chunk)>& on_chunk,
    std::string& err,
    long* out_status_code) {
  return request("POST", url, headers, body, on_chunk, err, out_status_code);
}

bool post(
    const std::string& url,
    const Headers& headers,
    const std::string& body,
    std::string& response,
    std::string& err,
    long* out_status_code) {
  return post_stream(url, headers, body,
                     [&](const std::string& chunk) { response += chunk; },
                     err, out_status_code);
}

bool get(
    const std::string& url,
    const Headers& headers,
    std::string& response,
    std::string& err,
    long* out_status_code) {
  return request("GET", url, headers, "",
                 [&](const std::string& chunk) { response += chunk; },
                 err, out_status_code);
}

}  // namespace http
