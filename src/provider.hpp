// provider.hpp - 统一的内部消息 / 工具 / 流式事件模型 + Provider 抽象
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "json.hpp"
#include "tools.hpp"
#include "config.hpp"

namespace prov {

// 一条内容项（消息由若干内容项组成）
struct ContentItem {
  enum Type { Text, ToolUse, ToolResult } type = Text;
  // Text
  std::string text;
  // ToolUse（assistant 发起）
  std::string id;
  std::string name;
  json::Value input;        // 解析后的 JSON 参数
  // ToolResult（user 回传）
  std::string tool_use_id;
  bool is_error = false;
};

struct Message {
  std::string role;                 // "user" | "assistant"
  std::vector<ContentItem> items;
};

// 流式事件（Agent 循环消费）
struct StreamEvent {
  enum Type {
    TextDelta,        // 增量文本 -> text
    ToolUseStart,     // 工具调用开始 -> id, name
    ToolInputDelta,   // 工具参数增量 JSON -> text
    ToolUseEnd,       // 工具调用结束（参数已收齐）
    MessageEnd,       // 消息结束 -> input_tokens, output_tokens
    Error             // 错误 -> text
  } type;
  std::string text;
  std::string id;
  std::string name;
  long input_tokens = 0;
  long output_tokens = 0;

  static StreamEvent text_delta(const std::string& t) { StreamEvent e; e.type = TextDelta; e.text = t; return e; }
  static StreamEvent toolStart(const std::string& id, const std::string& n) { StreamEvent e; e.type = ToolUseStart; e.id = id; e.name = n; return e; }
  static StreamEvent toolInput(const std::string& t) { StreamEvent e; e.type = ToolInputDelta; e.text = t; return e; }
  static StreamEvent toolEnd() { StreamEvent e; e.type = ToolUseEnd; return e; }
  static StreamEvent msgEnd(long in, long out) { StreamEvent e; e.type = MessageEnd; e.input_tokens = in; e.output_tokens = out; return e; }
  static StreamEvent err(const std::string& t) { StreamEvent e; e.type = Error; e.text = t; return e; }
};

class Provider {
public:
  virtual ~Provider() = default;
  // 以流式方式对话。on_event 逐事件回调，出错时 err 非空且可能先发一个 Error 事件
  virtual void stream_chat(
      const std::string& system,
      const std::vector<Message>& messages,
      const std::vector<tools::ToolDef>& tooldefs,
      const std::function<void(const StreamEvent&)>& on_event,
      std::string& err) = 0;
};

std::unique_ptr<Provider> make_provider(const cfg::Config& c);

}  // namespace prov
