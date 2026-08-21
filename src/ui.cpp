// ui.cpp - 交互式菜单/输入组件实现
// 交互分支使用 Win32 原生控制台 API（SetConsoleCursorPosition + _getch），
// 不依赖 VT 序列，方向键在任何真实 Windows 控制台下都可靠。
#include "ui.hpp"
#include "utils.hpp"
#include <iostream>
#include <algorithm>

#ifdef _WIN32
  #include <windows.h>
  #include <conio.h>   // _getch
  #include <io.h>      // _isatty, _fileno
#else
  #include <unistd.h>  // isatty
#endif

namespace ui {

static HANDLE g_in  = INVALID_HANDLE_VALUE;
static HANDLE g_out = INVALID_HANDLE_VALUE;
static bool   g_console = false;

static void detect_console() {
#ifdef _WIN32
  g_in  = GetStdHandle(STD_INPUT_HANDLE);
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD m = 0;
  g_console =
      g_in  != INVALID_HANDLE_VALUE && g_in  != nullptr &&
      g_out != INVALID_HANDLE_VALUE && g_out != nullptr &&
      _isatty(_fileno(stdin)) != 0 &&
      GetConsoleMode(g_in, &m) && GetConsoleMode(g_out, &m);
#else
  g_console = isatty(0) != 0;
#endif
}

bool interactive() {
  detect_console();
  return g_console;
}

// 用空格覆盖从 start 开始的 max_lines 行，然后光标移回 start（Win32 原生，无闪烁）
static void clear_region(COORD start, int max_lines) {
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO cbi;
  if (!GetConsoleScreenBufferInfo(g_out, &cbi)) return;
  int width = cbi.dwSize.X;
  DWORD written = 0;
  for (int i = 0; i < max_lines; i++) {
    COORD p = start;
    p.Y += (SHORT)i;
    FillConsoleOutputCharacterW(g_out, L' ', (DWORD)width, p, &written);
  }
  SetConsoleCursorPosition(g_out, start);
#else
  (void)start; (void)max_lines;
  std::cout << "\x1b[2J\x1b[H";
#endif
}

static std::string to_lower(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return r;
}

int select_menu(const std::string& title, const std::vector<MenuItem>& items, int def) {
  using namespace util::color;
  if (items.empty()) return -1;
  int sel = (def >= 0 && def < (int)items.size()) ? def : 0;
  detect_console();

  // ---- 非交互：打印列表，读取序号 ----
  if (!g_console) {
    std::cout << "\n" << title << "\n";
    for (size_t i = 0; i < items.size(); i++) {
      std::cout << "  " << cyan << "[" << (i + 1) << "]" << reset
                << " " << items[i].label;
      if (!items[i].hint.empty())
        std::cout << "  " << gray << items[i].hint << reset;
      std::cout << "\n";
    }
    std::cout << gray << "  输入序号 (1-" << items.size() << "，回车默认 " << (sel + 1) << "): " << reset;
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return sel;
    std::string t = util::trim(line);
    if (t.empty()) return sel;
    try {
      int n = std::stoi(t);
      if (n >= 1 && n <= (int)items.size()) return n - 1;
    } catch (...) {}
    std::string low = to_lower(t);
    for (size_t i = 0; i < items.size(); i++) {
      if (to_lower(items[i].label).find(low) != std::string::npos) return (int)i;
    }
    return sel;
  }

  // ---- 交互：原生控制台重绘菜单（上下键/数字） ----
  std::cout << "\n";
  std::cout.flush();
  CONSOLE_SCREEN_BUFFER_INFO cbi;
  if (!GetConsoleScreenBufferInfo(g_out, &cbi)) {
    // 极端情况下退化：静态打印 + 序号输入
    std::cout << title << "\n";
    for (size_t i = 0; i < items.size(); i++)
      std::cout << "  " << (i + 1) << ". " << items[i].label
                << (items[i].hint.empty() ? "" : "  " + items[i].hint) << "\n";
    std::cout.flush();
    std::string line;
    std::getline(std::cin, line);
    try { int n = std::stoi(util::trim(line)); if (n >= 1 && n <= (int)items.size()) return n - 1; }
    catch (...) {}
    return sel;
  }
  COORD start = cbi.dwCursorPosition;
  // 覆盖行数按 2 倍估算，容忍窄终端折行
  int max_lines = (int)(items.size() * 2 + 12);
  bool first = true;

  bool in_num = false;
  int num = 0;

  for (;;) {
    if (!first) clear_region(start, max_lines);
    first = false;

    std::cout << title << "\n";
    std::cout << cyan << "  " << std::string(52, '-') << "\n" << reset;

    for (size_t i = 0; i < items.size(); i++) {
      bool cur = ((int)i == sel);
      std::string numstr = "[" + std::to_string(i + 1) + "]";
      if (items.size() >= 10 && i + 1 < 10) numstr = "[ " + std::to_string(i + 1) + "]";
      std::cout << (cur ? cyan : gray) << (cur ? "  > " : "    ") << reset;
      std::cout << gray << numstr << reset;
      if (cur) std::cout << bold << cyan;
      std::cout << " " << items[i].label << reset;
      if (!items[i].hint.empty()) {
        std::string h = items[i].hint;
        if (h.size() > 44) h = h.substr(0, 41) + "...";
        std::cout << "  " << gray << h << reset;
      }
      std::cout << "\n";
    }

    if (in_num) {
      std::cout << gray << dim << "  数字: " << num
                << "  (Enter 确认 / Esc 取消)" << reset << "\n";
    } else {
      std::cout << gray << dim << "  ↑/↓ 或 w/s 选择   Enter 确认   Esc 取消   数字直选"
                << reset << "\n";
    }
    std::cout << "\n";
    std::cout.flush();

    int c = _getch();
    if (in_num) {
      if (c >= '0' && c <= '9') {
        num = num * 10 + (c - '0');
        if (num > (int)items.size()) num = (int)items.size();
      } else if (c == 13 || c == '\n') {
        if (num >= 1 && num <= (int)items.size()) return num - 1;
        in_num = false;
      } else if (c == 27) {
        in_num = false;
      } else if (c == 3) {
        return -1;
      }
      continue;
    }
    if (c == 0 || c == 224) {  // 方向键/功能键前缀
      c = _getch();
      if (c == 72)      sel = (sel - 1 + (int)items.size()) % (int)items.size();  // ↑
      else if (c == 80) sel = (sel + 1) % (int)items.size();                      // ↓
    } else if (c == 'w' || c == 'k') sel = (sel - 1 + (int)items.size()) % (int)items.size();
    else if (c == 's' || c == 'j') sel = (sel + 1) % (int)items.size();
    else if (c == 13 || c == '\n') return sel;                 // Enter
    else if (c == 27) return -1;                               // Esc
    else if (c == 3)  return -1;                               // Ctrl+C
    else if (c >= '1' && c <= '9') { in_num = true; num = c - '0'; }
  }
}

std::string input(const std::string& prompt, const std::string& def) {
  using namespace util::color;
  std::cout << "  " << prompt;
  if (!def.empty()) std::cout << gray << " (默认: " << def << ")" << reset;
  std::cout << "\n  " << cyan << "> " << reset;
  std::cout.flush();
  std::string line;
  if (!std::getline(std::cin, line)) return def;
  std::string t = util::trim(line);
  return t.empty() ? def : t;
}

bool confirm(const std::string& prompt, bool def) {
  using namespace util::color;
  detect_console();

  // ---- 非交互：读 y/n 行，空回车取默认 ----
  if (!g_console) {
    std::cout << yellow << prompt << (def ? " [Y/n] " : " [y/N] ") << reset;
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return def;
    std::string t = util::trim(to_lower(line));
    if (t.empty()) return def;
    if (t == "y" || t == "yes") return true;
    if (t == "n" || t == "no") return false;
    return def;
  }

  // ---- 交互：方向键选择 同意/拒绝 ----
  bool sel = def;
  std::cout << "\n";
  std::cout.flush();
  CONSOLE_SCREEN_BUFFER_INFO cbi;
  if (!GetConsoleScreenBufferInfo(g_out, &cbi)) {
    std::cout << yellow << prompt << " [y/N] " << reset;
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return def;
    std::string t = util::trim(to_lower(line));
    if (t == "y" || t == "yes") return true;
    return false;
  }
  COORD start = cbi.dwCursorPosition;
  const int max_lines = 9;
  bool first = true;

  for (;;) {
    if (!first) clear_region(start, max_lines);
    first = false;

    std::cout << yellow << "⚠ " << prompt << reset << "\n";
    std::cout << cyan << "  " << std::string(52, '-') << "\n" << reset;

    auto row = [&](const char* label, const char* hint, bool cur) {
      std::cout << (cur ? cyan : gray) << (cur ? "  > " : "    ") << reset;
      if (cur) std::cout << bold << green;
      std::cout << label << reset;
      std::cout << "  " << gray << hint << reset << "\n";
    };
    row("同意 (允许执行)",  "Enter 确认", sel);
    row("拒绝 (取消命令)",  "Esc 拒绝", !sel);

    std::cout << gray << dim << "  ↑/↓ 或 w/s 选择   Enter 确认   Esc 拒绝"
              << reset << "\n";
    std::cout << "\n";
    std::cout.flush();

    int c = _getch();
    if (c == 0 || c == 224) {          // 方向键/功能键前缀
      c = _getch();
      if (c == 72 || c == 80) sel = !sel;   // ↑ / ↓
    } else if (c == 'w' || c == 's' || c == 'k' || c == 'j') {
      sel = !sel;
    } else if (c == 13 || c == '\n') {
      return sel;
    } else if (c == 27 || c == 3) {
      return false;                     // Esc / Ctrl+C 一律拒绝
    } else if (c == 'y' || c == 'Y') {
      return true;
    } else if (c == 'n' || c == 'N') {
      return false;
    }
  }
}

}  // namespace ui
