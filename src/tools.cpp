// tools.cpp - 工具定义与执行（零依赖；目录遍历用 Win32 API）
#include "tools.hpp"
#include "utils.hpp"
#include <windows.h>
#include <functional>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

namespace tools {

static bool is_absolute(const std::string& p) {
  if (p.size() >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
  if (!p.empty() && p[0] == '/') return true;
  if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') return true;  // UNC
  return false;
}

static std::string resolve(const std::string& cwd, const std::string& p) {
  if (is_absolute(p)) return p;
  if (cwd.empty()) return p;
  return cwd + "/" + p;
}

static std::string to_forward_slash(const std::string& p) {
  std::string r = p;
  for (char& c : r) if (c == '\\') c = '/';
  return r;
}

// 递归遍历目录；cb(fullUtf8, isDir)。skipDir(name) 返回 true 时跳过该子目录。
static void walk(const std::string& dir,
                 const std::function<void(const std::string&, bool)>& cb,
                 const std::function<bool(const std::string&)>& skipDir = nullptr) {
  std::wstring wdir = util::utf8_to_wide(dir);
  std::wstring pat = wdir + L"\\*";
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pat.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
    std::string name = util::wide_to_utf8(fd.cFileName);
    std::string full = dir + "/" + name;
    bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    cb(full, isDir);
    if (isDir && (!skipDir || !skipDir(name))) walk(full, cb, skipDir);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

// 把 glob 模式转成正则（支持 * ? 和 **）
static std::string glob_to_regex(const std::string& pat) {
  std::string re;
  re.reserve(pat.size() * 2);
  for (size_t i = 0; i < pat.size(); i++) {
    char c = pat[i];
    if (c == '*') {
      if (i + 1 < pat.size() && pat[i + 1] == '*') {
        re += ".*";
        i++;
      } else {
        re += "[^/]*";
      }
    } else if (c == '?') {
      re += "[^/]";
    } else if (std::string(":.+|^()[]{}").find(c) != std::string::npos) {
      re += '\\';
      re += c;
    } else {
      re += c;
    }
  }
  return "^" + re + "$";
}

static std::vector<ToolDef> make_tools() {
  std::vector<ToolDef> t;

  t.push_back({"read_file",
    "读取文件内容。可指定起始行与行数上限。返回文件文本(超大文件会被截断)。",
    json::object({
      {"type", "object"},
      {"properties", json::object({
        {"path", json::object({{"type","string"},{"description","文件路径(绝对或相对)"}})},
        {"offset", json::object({{"type","integer"},{"description","起始行(1-based)，可选"}})},
        {"limit", json::object({{"type","integer"},{"description","读取行数上限，可选"}})}
      })},
      {"required", json::Value::array().push("path")}
    })});

  t.push_back({"write_file",
    "写入(或覆盖)文件。path 为文件路径，content 为完整内容。",
    json::object({
      {"type", "object"},
      {"properties", json::object({
        {"path", json::object({{"type","string"},{"description","文件路径"}})},
        {"content", json::object({{"type","string"},{"description","完整文件内容"}})}
      })},
      {"required", json::Value::array().push("path").push("content")}
    })});

  t.push_back({"edit_file",
    "在文件中把 old_string 替换为 new_string(需唯一匹配)。可选 replace_all 替换全部。",
    json::object({
      {"type", "object"},
      {"properties", json::object({
        {"path", json::object({{"type","string"},{"description","文件路径"}})},
        {"old_string", json::object({{"type","string"},{"description","要被替换的文本"}})},
        {"new_string", json::object({{"type","string"},{"description","替换后的文本"}})},
        {"replace_all", json::object({{"type","boolean"},{"description","替换全部匹配，默认 false"}})}
      })},
      {"required", json::Value::array().push("path").push("old_string").push("new_string")}
    })});

  t.push_back({"list_files",
    "列出目录中匹配通配符的文件。支持 *、? 与 **(递归)。",
    json::object({
      {"type", "object"},
      {"properties", json::object({
        {"path", json::object({{"type","string"},{"description","目录，默认当前目录"}})},
        {"pattern", json::object({{"type","string"},{"description","通配符，如 *.cpp 或 src/**/*.h"}})}
      })},
      {"required", json::Value::array()}
    })});

  t.push_back({"grep",
    "在文件内容中按正则(ECMAScript)搜索。返回 路径:行号:文本。自动跳过 .git / node_modules。",
    json::object({
      {"type", "object"},
      {"properties", json::object({
        {"pattern", json::object({{"type","string"},{"description","正则表达式(ECMAScript 语法)"}})},
        {"path", json::object({{"type","string"},{"description","搜索目录或文件，默认当前目录"}})},
        {"ignore_case", json::object({{"type","boolean"},{"description","忽略大小写"}})},
        {"line_numbers", json::object({{"type","boolean"},{"description","是否显示行号，默认 true"}})}
      })},
      {"required", json::Value::array().push("pattern")}
    })});

  t.push_back({"run_command",
    "执行一条 shell 命令(Windows 为 cmd /c，类 Unix 为 sh -c)，捕获 stdout/stderr 返回。",
    json::object({
      {"type", "object"},
      {"properties", json::object({
        {"command", json::object({{"type","string"},{"description","要执行的命令"}})},
        {"cwd", json::object({{"type","string"},{"description","工作目录，可选"}})}
      })},
      {"required", json::Value::array().push("command")}
    })});

  return t;
}

std::vector<ToolDef> all_tools() {
  static std::vector<ToolDef> t = make_tools();
  return t;
}

ToolResult execute(const std::string& name, const json::Value& input, const std::string& cwd,
                   const std::function<void(const std::string&)>& on_output) {
  ToolResult r;
  try {
    if (name == "read_file") {
      std::string p = input.getStr("path");
      if (p.empty()) { r.is_error = true; r.content = "缺少参数 path"; return r; }
      std::string full = resolve(cwd, p);
      std::ifstream f(full, std::ios::binary);
      if (!f) { r.is_error = true; r.content = "无法打开文件: " + full; return r; }
      std::vector<std::string> lines;
      std::string line;
      long total = 0;
      while (std::getline(f, line)) { lines.push_back(line); total++; }
      long offset = input.getLong("offset", 0);
      long limit = input.getLong("limit", 0);
      size_t start = 0;
      if (offset > 0) start = (size_t)(offset - 1);
      if (start > lines.size()) start = lines.size();
      size_t end = lines.size();
      const long kCap = 2000;
      if (limit > 0) end = std::min(lines.size(), start + (size_t)limit);
      std::ostringstream os;
      os << "<file path=\"" << to_forward_slash(full) << "\" total_lines=\"" << total << "\">\n";
      bool truncated = false;
      if (end - start > (size_t)kCap) { end = start + (size_t)kCap; truncated = true; }
      for (size_t i = start; i < end; i++) {
        os << (i + 1) << "\t" << lines[i] << "\n";
      }
      if (truncated) os << "... (truncated at " << kCap << " lines)\n";
      os << "</file>";
      r.content = os.str();
      return r;
    }

    if (name == "write_file") {
      std::string p = input.getStr("path");
      std::string content = input.get("content", json::Value("")).asString();
      if (p.empty()) { r.is_error = true; r.content = "缺少参数 path"; return r; }
      std::string full = resolve(cwd, p);
      if (!util::write_file(full, content)) {
        r.is_error = true; r.content = "写入失败: " + full; return r;
      }
      r.content = "已写入 " + std::to_string(content.size()) + " 字节到 " + to_forward_slash(full);
      return r;
    }

    if (name == "edit_file") {
      std::string p = input.getStr("path");
      std::string old_s = input.getStr("old_string");
      std::string new_s = input.get("new_string", json::Value("")).asString();
      bool replace_all = input.get("replace_all", json::Value(false)).asBool();
      if (p.empty() || old_s.empty()) { r.is_error = true; r.content = "缺少 path 或 old_string"; return r; }
      std::string full = resolve(cwd, p);
      std::string text;
      if (!util::read_file(full, text)) { r.is_error = true; r.content = "无法读取文件: " + full; return r; }
      size_t pos = text.find(old_s);
      if (pos == std::string::npos) { r.is_error = true; r.content = "未找到 old_string"; return r; }
      if (!replace_all) {
        if (text.find(old_s, pos + 1) != std::string::npos) {
          r.is_error = true; r.content = "old_string 存在多处匹配，请使其唯一或设置 replace_all=true"; return r;
        }
        text.replace(pos, old_s.size(), new_s);
        r.content = "已替换 1 处";
      } else {
        size_t from = 0, cnt = 0;
        while ((pos = text.find(old_s, from)) != std::string::npos) {
          text.replace(pos, old_s.size(), new_s);
          from = pos + new_s.size();
          cnt++;
        }
        r.content = "已替换 " + std::to_string(cnt) + " 处";
      }
      if (!util::write_file(full, text)) { r.is_error = true; r.content = "写回失败: " + full; return r; }
      return r;
    }

    if (name == "list_files") {
      std::string p = input.getStr("path");
      std::string pattern = input.getStr("pattern");
      std::string dir = resolve(cwd, p.empty() ? "." : p);
      std::regex re_full, re_base;
      bool have_pat = !pattern.empty();
      if (have_pat) {
        re_full = std::regex(glob_to_regex(pattern), std::regex::ECMAScript);
        std::string base = pattern;
        size_t sp = base.find_last_of("/\\");
        if (sp != std::string::npos) base = base.substr(sp + 1);
        re_base = std::regex(glob_to_regex(base), std::regex::ECMAScript);
      }
      std::ostringstream os;
      size_t count = 0;
      const size_t kMax = 500;
      walk(dir, [&](const std::string& full, bool isDir) {
        if (isDir) return;
        std::string relpath = full;
        if (relpath.size() > dir.size()) relpath = relpath.substr(dir.size());
        while (!relpath.empty() && (relpath[0] == '/' || relpath[0] == '\\')) relpath.erase(0, 1);
        std::string fname = full.substr(full.find_last_of("/\\") + 1);
        bool match = !have_pat;
        if (have_pat) {
          if (std::regex_match(relpath, re_full) || std::regex_match(fname, re_base)) match = true;
        }
        if (match) {
          os << relpath << "\n";
          if (++count >= kMax) { os << "... (truncated at " << kMax << ")\n"; }
        }
      });
      r.content = os.str();
      if (r.content.empty()) r.content = "(无匹配文件)";
      return r;
    }

    if (name == "grep") {
      std::string pat = input.getStr("pattern");
      std::string p = input.getStr("path");
      bool ic = input.get("ignore_case", json::Value(false)).asBool();
      bool ln = input.get("line_numbers", json::Value(true)).asBool();
      if (pat.empty()) { r.is_error = true; r.content = "缺少参数 pattern"; return r; }
      std::string dir = resolve(cwd, p.empty() ? "." : p);
      std::regex re;
      try {
        re = std::regex(pat, ic ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript);
      } catch (const std::exception& e) {
        r.is_error = true; r.content = "非法正则: " + std::string(e.what()); return r;
      }
      std::ostringstream os;
      size_t matches = 0;
      const size_t kMax = 300;
      auto skip = [](const std::string& n) { return n == ".git" || n == "node_modules"; };
      walk(dir, [&](const std::string& full, bool isDir) {
        if (isDir) return;
        if (matches >= kMax) return;
        std::ifstream f(full, std::ios::binary);
        if (!f) return;
        std::string line; long n = 0;
        while (std::getline(f, line) && matches < kMax) {
          n++;
          if (std::regex_search(line, re)) {
            if (ln) os << to_forward_slash(full) << ":" << n << ":" << line << "\n";
            else os << to_forward_slash(full) << ":" << line << "\n";
            matches++;
          }
        }
      }, skip);
      r.content = os.str();
      if (r.content.empty()) r.content = "(无匹配)";
      return r;
    }

    if (name == "run_command") {
      std::string cmd = input.getStr("command");
      std::string c = input.getStr("cwd");
      std::string run_dir = c.empty() ? cwd : resolve(cwd, c);
      std::string out, err;
      int rc = util::run_capture("cd /d \"" + run_dir + "\" && " + cmd, out, err, on_output);
      std::ostringstream os;
      os << "exit_code=" << rc << "\n";
      os << out;   // 输出已实时显示过，这里只给模型完整文本
      if (!err.empty()) os << err;
      r.content = os.str();
      r.is_error = (rc != 0);
      return r;
    }

    r.is_error = true;
    r.content = "未知工具: " + name;
    return r;
  } catch (const std::exception& e) {
    r.is_error = true;
    r.content = std::string("工具执行异常: ") + e.what();
    return r;
  }
}

}  // namespace tools
