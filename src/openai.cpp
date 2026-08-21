// openai.cpp - OpenAI 兼容 Chat Completions 适配（function calling）
#include "provider.hpp"
#include "config.hpp"
#include "http.hpp"
#include "sse.hpp"
#include "json.hpp"
#include "utils.hpp"
#include <cctype>
#include <map>

namespace {

// API Key 类错误的排查提示：2049/invalid api key/401/unauthorized 等
static std::string api_key_hint(const std::string& msg) {
  std::string lower = msg;
  for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
  bool bad_key =
      lower.find("invalid api key") != std::string::npos ||
      lower.find("2049") != std::string::npos ||
      lower.find("401") != std::string::npos ||
      lower.find("unauthorized") != std::string::npos ||
      lower.find("authentication") != std::string::npos;
  if (!bad_key) return "";
  return "（API Key 无效：① 确认与端点地区匹配——MiniMax 国际版 api.minimax.io / 国内版 api.minimaxi.com；"
         "② Key 前后无多余空格/换行；③ token plan 订阅 Key 与普通 API Key 不通用）";
}

// 把内部消息转成 OpenAI 兼容请求消息。
// 返回 vector：普通消息 1 条；带 ToolResult 的 user 消息拆成多条 role=tool 消息
// （OpenAI 官方格式，MiniMax 等兼容端点对 user+content 数组的旧格式会报
//  "tool call result does not follow tool call" 错误，多工具时必现）。
std::vector<json::Value> to_openai_messages(const prov::Message& m) {
  std::vector<json::Value> out;

  bool hasToolResult = false;
  for (auto& it : m.items)
    if (it.type == prov::ContentItem::ToolResult) hasToolResult = true;

  if (m.role == "user" && hasToolResult) {
    for (auto& it : m.items) {
      if (it.type != prov::ContentItem::ToolResult) continue;
      json::Value t = json::Value::object();
      t.set("role", "tool");
      t.set("tool_call_id", it.tool_use_id);
      t.set("content", it.text);
      out.push_back(std::move(t));
    }
    return out;
  }

  json::Value mm = json::Value::object();
  mm.set("role", m.role);

  bool hasToolUse = false;
  for (auto& it : m.items)
    if (it.type == prov::ContentItem::ToolUse) hasToolUse = true;

  if (m.role == "user") {
    std::string text;
    for (auto& it : m.items)
      if (it.type == prov::ContentItem::Text) { if (!text.empty()) text += "\n"; text += it.text; }
    mm.set("content", text);
  } else { // assistant
    std::string text;
    for (auto& it : m.items)
      if (it.type == prov::ContentItem::Text) { if (!text.empty()) text += "\n"; text += it.text; }

    if (hasToolUse) {
      if (!text.empty()) mm.set("content", text);
      else mm.set("content", json::Value(nullptr));
      json::Value tcs = json::Value::array();
      for (auto& it : m.items) {
        if (it.type == prov::ContentItem::ToolUse) {
          json::Value tc = json::Value::object();
          tc.set("id", it.id);
          tc.set("type", "function");
          json::Value fn = json::Value::object();
          fn.set("name", it.name);
          fn.set("arguments", it.input.isNull() ? std::string("{}") : json::serialize(it.input));
          tc.set("function", std::move(fn));
          tcs.push(std::move(tc));
        }
      }
      mm.set("tool_calls", std::move(tcs));
    } else {
      mm.set("content", text);
    }
  }
  out.push_back(std::move(mm));
  return out;
}

class OpenAIProvider : public prov::Provider {
public:
  explicit OpenAIProvider(const cfg::Config& c) : cfg_(c) {}

  void stream_chat(
      const std::string& system,
      const std::vector<prov::Message>& messages,
      const std::vector<tools::ToolDef>& tooldefs,
      const std::function<void(const prov::StreamEvent&)>& on_event,
      std::string& err) override {

    json::Value body = json::Value::object();
    body.set("model", cfg_.model);
    body.set("stream", true);

    json::Value msgs = json::Value::array();
    if (!system.empty()) {
      json::Value sm = json::Value::object();
      sm.set("role", "system");
      sm.set("content", system);
      msgs.push(std::move(sm));
    }
    for (auto& m : messages) {
      auto converted = to_openai_messages(m);
      for (auto& v : converted) msgs.push(std::move(v));
    }
    body.set("messages", std::move(msgs));

    json::Value tools = json::Value::array();
    for (auto& t : tooldefs) {
      json::Value o = json::Value::object();
      o.set("type", "function");
      json::Value fn = json::Value::object();
      fn.set("name", t.name);
      fn.set("description", t.description);
      fn.set("parameters", t.schema);
      o.set("function", std::move(fn));
      tools.push(std::move(o));
    }
    if (!tools.arr.empty()) {
      body.set("tools", std::move(tools));
      body.set("tool_choice", "auto");
    }
    json::Value so = json::Value::object();
    so.set("include_usage", true);
    body.set("stream_options", std::move(so));

    std::string payload = json::serialize(body);

    http::Headers headers;
    headers["authorization"] = "Bearer " + cfg_.api_key;
    headers["content-type"] = "application/json";

    struct TC { std::string id; std::string name; std::string args; };
    std::map<int, TC> tcalls;
    long inTokens = 0, outTokens = 0;
    bool ended = false;

    auto finalize_toolcalls = [&]() {
      for (auto& kv : tcalls) {
        TC& tc = kv.second;
        on_event(prov::StreamEvent::toolStart(tc.id, tc.name));
        on_event(prov::StreamEvent::toolInput(tc.args));
        on_event(prov::StreamEvent::toolEnd());
      }
      tcalls.clear();
    };
    auto emit_end = [&]() {
      if (ended) return;
      ended = true;
      finalize_toolcalls();
      on_event(prov::StreamEvent::msgEnd(inTokens, outTokens));
    };

    sse::Parser parser([&](const std::string& name, const std::string& data) {
      (void)name;
      if (data == "[DONE]") { emit_end(); return; }
      if (data.empty()) return;
      json::Value ev;
      try { ev = json::parse(data); } catch (...) { return; }
      if (ev.has("error")) {
        auto e = ev.get("error", json::Value::object());
        std::string em = e.getStr("message", "openai error");
        on_event(prov::StreamEvent::err(em + api_key_hint(em)));
        return;
      }
      // usage 单独块：choices 为空数组时视为独立 usage 事件
      if (ev.has("usage")) {
        auto ch = ev.get("choices", json::Value::array());
        if (ch.arr.empty()) {
          auto u = ev.get("usage", json::Value::object());
          inTokens = u.getLong("prompt_tokens", inTokens);
          outTokens = u.getLong("completion_tokens", outTokens);
        }
      }
      auto ch = ev.get("choices", json::Value::array());
      if (ch.arr.empty()) return;
      auto c0 = ch.arr[0];
      auto delta = c0.get("delta", json::Value::object());
      if (delta.has("content")) {
        std::string t = delta.get("content", json::Value("")).asString();
        if (!t.empty()) on_event(prov::StreamEvent::text_delta(t));
      }
      if (delta.has("tool_calls")) {
        auto tcs = delta.get("tool_calls", json::Value::array());
        for (auto& tcj : tcs.arr) {
          int idx = (int)tcj.getLong("index", 0);
          TC& tc = tcalls[idx];
          if (tcj.has("id")) tc.id = tcj.getStr("id");
          auto fn = tcj.get("function", json::Value::object());
          if (fn.has("name") && !fn.getStr("name").empty()) tc.name = fn.getStr("name");
          if (fn.has("arguments")) tc.args += fn.getStr("arguments");
        }
      }
      std::string fr = c0.getStr("finish_reason");
      // finish_reason 到达时 usage 事件可能尚未到来，不能立即 emit_end，
      // 延迟到 [DONE] 或流结束，确保 token 统计完整。
      (void)fr;
    });

    long status = 0;
    std::string full;
    bool ok = http::post_stream(cfg_.base_url, headers, payload,
        [&](const std::string& chunk) { parser.feed(chunk); full += chunk; },
        err, &status);
    if (!ok) return;
    parser.flush();
    emit_end();  // 兜底（若未收到 [DONE]）

    if (status >= 400 && err.empty()) {
      std::string msg = "HTTP " + std::to_string(status);
      try {
        json::Value e = json::parse(full);
        if (e.has("error")) {
          auto er = e.get("error", json::Value::object());
          msg = er.getStr("message", msg);
        } else if (e.has("base_resp")) {  // MiniMax 部分接口格式
          auto br = e.get("base_resp", json::Value::object());
          std::string sm = br.getStr("status_msg", "");
          if (!sm.empty()) msg = sm + " (" + std::to_string(br.getLong("status_code", 0)) + ")";
        }
        msg += api_key_hint(msg);
      } catch (...) {}
      err = msg;
      on_event(prov::StreamEvent::err(msg));
    }
  }

private:
  cfg::Config cfg_;
};

}  // namespace

std::unique_ptr<prov::Provider> make_openai(const cfg::Config& c) {
  return std::make_unique<OpenAIProvider>(c);
}
