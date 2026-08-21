// agent.cpp
#include "agent.hpp"
#include "tools.hpp"
#include "ui.hpp"
#include "utils.hpp"
#include "think.hpp"
#include <iostream>
#include <memory>
#include <sstream>

namespace agent {

// ---------- 默认终端钩子（保持原 stdout 彩色输出） ----------
Hooks terminal_hooks() {
  Hooks h;
  // <think> 过滤器：标签隐藏；思考内容用黑色显示，正文绿色
  auto filter = std::make_shared<util::ThinkFilter>(
      [](const std::string& t, bool think) {
        std::cout << (think ? util::color::black : util::color::green)
                  << t << util::color::reset;
        std::cout.flush();
      });
  h.on_stream_start = [filter]() { filter->reset(); };
  h.on_text = [filter](const std::string& t) {
    filter->feed(t);
  };
  h.on_tool_start = [filter](const std::string& id, const std::string& name) {
    filter->flush();   // 流转工具调用，吐出残留缓冲
    std::cout << "\n" << util::color::yellow << "⏺ " << name
              << " (" << id << ")" << util::color::reset << "\n";
    std::cout.flush();
  };
  h.on_tool_args = [](const std::string&, const std::string&) {};
  h.on_tool_line = [](const std::string&, const std::string& line) {
    std::cout << line;   // 命令输出实时上屏
    std::cout.flush();
  };
  h.on_tool_result = [](const std::string&, const std::string& content, bool) {
    // 结果截断显示，避免刷屏
    std::string shown = content;
    const size_t kMaxShow = 4000;
    if (shown.size() > kMaxShow) {
      shown = shown.substr(0, kMaxShow) + "\n... (结果已截断，共 " +
              std::to_string(content.size()) + " 字节)";
    }
    std::cout << util::color::gray << shown << util::color::reset << "\n";
    std::cout.flush();
  };
  h.on_message_end = [filter](long inTok, long outTok) {
    filter->flush();
    std::cout << util::color::gray << "\n[" << inTok << "↑ / " << outTok
              << "↓ tokens]" << util::color::reset << "\n";
    std::cout.flush();
  };
  h.on_error = [filter](const std::string& t) {
    filter->flush();
    std::cout << "\n" << util::color::red << "[错误] " << t
              << util::color::reset << "\n";
    std::cout.flush();
  };
  h.confirm = [](const std::string& cmd) {
    return ui::confirm("是否执行命令? " + cmd, false);
  };
  return h;
}

Agent::Agent(cfg::Config& cfg)
    : cfg_(cfg), provider_(prov::make_provider(cfg)), hooks_(terminal_hooks()) {}

void Agent::reset() {
  history_.clear();
  total_in_ = total_out_ = 0;
}

std::string Agent::build_system_prompt() const {
  std::ostringstream s;
  s << "你是一个运行在终端中的编程智能体（类似 Claude Code）。"
    << "当前系统：Windows（MinGW）。工作目录：" << cfg_.cwd << "。\n";
  s << "当前时间：" << util::now_string() << "。\n\n";
  s << "能力：你可以通过工具读取/搜索/编辑文件、执行 shell 命令，从而真正地帮用户完成软件工程任务。\n";
  s << "准则：\n";
  s << "1. 能用工具核实的，不要凭空猜测文件路径或命令结果。\n";
  s << "2. 优先用精确的小改动（edit_file）而非整文件重写（write_file）。\n";
  s << "3. 修改代码前先读取相关文件，理解上下文。\n";
  s << "4. 回答简洁、面向行动；需要执行命令或改文件时直接调用工具。\n";
  s << "5. 当 run_command 涉及有风险的操作时谨慎；普通构建/查询可直接执行。\n";
  s << "6. 用中文回复用户。\n";
  if (!cfg_.extra_system.empty()) s << "\n" << cfg_.extra_system << "\n";
  return s.str();
}

bool Agent::chat(const std::string& user_text) {
  // 1. 压入用户消息
  prov::Message um;
  um.role = "user";
  prov::ContentItem t;
  t.type = prov::ContentItem::Text;
  t.text = user_text;
  um.items.push_back(std::move(t));
  history_.push_back(std::move(um));

  const int kMaxIter = 25;
  bool ok = true;

  for (int iter = 0; iter < kMaxIter; iter++) {
    // 2. 调用模型（流式）
    std::string system = build_system_prompt();
    auto tools = tools::all_tools();

    prov::Message assistant;
    assistant.role = "assistant";
    int curTextIdx = -1;
    int curToolIdx = -1;
    std::string curToolBuf;
    bool hadError = false;
    long inTok = 0, outTok = 0;

    if (hooks_.on_stream_start) hooks_.on_stream_start();

    std::string err;
    provider_->stream_chat(system, history_, tools,
        [&](const prov::StreamEvent& e) {
          switch (e.type) {
            case prov::StreamEvent::TextDelta: {
              if (curTextIdx < 0 || assistant.items[curTextIdx].type != prov::ContentItem::Text) {
                prov::ContentItem it;
                it.type = prov::ContentItem::Text;
                assistant.items.push_back(std::move(it));
                curTextIdx = (int)assistant.items.size() - 1;
              }
              assistant.items[curTextIdx].text += e.text;
              if (hooks_.on_text) hooks_.on_text(e.text);
              break;
            }
            case prov::StreamEvent::ToolUseStart: {
              prov::ContentItem it;
              it.type = prov::ContentItem::ToolUse;
              it.id = e.id;
              it.name = e.name;
              assistant.items.push_back(std::move(it));
              curToolIdx = (int)assistant.items.size() - 1;
              curToolBuf.clear();
              if (hooks_.on_tool_start) hooks_.on_tool_start(e.id, e.name);
              break;
            }
            case prov::StreamEvent::ToolInputDelta: {
              if (curToolIdx >= 0) curToolBuf += e.text;
              break;
            }
            case prov::StreamEvent::ToolUseEnd: {
              if (curToolIdx >= 0) {
                try {
                  assistant.items[curToolIdx].input =
                      curToolBuf.empty() ? json::Value::object() : json::parse(curToolBuf);
                } catch (...) {
                  assistant.items[curToolIdx].input = json::Value::object();
                }
                if (hooks_.on_tool_args) {
                  std::string args = curToolBuf.empty()
                      ? "{}" : curToolBuf;
                  hooks_.on_tool_args(assistant.items[curToolIdx].id, args);
                }
              }
              break;
            }
            case prov::StreamEvent::MessageEnd: {
              inTok = e.input_tokens;
              outTok = e.output_tokens;
              break;
            }
            case prov::StreamEvent::Error: {
              hadError = true;
              if (hooks_.on_error) hooks_.on_error(e.text);
              break;
            }
          }
        }, err);

    if (!err.empty()) {
      ok = false;
      if (!hadError && hooks_.on_error)
        hooks_.on_error("[请求失败] " + err);
    }

    total_in_ += inTok;
    total_out_ += outTok;

    // 3. 压入 assistant 消息（即便为空也保留，便于继续）
    bool hasToolUse = false;
    for (auto& it : assistant.items)
      if (it.type == prov::ContentItem::ToolUse) hasToolUse = true;
    history_.push_back(std::move(assistant));

    if (hadError) break;
    if (!hasToolUse) {
      // 文本回复完成
      if (hooks_.on_message_end) hooks_.on_message_end(inTok, outTok);
      break;
    }

    // 4. 执行工具，构造 tool_result 用户消息
    prov::Message toolMsg;
    toolMsg.role = "user";
    // 取出刚压入的 assistant 消息里的 tool_use
    prov::Message& lastAssistant = history_.back();
    for (auto& tu : lastAssistant.items) {
      if (tu.type != prov::ContentItem::ToolUse) continue;

      // run_command 确认（终端方向键选择 / 网页按钮）
      if (tu.name == "run_command" && !cfg_.auto_approve_commands) {
        std::string cmd = tu.input.getStr("command");
        bool approved = hooks_.confirm ? hooks_.confirm(cmd) : true;
        if (!approved) {
          prov::ContentItem r;
          r.type = prov::ContentItem::ToolResult;
          r.tool_use_id = tu.id;
          r.is_error = true;
          r.text = "用户取消了命令执行。";
          toolMsg.items.push_back(std::move(r));
          continue;
        }
      }

      // run_command 逐行实时显示，其余工具执行后一次性显示
      tools::ToolResult res;
      if (tu.name == "run_command") {
        res = tools::execute(tu.name, tu.input, cfg_.cwd,
            [&](const std::string& line) {
              if (hooks_.on_tool_line) hooks_.on_tool_line(tu.id, line);
            });
        // 输出已实时显示，这里只补一行退出码
        std::string ec = res.content;
        size_t nl = ec.find('\n');
        if (nl != std::string::npos) ec = ec.substr(0, nl);
        std::cout << util::color::gray << "  ↳ " << ec << util::color::reset << "\n";
        std::cout.flush();
      } else {
        res = tools::execute(tu.name, tu.input, cfg_.cwd);
      }

      if (hooks_.on_tool_result)
        hooks_.on_tool_result(tu.id, res.content, res.is_error);

      prov::ContentItem r;
      r.type = prov::ContentItem::ToolResult;
      r.tool_use_id = tu.id;
      r.is_error = res.is_error;
      r.text = res.content;
      toolMsg.items.push_back(std::move(r));
    }
    history_.push_back(std::move(toolMsg));
    // 继续循环，让模型基于工具结果继续
  }

  return ok;
}

}  // namespace agent
