// webui.cpp - 内嵌 Web 界面：Winsock2 mini HTTP 服务器 + SSE 事件流 + 内嵌聊天页面
//
// 端点：
//   GET  /              内嵌 HTML 聊天页
//   GET  /api/events    SSE 事件流（重连/刷新时从 0 回放全部历史事件）
//   GET  /api/status    JSON 状态（模型/工作目录/权限/忙闲）
//   POST /api/message   {text}            发起一轮对话（忙时 409）
//   POST /api/confirm   {approved}        应答 run_command 确认
//   POST /api/settings  {auto_approve}    切换免确认执行命令
//   POST /api/reset     清空会话（同"新会话"）
//   GET  /api/sessions             会话列表（左侧历史栏）
//   POST /api/session/open {id}    打开历史会话（回放到前端）
//   POST /api/session/new          新建会话（清空当前上下文）
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // 需要 Vista+ 的条件变量 API
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include "webui.hpp"
#include "agent.hpp"
#include "json.hpp"
#include "think.hpp"
#include "session.hpp"
#include "utils.hpp"
#include "version.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace webui {

// 内嵌页面（定义在文件末尾）
extern const char* k_index_html;

// ---------------- Win32 同步封装（MinGW win32 线程模型无 std::thread） ----------------
struct CSLock {
  CRITICAL_SECTION* cs;
  explicit CSLock(CRITICAL_SECTION& c) : cs(&c) { EnterCriticalSection(cs); }
  ~CSLock() { LeaveCriticalSection(cs); }
  CSLock(const CSLock&) = delete;
  CSLock& operator=(const CSLock&) = delete;
};

template <class F>
static void spawn_thread(F f) {
  struct Box { F f; };
  Box* box = new Box{std::move(f)};
  HANDLE h = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
    Box* b = (Box*)p;
    b->f();
    delete b;
    return 0;
  }, box, 0, nullptr);
  if (h) CloseHandle(h);   // 分离线程
}

// ---------------- 事件总线：发布/订阅 + 全量日志（供刷新回放） ----------------
class EventBus {
public:
  EventBus() {
    InitializeCriticalSection(&cs_);
    InitializeConditionVariable(&cv_);
  }
  ~EventBus() { DeleteCriticalSection(&cs_); }

  void publish(const json::Value& ev) {
    std::string s = json::serialize(ev);
    EnterCriticalSection(&cs_);
    log_.push_back(std::move(s));
    LeaveCriticalSection(&cs_);
    WakeAllConditionVariable(&cv_);
  }

  // 等待第 idx 条事件；deadline 前无新事件返回空串
  std::string wait(size_t idx, int timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
    EnterCriticalSection(&cs_);
    for (;;) {
      if (idx < log_.size()) {
        std::string s = log_[idx];
        LeaveCriticalSection(&cs_);
        return s;
      }
      ULONGLONG now = GetTickCount64();
      if (now >= deadline) { LeaveCriticalSection(&cs_); return ""; }
      SleepConditionVariableCS(&cv_, &cs_, (DWORD)(deadline - now));
    }
  }

private:
  CRITICAL_SECTION cs_;
  CONDITION_VARIABLE cv_;
  std::deque<std::string> log_;
};

// ---------------- 服务器全局状态 ----------------
struct ServerState {
  cfg::Config cfg;
  agent::Agent* agent = nullptr;
  EventBus bus;

  CRITICAL_SECTION m;            // 保护 busy / 配置项 / 当前会话文件
  bool busy = false;
  std::string session_file;      // 当前会话文件（空 = 下一条消息时新建）

  // run_command 确认应答
  CRITICAL_SECTION cm;
  CONDITION_VARIABLE ccv;
  bool confirm_pending = false;
  bool confirm_result = false;

  ServerState() {
    InitializeCriticalSection(&m);
    InitializeCriticalSection(&cm);
    InitializeConditionVariable(&ccv);
  }
  ~ServerState() {
    DeleteCriticalSection(&m);
    DeleteCriticalSection(&cm);
  }
};

static json::Value status_event(ServerState* st) {
  CSLock lk(st->m);
  return json::object({
      {"type", "status"},
      {"busy", st->busy},
      {"model", st->cfg.model},
      {"provider", st->cfg.provider},
      {"cwd", st->cfg.cwd},
      {"auto_approve", st->cfg.auto_approve_commands},
  });
}

// ---------------- 会话历史（左侧栏数据源） ----------------

// 当前会话 id（文件名去 .json；尚无会话返回空）
static std::string current_session_id(ServerState* st) {
  std::string f;
  { CSLock lk(st->m); f = st->session_file; }
  if (f.empty()) return "";
  size_t p = f.find_last_of("/\\");
  std::string name = (p == std::string::npos) ? f : f.substr(p + 1);
  if (name.size() > 5 && name.substr(name.size() - 5) == ".json")
    name = name.substr(0, name.size() - 5);
  return name;
}

// 会话列表事件/响应（type=sessions，前端据此渲染左侧栏）
static json::Value sessions_json(ServerState* st) {
  json::Value arr = json::Value::array();
  for (const auto& mt : session::list()) {
    arr.push(json::object({
        {"id", mt.id}, {"saved_at", mt.saved_at}, {"model", mt.model},
        {"preview", mt.preview}, {"turns", (long)mt.turns}}));
  }
  return json::object({
      {"type", "sessions"},
      {"current", current_session_id(st)},
      {"sessions", std::move(arr)}});
}

// 把已加载的会话历史回放成 SSE 事件（前端重建聊天视图）。
// assistant 文本含 <think> 原文，须经 ThinkFilter 拆分思考/正文。
static void replay_history(ServerState* st) {
  for (const auto& m : st->agent->history()) {
    if (m.role == "user") {
      std::string text;
      for (const auto& it : m.items) {
        if (it.type == prov::ContentItem::ToolResult) {
          st->bus.publish(json::object({
              {"type", "tool_result"}, {"id", it.tool_use_id},
              {"content", it.text}, {"error", it.is_error},
              {"truncated", false}}));
        } else if (it.type == prov::ContentItem::Text) {
          text += it.text;
        }
      }
      if (!text.empty())
        st->bus.publish(json::object({{"type", "user"}, {"text", text}}));
    } else if (m.role == "assistant") {
      util::ThinkFilter f([st](const std::string& t, bool think) {
        st->bus.publish(json::object({
            {"type", "delta"}, {"text", t}, {"think", think}}));
      });
      for (const auto& it : m.items) {
        if (it.type == prov::ContentItem::Text) {
          f.feed(it.text);
        } else if (it.type == prov::ContentItem::ToolUse) {
          f.flush();
          st->bus.publish(json::object({
              {"type", "tool_start"}, {"id", it.id}, {"name", it.name}}));
          st->bus.publish(json::object({
              {"type", "tool_args"}, {"id", it.id},
              {"args", json::serialize(
                   it.input.isNull() ? json::Value::object() : it.input)}}));
        }
      }
      f.flush();
    }
  }
}

// 给 Agent 安装 Web 钩子：全部转为 SSE 事件
static void install_web_hooks(agent::Agent* a, ServerState* st) {
  agent::Hooks h;
  // <think> 过滤器：标签隐藏，思考内容以 think=true 的 delta 推给前端
  auto filter = std::make_shared<util::ThinkFilter>(
      [st](const std::string& t, bool think) {
        st->bus.publish(json::object({
            {"type", "delta"}, {"text", t}, {"think", think}}));
      });
  h.on_stream_start = [filter]() { filter->reset(); };
  h.on_text = [filter](const std::string& t) {
    filter->feed(t);
  };
  h.on_tool_start = [st, filter](const std::string& id, const std::string& name) {
    filter->flush();
    st->bus.publish(json::object({{"type", "tool_start"}, {"id", id}, {"name", name}}));
  };
  h.on_tool_args = [st](const std::string& id, const std::string& args) {
    st->bus.publish(json::object({{"type", "tool_args"}, {"id", id}, {"args", args}}));
  };
  h.on_tool_line = [st](const std::string& id, const std::string& line) {
    st->bus.publish(json::object({{"type", "tool_output"}, {"id", id}, {"line", line}}));
  };
  h.on_tool_result = [st](const std::string& id, const std::string& content, bool is_error) {
    // 内容太长截断（页面可展开的场景少，先统一截断）
    std::string c = content;
    const size_t kMax = 20000;
    bool truncated = false;
    if (c.size() > kMax) {
      c = c.substr(0, kMax) + "\n... (已截断，共 " + std::to_string(content.size()) + " 字节)";
      truncated = true;
    }
    st->bus.publish(json::object({
        {"type", "tool_result"}, {"id", id}, {"content", c},
        {"error", is_error}, {"truncated", truncated}}));
  };
  h.on_message_end = [st, filter](long in, long out) {
    filter->flush();
    st->bus.publish(json::object({{"type", "message_end"}, {"in", in}, {"out", out}}));
  };
  h.on_error = [st, filter](const std::string& t) {
    filter->flush();
    st->bus.publish(json::object({{"type", "error"}, {"text", t}}));
  };
  h.confirm = [st](const std::string& cmd) -> bool {
    st->bus.publish(json::object({{"type", "confirm_request"}, {"command", cmd}}));
    EnterCriticalSection(&st->cm);
    st->confirm_pending = true;
    st->confirm_result = false;
    while (st->confirm_pending)
      SleepConditionVariableCS(&st->ccv, &st->cm, INFINITE);
    bool approved = st->confirm_result;
    LeaveCriticalSection(&st->cm);
    st->bus.publish(json::object({
        {"type", "confirm_done"}, {"command", cmd}, {"approved", approved}}));
    return approved;
  };
  a->set_hooks(std::move(h));
}

// ---------------- HTTP 基础 ----------------
static int send_all(SOCKET s, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    int n = send(s, data.data() + off, (int)(data.size() - off), 0);
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static void respond(SOCKET s, int code, const std::string& ctype, const std::string& body) {
  const char* reason = code == 200 ? "OK"
                     : code == 400 ? "Bad Request"
                     : code == 404 ? "Not Found"
                     : code == 409 ? "Conflict" : "Error";
  std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
      "Content-Type: " + ctype + "\r\n"
      "Content-Length: " + std::to_string(body.size()) + "\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n";
  send_all(s, head + body);
}

static void respond_json(SOCKET s, int code, const json::Value& v) {
  respond(s, code, "application/json; charset=utf-8", json::serialize(v));
}

static std::string to_lower(const std::string& s) {
  std::string r = s;
  for (auto& c : r) if (c >= 'A' && c <= 'Z') c += 32;
  return r;
}

// 读取一个完整 HTTP 请求（头 + body）。失败返回空 method。
static bool read_request(SOCKET s, std::string& method, std::string& path, std::string& body) {
  std::string buf;
  char tmp[8192];
  size_t head_end = std::string::npos;
  while (head_end == std::string::npos) {
    int n = recv(s, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    buf.append(tmp, (size_t)n);
    head_end = buf.find("\r\n\r\n");
    if (buf.size() > 65536) return false;
  }
  std::string head = buf.substr(0, head_end);
  body = buf.substr(head_end + 4);

  // 请求行
  size_t sp1 = head.find(' ');
  size_t sp2 = head.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
  method = head.substr(0, sp1);
  path = head.substr(sp1 + 1, sp2 - sp1 - 1);

  // content-length
  std::string low = to_lower(head);
  size_t cl = low.find("content-length:");
  size_t content_length = 0;
  if (cl != std::string::npos) {
    size_t vs = cl + 15;
    while (vs < head.size() && head[vs] == ' ') vs++;
    content_length = (size_t)atoll(head.c_str() + vs);
    if (content_length > 1024 * 1024) return false;
  }

  // 继续读 body
  while (body.size() < content_length) {
    int n = recv(s, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    body.append(tmp, (size_t)n);
  }
  return true;
}

// ---------------- SSE 连接处理 ----------------
static void handle_sse(SOCKET s, ServerState* st) {
  std::string head = "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream; charset=utf-8\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n\r\n";
  if (send_all(s, head) < 0) return;

  // 发送初始状态，保证新连接立刻拿到模型/权限信息
  {
    std::string ev = json::serialize(status_event(st));
    send_all(s, "data: " + ev + "\n\n");
  }

  size_t idx = 0;
  for (;;) {
    std::string ev = st->bus.wait(idx, 15000);
    if (ev.empty()) {
      if (send_all(s, ": ping\n\n") < 0) break;
      continue;
    }
    std::string frame = "id: " + std::to_string(idx) + "\ndata: " + ev + "\n\n";
    if (send_all(s, frame) < 0) break;
    idx++;
  }
}

// ---------------- 业务路由 ----------------
// 原子忙检 + 启动 agent 线程；返回 false 表示正忙
static bool start_message(ServerState* st, const std::string& body) {
  json::Value req;
  try { req = json::parse(body); } catch (...) { return false; }
  std::string text = req.getStr("text");
  if (text.empty()) return false;

  {
    CSLock lk(st->m);
    if (st->busy) return false;
    st->busy = true;
  }
  st->bus.publish(json::object({{"type", "user"}, {"text", text}}));
  st->bus.publish(status_event(st));

  spawn_thread([st, text]() {
    st->agent->chat(text);
    // 持久化会话（首次落盘时创建新文件）
    std::string sf;
    {
      CSLock lk(st->m);
      if (st->session_file.empty())
        st->session_file = session::new_session_file();
      sf = st->session_file;
    }
    session::save(sf, st->cfg.model, st->agent->history());
    {
      CSLock lk(st->m);
      st->busy = false;
    }
    st->bus.publish(json::object({{"type", "done"}}));
    st->bus.publish(status_event(st));
    st->bus.publish(sessions_json(st));   // 左侧栏刷新
  });
  return true;
}

static void handle_client(SOCKET s, ServerState* st) {
  std::string method, path, body;
  if (!read_request(s, method, path, body)) { closesocket(s); return; }

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    respond(s, 200, "text/html; charset=utf-8", k_index_html);
  } else if (method == "GET" && path == "/api/events") {
    handle_sse(s, st);
  } else if (method == "GET" && path == "/api/status") {
    respond_json(s, 200, status_event(st));
  } else if (method == "POST" && path == "/api/message") {
    if (start_message(st, body)) {
      respond_json(s, 200, json::object({{"ok", true}}));
    } else {
      respond_json(s, 409, json::object({{"error", "busy"}}));
    }
  } else if (method == "POST" && path == "/api/confirm") {
    json::Value req;
    try { req = json::parse(body); } catch (...) { req = json::Value::object(); }
    bool approved = req.has("approved") && req.get("approved").asBool();
    EnterCriticalSection(&st->cm);
    if (st->confirm_pending) {
      st->confirm_result = approved;
      st->confirm_pending = false;
    }
    WakeAllConditionVariable(&st->ccv);
    LeaveCriticalSection(&st->cm);
    respond_json(s, 200, json::object({{"ok", true}}));
  } else if (method == "POST" && path == "/api/settings") {
    json::Value req;
    try { req = json::parse(body); } catch (...) { req = json::Value::object(); }
    if (req.has("auto_approve")) {
      bool v = req.get("auto_approve").asBool();
      { CSLock lk(st->m); st->cfg.auto_approve_commands = v; }
      st->bus.publish(status_event(st));
      respond_json(s, 200, json::object({{"ok", true}}));
    } else {
      respond_json(s, 400, json::object({{"error", "missing auto_approve"}}));
    }
  } else if (method == "GET" && path == "/api/sessions") {
    respond_json(s, 200, sessions_json(st));
  } else if (method == "POST" && path == "/api/session/open") {
    {
      CSLock lk(st->m);
      if (st->busy) { respond_json(s, 409, json::object({{"error", "busy"}})); closesocket(s); return; }
    }
    json::Value req;
    try { req = json::parse(body); } catch (...) { req = json::Value::object(); }
    std::string id = req.getStr("id");
    // id 只允许纯文件名（防路径穿越）
    if (id.empty() || id.find('/') != std::string::npos ||
        id.find('\\') != std::string::npos) {
      respond_json(s, 400, json::object({{"error", "bad id"}}));
    } else {
      std::string file = session::sessions_dir() + "/" + id + ".json";
      std::vector<prov::Message> hist;
      std::string model;
      if (!session::load(file, hist, model)) {
        respond_json(s, 404, json::object({{"error", "session not found"}}));
      } else {
        st->agent->set_history(std::move(hist));
        { CSLock lk(st->m); st->session_file = file; }
        // 先清前端视图，再回放该会话的全部消息
        st->bus.publish(json::object({{"type", "reset"}}));
        replay_history(st);
        st->bus.publish(sessions_json(st));
        respond_json(s, 200, json::object({{"ok", true}}));
      }
    }
  } else if (method == "POST" && path == "/api/session/new") {
    {
      CSLock lk(st->m);
      if (st->busy) { respond_json(s, 409, json::object({{"error", "busy"}})); closesocket(s); return; }
    }
    st->agent->reset();
    { CSLock lk(st->m); st->session_file.clear(); }
    st->bus.publish(json::object({{"type", "reset"}}));
    st->bus.publish(sessions_json(st));
    respond_json(s, 200, json::object({{"ok", true}}));
  } else if (method == "POST" && path == "/api/reset") {
    {
      CSLock lk(st->m);
      if (st->busy) { respond_json(s, 409, json::object({{"error", "busy"}})); closesocket(s); return; }
    }
    st->agent->reset();
    { CSLock lk(st->m); st->session_file.clear(); }   // 语义同"新会话"
    st->bus.publish(json::object({{"type", "reset"}}));
    st->bus.publish(sessions_json(st));
    respond_json(s, 200, json::object({{"ok", true}}));
  } else {
    respond_json(s, 404, json::object({{"error", "not found"}}));
  }
  closesocket(s);
}

// ---------------- 入口 ----------------

// 创建监听 socket 并绑定端口（仅本机访问）；成功返回 true
static bool setup_listener(int port, SOCKET* out) {
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return false;

  // 注意：Windows 下不开 SO_REUSEADDR（它会造成端口被抢占、两个实例行为不确定），
  // 端口被占用时 bind 直接失败，由调用方提示。

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 仅本机访问
  addr.sin_port = htons((u_short)port);
  if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    std::cout << "端口 " << port << " 绑定失败（可能被占用），错误码 "
              << WSAGetLastError() << "\n";
    closesocket(listener);
    return false;
  }
  if (listen(listener, 16) == SOCKET_ERROR) {
    closesocket(listener);
    return false;
  }
  *out = listener;
  return true;
}

// accept 主循环（永不返回）
static void accept_loop(SOCKET listener, ServerState* st) {
  for (;;) {
    sockaddr_in cli;
    int clen = sizeof(cli);
    SOCKET cs = accept(listener, (sockaddr*)&cli, &clen);
    if (cs == INVALID_SOCKET) continue;
    spawn_thread([cs, st]() { handle_client(cs, st); });
  }
}

int run(cfg::Config& cfg, int port) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::cout << "WSAStartup 失败\n";
    return 1;
  }

  SOCKET listener;
  if (!setup_listener(port, &listener)) return 1;

  ServerState st;
  st.cfg = cfg;
  agent::Agent a(cfg);
  st.agent = &a;
  install_web_hooks(&a, &st);

  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
  std::cout << util::color::cyan << OPENAIDD_NAME << " WebUI v" << OPENAIDD_VERSION
            << " 已启动: " << url << util::color::reset << "\n";
  std::cout << util::color::gray << "模型: " << cfg.model
            << "  工作目录: " << cfg.cwd
            << "  免确认执行命令: " << (cfg.auto_approve_commands ? "开" : "关")
            << "\n按 Ctrl+C 退出。\n" << util::color::reset;
  std::cout.flush();

  // 打开默认浏览器
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

  accept_loop(listener, &st);
  return 0;  // 不可达；Ctrl+C 直接终止进程
}

bool start_background(cfg::Config cfg, int port) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

  SOCKET listener;
  if (!setup_listener(port, &listener)) return false;

  // ServerState / Agent 故意在堆上且不释放：进程退出时一并回收，
  // 避免后台线程还在用就被析构。
  ServerState* st = new ServerState();
  st->cfg = cfg;
  agent::Agent* a = new agent::Agent(cfg);
  st->agent = a;
  install_web_hooks(a, st);

  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
  std::cout << util::color::cyan << OPENAIDD_NAME << " WebUI 已后台启动: " << url
            << util::color::reset << "\n" << util::color::gray
            << "（终端 REPL 可继续使用；两边各自独立会话，历史统一存 resume/）\n"
            << util::color::reset;
  std::cout.flush();

  // 打开默认浏览器
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

  spawn_thread([listener, st]() { accept_loop(listener, st); });
  return true;
}

}  // namespace webui

// ---------------- 内嵌聊天页面 ----------------
const char* webui::k_index_html = R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>openaidd</title>
<style>
  :root {
    --bg: #0f1117; --panel: #171a23; --panel2: #1d212c;
    --border: #2a2f3d; --text: #e2e5ec; --dim: #8b93a7;
    --accent: #4f8cff; --green: #3fb96b; --red: #e0564f; --amber: #e6b450;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { height: 100%; }
  body {
    background: var(--bg); color: var(--text);
    font-family: "Segoe UI", "Microsoft YaHei", system-ui, sans-serif;
    font-size: 14px; display: flex; flex-direction: row;
  }

  /* 左侧历史会话栏 */
  #side {
    width: 248px; flex: none; background: var(--panel);
    border-right: 1px solid var(--border);
    display: flex; flex-direction: column; overflow: hidden;
  }
  .side-head {
    display: flex; align-items: center; justify-content: space-between;
    padding: 12px 12px 8px;
  }
  .side-head .stitle { font-weight: 700; font-size: 13px; }
  #newBtn {
    background: var(--accent); color: #fff; border: none; border-radius: 6px;
    padding: 4px 10px; cursor: pointer; font-size: 12px;
  }
  #newBtn:hover { filter: brightness(1.15); }
  #sessList { flex: 1; overflow-y: auto; padding: 2px 8px 12px; }
  .sess {
    padding: 8px 10px; border-radius: 8px; cursor: pointer; margin-bottom: 2px;
    border: 1px solid transparent;
  }
  .sess:hover { background: var(--panel2); }
  .sess.cur { background: var(--panel2); border-color: var(--accent); }
  .sess .sp {
    font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  .sess .sm { font-size: 11px; color: var(--dim); margin-top: 3px; }
  .sess .empty { color: var(--dim); font-size: 12px; text-align: center; padding: 14px 0; }

  /* 主区（原单栏结构整体移入） */
  #main { flex: 1; min-width: 0; display: flex; flex-direction: column; }
  @media (max-width: 720px) { #side { display: none; } }
  header {
    display: flex; align-items: center; gap: 12px;
    padding: 10px 16px; background: var(--panel);
    border-bottom: 1px solid var(--border); flex-wrap: wrap;
  }
  header .logo { font-weight: 700; color: var(--accent); letter-spacing: .5px; }
  header .dot { width: 9px; height: 9px; border-radius: 50%; background: var(--green); flex: none; }
  header .dot.busy { background: var(--amber); animation: pulse 1s infinite alternate; }
  @keyframes pulse { from { opacity: 1; } to { opacity: .35; } }
  header .meta { color: var(--dim); font-size: 12px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  header .spacer { flex: 1; }
  header label { display: flex; align-items: center; gap: 6px; color: var(--dim); font-size: 12px; cursor: pointer; user-select: none; }
  header button {
    background: var(--panel2); color: var(--text); border: 1px solid var(--border);
    border-radius: 6px; padding: 5px 12px; cursor: pointer; font-size: 12px;
  }
  header button:hover { border-color: var(--accent); }

  #msgs { flex: 1; overflow-y: auto; padding: 16px; display: flex; flex-direction: column; gap: 14px; }
  .msg { max-width: 86%; white-space: pre-wrap; word-break: break-word; line-height: 1.65; }
  .msg.user {
    align-self: flex-end; background: #243252; border: 1px solid #33456e;
    padding: 9px 13px; border-radius: 12px 12px 3px 12px;
  }
  .msg.assistant { align-self: flex-start; }
  .msg.error { align-self: flex-start; color: var(--red); }
  .msg .tokens { color: var(--dim); font-size: 11px; margin-top: 6px; }

  /* 思考块：<think> 标签隐藏，思考内容收进黑色可折叠块 */
  .think {
    align-self: flex-start; width: min(86%, 720px);
    background: #000; border: 1px solid #1a1a1a;
    border-radius: 8px; overflow: hidden;
    color: #7a8296;
  }
  .think .thead {
    display: flex; align-items: center; gap: 8px; padding: 6px 12px;
    cursor: pointer; font-size: 12px; color: #7a8296; user-select: none;
  }
  .think .thead .chev { margin-left: auto; font-size: 10px; }
  .think .tbody {
    padding: 4px 12px 10px; font-size: 13px; line-height: 1.6;
    white-space: pre-wrap; word-break: break-word;
    max-height: 300px; overflow-y: auto;
  }
  .think.closed .tbody { display: none; }

  .tool {
    align-self: flex-start; width: min(86%, 720px);
    background: var(--panel); border: 1px solid var(--border);
    border-left: 3px solid var(--amber); border-radius: 8px; overflow: hidden;
  }
  .tool .thead {
    display: flex; align-items: center; gap: 8px; padding: 8px 12px;
    cursor: pointer; color: var(--amber); font-size: 13px; user-select: none;
  }
  .tool .thead .chev { margin-left: auto; color: var(--dim); font-size: 10px; }
  .tool .tbody { border-top: 1px solid var(--border); display: none; }
  .tool.open .tbody { display: block; }
  .tool .label { color: var(--dim); font-size: 11px; padding: 6px 12px 0; }
  .tool pre {
    margin: 2px 12px 10px; padding: 8px 10px; background: var(--panel2);
    border-radius: 6px; font-size: 12px; line-height: 1.5;
    max-height: 260px; overflow: auto; white-space: pre-wrap; word-break: break-word;
    font-family: Consolas, "Courier New", monospace; color: #cdd3e0;
  }
  .tool.err { border-left-color: var(--red); }
  .tool.err .thead { color: var(--red); }

  #confirm {
    display: none; margin: 0 16px 8px; padding: 12px 14px;
    background: #2d2413; border: 1px solid var(--amber); border-radius: 8px;
  }
  #confirm .cmd {
    font-family: Consolas, monospace; font-size: 13px; color: var(--amber);
    white-space: pre-wrap; word-break: break-all; margin-bottom: 10px;
  }
  #confirm .btns { display: flex; gap: 10px; }
  #confirm button {
    padding: 6px 20px; border-radius: 6px; border: none; cursor: pointer; font-size: 13px;
  }
  #confirm .yes { background: var(--green); color: #fff; }
  #confirm .no { background: transparent; color: var(--red); border: 1px solid var(--red); }

  #inputrow {
    display: flex; gap: 10px; padding: 12px 16px;
    background: var(--panel); border-top: 1px solid var(--border);
  }
  #input {
    flex: 1; background: var(--panel2); border: 1px solid var(--border);
    color: var(--text); border-radius: 8px; padding: 10px 14px;
    font-size: 14px; font-family: inherit; resize: none; min-height: 42px; max-height: 160px;
  }
  #input:focus { outline: none; border-color: var(--accent); }
  #send {
    background: var(--accent); color: #fff; border: none; border-radius: 8px;
    padding: 0 22px; cursor: pointer; font-size: 14px;
  }
  #send:disabled { background: #33415e; cursor: not-allowed; }

  #notice { text-align: center; color: var(--dim); font-size: 12px; padding: 6px; }
</style>
</head>
<body>
<aside id="side">
  <div class="side-head">
    <span class="stitle">历史会话</span>
    <button id="newBtn">＋ 新会话</button>
  </div>
  <div id="sessList"></div>
</aside>
<main id="main">
<header>
  <span class="logo">openaidd <small style="color:var(--dim);font-weight:400">v0.0.0.1</small></span>
  <span class="dot" id="dot"></span>
  <span class="meta" id="meta">连接中…</span>
  <span class="spacer"></span>
  <label><input type="checkbox" id="autoApprove"> 免确认执行命令</label>
  <button id="resetBtn">新会话</button>
</header>
<div id="msgs"></div>
<div id="confirm">
  <div class="cmd" id="confirmCmd"></div>
  <div class="btns">
    <button class="yes" id="confirmYes">同意执行</button>
    <button class="no" id="confirmNo">拒绝</button>
  </div>
</div>
<div id="inputrow">
  <textarea id="input" placeholder="输入消息，Enter 发送，Shift+Enter 换行" rows="1"></textarea>
  <button id="send">发送</button>
</div>
<div id="notice"></div>
</main>

<script>
"use strict";
const $ = id => document.getElementById(id);
const msgs = $("msgs"), input = $("input"), send = $("send"), dot = $("dot"), meta = $("meta");
let busy = false;
let curAssistant = null;      // 当前流式 assistant 气泡
let curThink = null;          // 当前流式思考块（黑色）
const toolCards = new Map();  // id -> {el, outputEl, streamed}
let curSession = "";          // 当前会话 id（左侧栏高亮）

// ---------- 左侧历史会话栏 ----------
function renderSessions(ev) {
  curSession = ev.current || "";
  const list = $("sessList");
  list.innerHTML = "";
  const items = ev.sessions || [];
  if (!items.length) {
    const d = document.createElement("div");
    d.className = "empty"; d.textContent = "暂无历史会话";
    list.appendChild(d);
    return;
  }
  for (const s of items) {
    const d = document.createElement("div");
    d.className = "sess" + (s.id === curSession ? " cur" : "");
    const p = document.createElement("div");
    p.className = "sp"; p.textContent = s.preview || "(无预览)";
    const m = document.createElement("div");
    m.className = "sm";
    m.textContent = (s.saved_at || "") + " · " + (s.turns || 0) + " 轮";
    d.appendChild(p); d.appendChild(m);
    d.title = (s.model || "") + "\n" + (s.saved_at || "");
    d.onclick = () => openSession(s.id);
    list.appendChild(d);
  }
}
function openSession(id) {
  if (busy || id === curSession) return;
  fetch("/api/session/open", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ id })
  }).catch(() => {});
}
function newSession() {
  if (busy) return;
  fetch("/api/session/new", { method: "POST" }).catch(() => {});
}

function esc(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
function scrollBottom() { msgs.scrollTop = msgs.scrollHeight; }
function closeAssistant() { curAssistant = null; }
function closeThink() { if (curThink) { curThink.classList.add("closed"); curThink = null; } }

function addUser(text) {
  closeAssistant(); closeThink();
  const d = document.createElement("div");
  d.className = "msg user"; d.textContent = text;
  msgs.appendChild(d); scrollBottom();
}

// 黑色思考块：正文出现后自动折叠
function addThink() {
  if (curThink) return curThink;
  const el = document.createElement("div");
  el.className = "think";
  el.innerHTML =
    '<div class="thead"><span>&#10087; 思考过程</span><span class="chev">&#9662;</span></div>' +
    '<div class="tbody"></div>';
  el.querySelector(".thead").onclick = () => el.classList.toggle("closed");
  msgs.appendChild(el);
  curThink = el;
  return el;
}

function appendThink(text) {
  addThink().querySelector(".tbody").textContent += text;
  scrollBottom();
}

function addAssistant() {
  if (curAssistant) return curAssistant;
  closeThink();   // 正文开始，折叠思考块
  const d = document.createElement("div");
  d.className = "msg assistant";
  msgs.appendChild(d);
  curAssistant = d;
  return d;
}

function addTokens(inT, outT) {
  if (!curAssistant) return;
  closeThink();
  const t = document.createElement("div");
  t.className = "tokens"; t.textContent = inT + "↑ / " + outT + "↓ tokens";
  curAssistant.appendChild(t);
  closeAssistant(); scrollBottom();
}

function addToolCard(id, name) {
  closeAssistant(); closeThink();
  const el = document.createElement("div");
  el.className = "tool";
  el.innerHTML =
    '<div class="thead"><span>&#9209; ' + esc(name) + '</span><span class="chev">&#9662;</span></div>' +
    '<div class="tbody"><div class="label">参数</div><pre class="args"></pre>' +
    '<div class="label">输出</div><pre class="out"></pre></div>';
  el.querySelector(".thead").onclick = () => el.classList.toggle("open");
  msgs.appendChild(el);
  toolCards.set(id, { el, streamed: false });
  scrollBottom();
}

function toolCard(id) { return toolCards.get(id); }

function setToolArgs(id, args) {
  const c = toolCard(id); if (!c) return;
  c.el.querySelector(".args").textContent = args;
}

function appendToolLine(id, line) {
  const c = toolCard(id); if (!c) return;
  c.el.classList.add("open");
  c.el.querySelector(".out").textContent += line;
  c.streamed = true;
  scrollBottom();
}

function finishToolCard(id, content, isError) {
  const c = toolCard(id); if (!c) return;
  if (isError) c.el.classList.add("err");
  if (!c.streamed) {
    // 非流式工具：直接展示结果
    c.el.querySelector(".out").textContent = content;
  } else {
    // run_command 已流式展示输出，结果里只补退出码行
    const first = content.split("\n")[0];
    c.el.querySelector(".out").textContent += "\n[" + first + "]\n";
  }
  scrollBottom();
}

function showConfirm(cmd) {
  $("confirmCmd").textContent = cmd;
  $("confirm").style.display = "block";
  scrollBottom();
}
function hideConfirm() { $("confirm").style.display = "none"; }

function setBusy(b) {
  busy = b;
  send.disabled = b;
  dot.classList.toggle("busy", b);
  input.placeholder = b ? "agent 处理中…" : "输入消息，Enter 发送，Shift+Enter 换行";
}

function setStatus(ev) {
  setBusy(!!ev.busy);
  $("autoApprove").checked = !!ev.auto_approve;
  meta.textContent = ev.model + " · " + ev.cwd;
}

function handle(ev) {
  switch (ev.type) {
    case "user":      addUser(ev.text); break;
    case "delta":
      if (ev.think) appendThink(ev.text);
      else { addAssistant().textContent += ev.text; scrollBottom(); }
      break;
    case "tool_start":addToolCard(ev.id, ev.name); break;
    case "tool_args": setToolArgs(ev.id, ev.args); break;
    case "tool_output": appendToolLine(ev.id, ev.line); break;
    case "tool_result": finishToolCard(ev.id, ev.content, ev.error); break;
    case "message_end": addTokens(ev.in, ev.out); break;
    case "error": {
      closeAssistant(); closeThink();
      const d = document.createElement("div");
      d.className = "msg error"; d.textContent = "[错误] " + ev.text;
      msgs.appendChild(d); scrollBottom(); break;
    }
    case "confirm_request": showConfirm(ev.command); break;
    case "confirm_done": hideConfirm(); break;
    case "done": setBusy(false); closeAssistant(); closeThink(); break;
    case "reset": msgs.innerHTML = ""; toolCards.clear(); curAssistant = null; curThink = null; break;
    case "status": setStatus(ev); break;
    case "sessions": renderSessions(ev); break;
  }
}

// SSE：服务器断开时浏览器自动重连，并从 id=0 回放全部历史
const es = new EventSource("/api/events");
es.onmessage = e => { try { handle(JSON.parse(e.data)); } catch (err) {} };

fetch("/api/status").then(r => r.json()).then(setStatus).catch(() => {});
fetch("/api/sessions").then(r => r.json()).then(renderSessions).catch(() => {});
$("newBtn").onclick = newSession;

// 发送消息
function submit() {
  const text = input.value.trim();
  if (!text || busy) return;
  input.value = ""; input.style.height = "auto";
  setBusy(true);
  fetch("/api/message", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text })
  }).then(r => { if (r.status === 409) setBusy(false); }).catch(() => setBusy(false));
}

send.onclick = submit;
input.addEventListener("keydown", e => {
  if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); submit(); }
});
input.addEventListener("input", () => {
  input.style.height = "auto";
  input.style.height = Math.min(input.scrollHeight, 160) + "px";
});

// 确认按钮
$("confirmYes").onclick = () => postConfirm(true);
$("confirmNo").onclick = () => postConfirm(false);
function postConfirm(approved) {
  hideConfirm();
  fetch("/api/confirm", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ approved })
  }).catch(() => {});
}

// 设置
$("autoApprove").onchange = e => {
  fetch("/api/settings", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ auto_approve: e.target.checked })
  }).catch(() => {});
};

$("resetBtn").onclick = newSession;

input.focus();
</script>
</body>
</html>)html";
