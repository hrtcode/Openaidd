// anthropic.cpp - Anthropic Messages API 适配
#include "provider.hpp"
#include "config.hpp"
#include "http.hpp"
#include "sse.hpp"
#include "json.hpp"
#include "utils.hpp"

namespace {

class AnthropicProvider : public prov::Provider {
public:
  explicit AnthropicProvider(const cfg::Config& c) : cfg_(c) {}

  void stream_chat(
      const std::string& system,
      const std::vector<prov::Message>& messages,
      const std::vector<tools::ToolDef>& tooldefs,
      const std::function<void(const prov::StreamEvent&)>& on_event,
      std::string& err) override {

    json::Value body = json::Value::object();
    body.set("model", cfg_.model);
    body.set("max_tokens", cfg_.max_tokens);
    if (!system.empty()) body.set("system", system);
    body.set("stream", true);

    // tools
    json::Value tools = json::Value::array();
    for (auto& t : tooldefs) {
      json::Value o = json::Value::object();
      o.set("name", t.name);
      o.set("description", t.description);
      o.set("input_schema", t.schema);
      tools.push(std::move(o));
    }
    if (!tools.arr.empty()) body.set("tools", std::move(tools));

    // messages
    json::Value msgs = json::Value::array();
    for (auto& m : messages) {
      json::Value content = json::Value::array();
      for (auto& it : m.items) {
        if (it.type == prov::ContentItem::Text) {
          json::Value b = json::Value::object();
          b.set("type", "text");
          b.set("text", it.text);
          content.push(std::move(b));
        } else if (it.type == prov::ContentItem::ToolUse) {
          json::Value b = json::Value::object();
          b.set("type", "tool_use");
          b.set("id", it.id);
          b.set("name", it.name);
          b.set("input", it.input.isNull() ? json::Value::object() : it.input);
          content.push(std::move(b));
        } else if (it.type == prov::ContentItem::ToolResult) {
          json::Value b = json::Value::object();
          b.set("type", "tool_result");
          b.set("tool_use_id", it.tool_use_id);
          b.set("content", it.text);
          b.set("is_error", it.is_error);
          content.push(std::move(b));
        }
      }
      json::Value mm = json::Value::object();
      mm.set("role", m.role);
      mm.set("content", std::move(content));
      msgs.push(std::move(mm));
    }
    body.set("messages", std::move(msgs));

    std::string payload = json::serialize(body);

    http::Headers headers;
    headers["x-api-key"] = cfg_.api_key;
    headers["anthropic-version"] = "2023-06-01";
    headers["content-type"] = "application/json";

    bool inTool = false;
    std::string toolId, toolName, toolInput;
    long inTokens = 0, outTokens = 0;

    sse::Parser parser([&](const std::string& name, const std::string& data) {
      if (data.empty()) return;
      json::Value ev;
      try { ev = json::parse(data); } catch (...) { return; }
      std::string type = ev.getStr("type");
      if (type == "message_start") {
        auto m = ev.get("message", json::Value::object());
        auto u = m.get("usage", json::Value::object());
        inTokens = u.getLong("input_tokens", 0);
      } else if (type == "content_block_start") {
        auto cb = ev.get("content_block", json::Value::object());
        if (cb.getStr("type") == "tool_use") {
          inTool = true;
          toolId = cb.getStr("id");
          toolName = cb.getStr("name");
          toolInput.clear();
          on_event(prov::StreamEvent::toolStart(toolId, toolName));
        }
      } else if (type == "content_block_delta") {
        auto d = ev.get("delta", json::Value::object());
        std::string dt = d.getStr("type");
        if (dt == "text_delta") {
          on_event(prov::StreamEvent::text_delta(d.getStr("text")));
        } else if (dt == "input_json_delta") {
          toolInput += d.getStr("partial_json");
          on_event(prov::StreamEvent::toolInput(d.getStr("partial_json")));
        }
      } else if (type == "content_block_stop") {
        if (inTool) {
          inTool = false;
          // 完整参数 JSON 已由各 input_json_delta 片段累积到 toolInput，
          // agent 在收到 toolEnd 时把累积片段解析为 input。
          on_event(prov::StreamEvent::toolEnd());
        }
      } else if (type == "message_delta") {
        auto u = ev.get("usage", json::Value::object());
        outTokens = u.getLong("output_tokens", outTokens);
      } else if (type == "message_stop") {
        on_event(prov::StreamEvent::msgEnd(inTokens, outTokens));
      } else if (type == "error") {
        auto e = ev.get("error", json::Value::object());
        on_event(prov::StreamEvent::err(e.getStr("message", "anthropic error")));
      }
    });

    long status = 0;
    std::string full;
    bool ok = http::post_stream(cfg_.base_url, headers, payload,
        [&](const std::string& chunk) { parser.feed(chunk); full += chunk; },
        err, &status);

    if (!ok) return;
    parser.flush();

    if (status >= 400) {
      // 非 2xx：尝试从返回体解析错误信息
      std::string msg = "HTTP " + std::to_string(status);
      try {
        json::Value e = json::parse(full);
        if (e.has("error")) {
          auto er = e.get("error", json::Value::object());
          msg = er.getStr("type", "") + ": " + er.getStr("message", msg);
        } else if (e.has("type") && e.getStr("type") == "error") {
          msg = e.getStr("message", msg);
        }
      } catch (...) {}
      err = msg;
      on_event(prov::StreamEvent::err(msg));
    }
  }

private:
  cfg::Config cfg_;
};

}  // namespace

std::unique_ptr<prov::Provider> make_anthropic(const cfg::Config& c) {
  return std::make_unique<AnthropicProvider>(c);
}
