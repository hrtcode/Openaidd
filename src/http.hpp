// http.hpp - 基于 Windows WinHTTP 的零依赖 HTTPS 客户端（支持流式）
#pragma once
#include <string>
#include <map>
#include <functional>

namespace http {

using Headers = std::map<std::string, std::string>;

// 流式 POST：每收到一块响应字节就调用 on_chunk（用于 SSE/流式输出）
// 返回 false 表示出错（错误信息在 err 中）
bool post_stream(
    const std::string& url,
    const Headers& headers,
    const std::string& body,
    const std::function<void(const std::string& chunk)>& on_chunk,
    std::string& err,
    long* out_status_code = nullptr);

// 普通一次性 POST（非流式），返回 body 字符串
bool post(
    const std::string& url,
    const Headers& headers,
    const std::string& body,
    std::string& response,
    std::string& err,
    long* out_status_code = nullptr);

// 一次性 GET（非流式），返回 body 字符串（用于拉取 /models 等）
bool get(
    const std::string& url,
    const Headers& headers,
    std::string& response,
    std::string& err,
    long* out_status_code = nullptr);

}  // namespace http
