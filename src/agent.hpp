// agent.hpp - 多轮对话编排 + 工具调用循环
#pragma once
#include <string>
#include <vector>
#include "provider.hpp"
#include "config.hpp"

namespace agent {

// 事件钩子：REPL/终端用默认实现（打印到 stdout），WebUI 注入自己的回调
struct Hooks {
  // 一段流式输出开始（每轮请求/每次工具循环迭代开始时触发；
  // 用于重置 <think> 过滤器等流级状态）
  std::function<void()> on_stream_start;
  // 文本增量
  std::function<void(const std::string& delta)> on_text;
  // 工具调用开始（参数尚未收齐）
  std::function<void(const std::string& id, const std::string& name)> on_tool_start;
  // 工具参数收齐（JSON 字符串）
  std::function<void(const std::string& id, const std::string& args_json)> on_tool_args;
  // run_command 逐行实时输出
  std::function<void(const std::string& id, const std::string& line)> on_tool_line;
  // 工具执行完毕
  std::function<void(const std::string& id, const std::string& content, bool is_error)> on_tool_result;
  // 一轮消息结束（token 统计）
  std::function<void(long in_tokens, long out_tokens)> on_message_end;
  // 错误
  std::function<void(const std::string& text)> on_error;
  // run_command 确认：返回 true 允许执行
  std::function<bool(const std::string& command)> confirm;
};

// 默认的终端钩子（保持原 stdout 彩色输出行为）
Hooks terminal_hooks();

class Agent {
public:
  explicit Agent(cfg::Config& cfg);

  // 处理一轮用户输入（含工具循环），返回是否成功
  bool chat(const std::string& user_text);

  // 清空对话历史
  void reset();

  // 历史消息数（用于 UI 显示）
  size_t history_size() const { return history_.size(); }

  // 完整历史（供 WebUI 重连/刷新时回放）
  const std::vector<prov::Message>& history() const { return history_; }

  // 恢复历史（/resume 从会话文件载入）
  void set_history(std::vector<prov::Message> h) { history_ = std::move(h); }

  // 替换事件钩子（默认为终端输出）
  void set_hooks(Hooks h) { hooks_ = std::move(h); }

private:
  std::string build_system_prompt() const;

  cfg::Config& cfg_;
  std::vector<prov::Message> history_;
  std::unique_ptr<prov::Provider> provider_;
  long total_in_ = 0, total_out_ = 0;
  Hooks hooks_;
};

}  // namespace agent
