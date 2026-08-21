// utils.cpp
#include "utils.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>

#ifdef _WIN32
  #include <windows.h>
  #include <shellapi.h>
  #include <io.h>
  #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
    #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
  #endif
#else
  #include <unistd.h>
  #include <dirent.h>
  #include <sys/stat.h>
  #include <errno.h>
#endif

namespace util {

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w;
  w.resize(n);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s;
  s.resize(n);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

void enable_vt() {
#ifdef _WIN32
  // 控制台统一使用 UTF-8，避免中文/特殊符号乱码
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) return;
  DWORD mode = 0;
  if (GetConsoleMode(hOut, &mode)) {
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
  }
#endif
}

namespace color {
  const char* reset   = "\033[0m";
  const char* bold    = "\033[1m";
  const char* dim     = "\033[2m";
  const char* red     = "\033[31m";
  const char* green   = "\033[32m";
  const char* yellow  = "\033[33m";
  const char* blue    = "\033[34m";
  const char* magenta = "\033[35m";
  const char* cyan    = "\033[36m";
  const char* gray    = "\033[90m";
  const char* black   = "\033[30m";
}

void print_colored(const std::string& text, const char* c) {
  printf("%s%s%s", c, text.c_str(), color::reset);
}

bool read_file(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return false; }
  fseek(f, 0, SEEK_SET);
  out.resize((size_t)sz);
  if (sz > 0) {
    size_t r = fread(&out[0], 1, (size_t)sz, f);
    if (r != (size_t)sz) { fclose(f); return false; }
  }
  fclose(f);
  return true;
}

bool write_file(const std::string& path, const std::string& content) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  size_t w = fwrite(content.data(), 1, content.size(), f);
  fclose(f);
  return w == content.size();
}

std::string now_string() {
  time_t t = time(nullptr);
  struct tm tm {};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a]==' '||s[a]=='\t'||s[a]=='\r'||s[a]=='\n')) a++;
  while (b > a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\r'||s[b-1]=='\n')) b--;
  return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == delim) { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

std::string home_dir() {
#ifdef _WIN32
  const char* u = getenv("USERPROFILE");
  if (u) return u;
  const char* h = getenv("HOME");
  if (h) return h;
  return ".";
#else
  const char* h = getenv("HOME");
  return h ? h : ".";
#endif
}

// 用管道运行命令并捕获 stdout/stderr（Windows 用 cmd /c，类 Unix 用 sh -c）
int run_capture(const std::string& cmd, std::string& out, std::string& err,
                const std::function<void(const std::string&)>& on_line) {
#ifdef _WIN32
  // main() 的 argv 与程序内字符串都是 UTF-8，而 cmd.exe 期望系统代码页(GBK)。
  // 先转码，否则含中文的命令（如 echo 你好）会乱码或解析失败。
  std::string cmdA = utf8_to_ansi(cmd);
  std::string full = "cmd /c \"" + cmdA + "\" 2>&1";
  FILE* pipe = _popen(full.c_str(), "r");
#else
  std::string full = "sh -c \"" + cmd + "\" 2>&1";
  FILE* pipe = popen(full.c_str(), "r");
#endif
  if (!pipe) { err = "failed to launch command"; return -1; }
  char buf[4096];
  while (fgets(buf, sizeof(buf), pipe) != nullptr) {
    std::string line = buf;
#ifdef _WIN32
    // cmd 输出是 GBK 字节，转回 UTF-8 再上屏/发给模型
    line = ansi_to_utf8(line);
#endif
    out += line;
    if (on_line) on_line(line);   // 实时回调（line 含换行符）
  }
#ifdef _WIN32
  int rc = _pclose(pipe);
#else
  int rc = pclose(pipe);
#endif
  if (rc == -1) rc = 0;
  return rc;
}

std::vector<std::string> command_line_args() {
#ifdef _WIN32
  // main() 的 argv 是系统代码页(GBK)编码，直接当 UTF-8 用会把中文参数变成乱码发给模型。
  // 改用 UTF-16 命令行重建，得到纯 UTF-8 参数。
  int n = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
  if (wargv) {
    std::vector<std::string> out;
    out.reserve((size_t)n);
    for (int i = 0; i < n; i++) out.push_back(wide_to_utf8(wargv[i]));
    LocalFree(wargv);
    return out;
  }
#endif
  return {};
}

std::string utf8_to_ansi(const std::string& s) {
  if (s.empty()) return {};
  std::wstring w = utf8_to_wide(s);
  int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return s;
  std::string out;
  out.resize(n);
  WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
  return out;
}

std::string ansi_to_utf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
  if (n <= 0) return s;
  std::wstring w;
  w.resize(n);
  MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
  return wide_to_utf8(w);
}

std::string exe_dir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH * 2] = {};
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return ".";
  std::string p = wide_to_utf8(buf);
  for (auto& c : p) if (c == '\\') c = '/';
  size_t slash = p.rfind('/');
  if (slash == std::string::npos) return ".";
  return p.substr(0, slash);
#else
  return ".";
#endif
}

bool ensure_dir(const std::string& path) {
  if (path.empty()) return false;
  std::string p = path;
  for (auto& c : p) if (c == '\\') c = '/';
  size_t i = 0;
  if (p.size() > 2 && p[1] == ':') i = 2;   // 跳过盘符
  while (i < p.size()) {
    size_t next = p.find('/', i + 1);
    if (next == std::string::npos) next = p.size();
    std::string part = p.substr(0, next);
    if (part.size() > 3) {   // 跳过 "D:" 和 "/"
#ifdef _WIN32
      std::wstring w = utf8_to_wide(part);
      if (!CreateDirectoryW(w.c_str(), nullptr) &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
      }
#else
      if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
    }
    i = next;
  }
  return true;
}

std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> out;
#ifdef _WIN32
  std::wstring pattern = utf8_to_wide(dir);
  if (!pattern.empty() && pattern.back() != L'/' && pattern.back() != L'\\')
    pattern += L"/";
  pattern += L"*";
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    out.push_back(wide_to_utf8(fd.cFileName));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string n = e->d_name;
    std::string full = dir + "/" + n;
    struct stat st;
    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(n);
  }
  closedir(d);
#endif
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace util
