// repl.cpp
#include "repl.hpp"
#include "agent.hpp"
#include "setup.hpp"
#include "session.hpp"
#include "tools.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "version.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace repl {

static void banner(const cfg::Config& cfg) {
  // 顶部分隔线
  std::string line(58, '-');
  std::cout << util::color::cyan << "  " << line << "\n" << util::color::reset;

  // ASCII Logo - openaidd
  std::cout << util::color::bold << util::color::cyan
            << "        _ __                         _  _ __   _ __  \n"
            << "  ___  | '_ \\   ___   _ __     __ _ (_)| '_ \\ | '_ \\ \n"
            << " / _ \\ | |_) | / _ \\ | '_ \\   / _` || || | | || | | |\n"
            << "| | | || .__/ |  __/ | | | | | (_| || || | | || | | |\n"
            << "| |_| ||_|     \\___| | | | |  \\__,_||_||_| |_||_| |_|\n"
            << " \\___/               |_| |_|                        \n"
            << util::color::bold << util::color::green
            << "                                                  v" << OPENAIDD_VERSION << "\n"
            << util::color::reset;

  std::cout << util::color::cyan << "  " << line << "\n" << util::color::reset;

  // 状态信息
  std::cout << util::color::gray
            << "  provider  " << util::color::green << cfg.provider << "\n"
            << util::color::gray
            << "  model     " << util::color::green << cfg.model << "\n"
            << util::color::gray
            << "  tools     " << util::color::green << tools::all_tools().size() << " (read/write/edit/list/grep/run)\n"
            << util::color::gray
            << "  approve   " << util::color::green
            << (cfg.auto_approve_commands ? "all (免确认)" : "ask (命令需确认)") << "\n"
            << util::color::gray
            << "  cwd      " << util::color::green << cfg.cwd << "\n"
            << util::color::gray
            << "  workspace " << util::color::green << cfg.workspace << "\n"
            << util::color::reset;

  std::cout << util::color::cyan << "  " << line << "\n" << util::color::reset;

  std::cout << util::color::yellow
            << "  输入 /help 查看命令，直接输入消息开始对话。\n"
            << util::color::reset;
}

static void help() {
  std::cout << util::color::bold << "命令：\n" << util::color::reset;
  std::cout << "  /help              显示帮助\n";
  std::cout << "  /resume            查看并恢复历史会话（/resume last 直接恢复最近一次）\n";
  std::cout << "  /clear             清空对话历史\n";
  std::cout << "  /model <名称>      切换模型（如 claude-sonnet-4-20250514 / gpt-4o）\n";
  std::cout << "  /provider <p>      切换后端：anthropic | openai\n";
  std::cout << "  /approve on|off    开启/关闭 run_command 免确认\n";
  std::cout << "  /allaccept          全部开放权限：所有工具免确认直接执行\n";
  std::cout << "  /save              保存当前配置到 " << cfg::config_path() << "\n";
  std::cout << "  /return            重新进入配置向导（厂商/模型/Key）\n";
  std::cout << "  /exit, /quit       退出\n";
  std::cout << "其它输入视为用户消息，直接发送给模型。\n";
}

void run(cfg::Config& cfg) {
  banner(cfg);
  auto agent = std::make_unique<agent::Agent>(cfg);
  bool multiline = false;

  // 会话持久化：首轮对话后创建会话文件，之后每轮结束整写覆盖
  std::string cur_session;
  auto persist = [&]() {
    if (agent->history().empty()) return;
    if (cur_session.empty())
      cur_session = session::new_session_file();
    if (!session::save(cur_session, cfg.model, agent->history())) {
      std::cout << util::color::red << "[警告] 会话保存失败: " << cur_session
                << util::color::reset << "\n";
    }
  };

  auto do_resume = [&](const session::Meta& m) {
    std::vector<prov::Message> hist;
    std::string model;
    if (!session::load(m.file, hist, model)) {
      std::cout << util::color::red << "会话文件读取失败: " << m.file
                << util::color::reset << "\n";
      return;
    }
    agent->set_history(std::move(hist));
    cur_session = m.file;
    std::cout << util::color::green << "已恢复会话 " << m.id
              << util::color::reset << util::color::gray
              << "（" << m.turns << " 轮对话"
              << (m.model.empty() ? "" : "，模型 " + m.model)
              << (m.saved_at.empty() ? "" : "，保存于 " + m.saved_at)
              << "），可继续对话。\n" << util::color::reset;
  };

  auto resume_interactive = [&]() {
    auto sessions = session::list();
    if (sessions.empty()) {
      std::cout << util::color::gray << "没有历史会话（保存在 "
                << session::sessions_dir() << "）\n"
                << util::color::reset;
      return;
    }
    std::cout << util::color::bold << "历史会话（最新在前）：\n" << util::color::reset;
    for (size_t i = 0; i < sessions.size(); i++) {
      const auto& s = sessions[i];
      std::cout << "  " << util::color::cyan << (i + 1) << util::color::reset
                << ". " << util::color::bold << s.id << util::color::reset
                << util::color::gray << "  " << s.turns << " 轮"
                << (s.saved_at.empty() ? "" : "  " + s.saved_at) << "\n"
                << "     " << util::color::gray
                << (s.preview.empty() ? "(无预览)" : s.preview) << "\n"
                << util::color::reset;
    }
    std::cout << util::color::yellow << "输入编号恢复（直接回车取消）: "
              << util::color::reset;
    std::cout.flush();
    std::string pick;
    if (!std::getline(std::cin, pick)) return;
    pick = util::trim(pick);
    if (pick.empty()) return;
    char* end = nullptr;
    long n = strtol(pick.c_str(), &end, 10);
    if (end && *end == '\0' && n >= 1 && (size_t)n <= sessions.size()) {
      do_resume(sessions[(size_t)n - 1]);
    } else {
      std::cout << util::color::gray << "已取消。\n" << util::color::reset;
    }
  };

  auto rebuild = [&]() {
    auto hist = agent->history();   // 切换模型/后端时保留对话
    agent = std::make_unique<agent::Agent>(cfg);
    agent->set_history(std::move(hist));
  };

  std::string line;
  while (true) {
    std::cout << util::color::cyan << (multiline ? "… " : "❯ ") << util::color::reset;
    std::cout.flush();
    if (!std::getline(std::cin, line)) break;  // Ctrl+Z / EOF

    if (multiline && !line.empty()) {
      // 继续累积，直到空行
      std::string buf = line;
      std::string more;
      while (true) {
        std::cout << util::color::cyan << "… " << util::color::reset;
        std::cout.flush();
        if (!std::getline(std::cin, more)) { buf += "\n"; break; }
        if (more.empty()) break;
        buf += "\n" + more;
      }
      line = buf;
    }

    std::string trimmed = util::trim(line);
    if (trimmed.empty()) continue;

    if (trimmed[0] == '/') {
      std::vector<std::string> parts = util::split(trimmed, ' ');
      std::string cmd = parts[0];
      if (cmd == "/exit" || cmd == "/quit") {
        break;
      } else if (cmd == "/help" || cmd == "/?") {
        help();
      } else if (cmd == "/resume") {
        auto sessions = session::list();
        if (parts.size() >= 2 && parts[1] == "last") {
          if (sessions.empty()) {
            std::cout << util::color::gray << "没有历史会话。\n" << util::color::reset;
          } else {
            do_resume(sessions[0]);
          }
        } else if (parts.size() >= 2 && parts[1].find_first_not_of("0123456789") == std::string::npos) {
          long n = strtol(parts[1].c_str(), nullptr, 10);
          if (n >= 1 && (size_t)n <= sessions.size()) {
            do_resume(sessions[(size_t)n - 1]);
          } else {
            std::cout << util::color::red << "编号超出范围（共 "
                      << sessions.size() << " 个会话）。\n" << util::color::reset;
          }
        } else {
          resume_interactive();
        }
      } else if (cmd == "/clear") {
        agent->reset();
        cur_session.clear();   // 下次对话开启新会话
        std::cout << util::color::gray << "对话历史已清空。\n" << util::color::reset;
      } else if (cmd == "/model") {
        if (parts.size() < 2) { std::cout << "用法: /model <名称>\n"; continue; }
        cfg.model = parts[1];
        rebuild();
        cfg::save(cfg);
        std::cout << util::color::gray << "已切换模型: " << cfg.model << "\n" << util::color::reset;
      } else if (cmd == "/provider") {
        if (parts.size() < 2 ||
            (parts[1] != "anthropic" && parts[1] != "openai")) {
          std::cout << "用法: /provider anthropic|openai\n"; continue;
        }
        cfg.provider = parts[1];
        if (cfg.base_url.empty())
          cfg.base_url = cfg.is_anthropic()
              ? "https://api.anthropic.com/v1/messages"
              : "https://api.openai.com/v1/chat/completions";
        rebuild();
        cfg::save(cfg);
        std::cout << util::color::gray << "已切换后端: " << cfg.provider
                  << " (model=" << cfg.model << ")\n" << util::color::reset;
      } else if (cmd == "/approve") {
        if (parts.size() < 2) { std::cout << "用法: /approve on|off\n"; continue; }
        cfg.auto_approve_commands = (parts[1] == "on");
        std::cout << util::color::gray << "run_command 免确认: "
                  << (cfg.auto_approve_commands ? "开" : "关") << "\n" << util::color::reset;
      } else if (cmd == "/allaccept") {
        cfg.auto_approve_commands = true;
        cfg::save(cfg);
        std::cout << util::color::green << "已开启全部工具免确认（all accept）——后续命令直接执行，不再询问。\n"
                  << util::color::gray << "可用 /approve off 或 /return 重新开启确认。\n" << util::color::reset;
      } else if (cmd == "/save") {
        cfg::save(cfg);
        std::cout << util::color::gray << "已保存到 " << cfg::config_path() << "\n" << util::color::reset;
      } else if (cmd == "/return") {
        std::cout << "\n";
        if (setup::first_run(cfg)) {
          rebuild();
          std::cout << util::color::green << "配置已更新，重新加载完成。\n" << util::color::reset;
        } else {
          std::cout << util::color::gray << "已取消，保持原配置。\n" << util::color::reset;
        }
      } else if (cmd == "/multiline") {
        multiline = !multiline;
        std::cout << util::color::gray << "多行模式: " << (multiline ? "开" : "关") << "\n" << util::color::reset;
      } else {
        std::cout << util::color::red << "未知命令: " << cmd << "（/help 查看）\n" << util::color::reset;
      }
      continue;
    }

    agent->chat(line);
    persist();   // 每轮结束落盘，随时可 /resume 恢复
  }
  std::cout << util::color::gray << "再见。\n" << util::color::reset;
}

}  // namespace repl
