// utils.hpp - 跨平台小工具 + Windows ANSI 颜色
#pragma once
#include <string>
#include <vector>
#include <functional>

namespace util {

// UTF-8 <-> wstring (Windows API 需要宽字符)
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

// 启用 Windows 控制台 ANSI 转义（VT）
void enable_vt();

// 颜色常量
namespace color {
  extern const char* reset;
  extern const char* bold;
  extern const char* dim;
  extern const char* red;
  extern const char* green;
  extern const char* yellow;
  extern const char* blue;
  extern const char* magenta;
  extern const char* cyan;
  extern const char* gray;
  extern const char* black;
}

// 带颜色的打印（自动处理 ANSI 开关）
void print_colored(const std::string& text, const char* c);

// 读取整个文件（二进制安全到字符串）
bool read_file(const std::string& path, std::string& out);
// 写入整个文件
bool write_file(const std::string& path, const std::string& content);

// 当前时间字符串 YYYY-MM-DD HH:MM:SS
std::string now_string();

// trim
std::string trim(const std::string& s);
std::string trim_copy(const std::string& s);

// 切分字符串
std::vector<std::string> split(const std::string& s, char delim);

// 取用户主目录
std::string home_dir();

// 把命令输出/错误捕获为字符串（用于 run_command 工具，非交互）。
// on_line 非空时，每读到一行实时回调（用于流式显示命令输出）。
// 注意：Windows 下命令以系统代码页(GBK)传给 cmd.exe，输出转回 UTF-8。
int run_capture(const std::string& cmd, std::string& out, std::string& err,
                const std::function<void(const std::string&)>& on_line = {});

// Windows 下获取命令行参数（UTF-8）：用 GetCommandLineW + CommandLineToArgvW 重建，
// 规避 main() 的 argv 使用系统代码页(GBK)导致的乱码。失败时返回空 vector。
std::vector<std::string> command_line_args();

// UTF-8 <-> 系统代码页（Windows 上为 GBK/ANSI）。
// 命令交给 cmd.exe 前需转成 GBK；cmd 输出是 GBK，需转回 UTF-8 再发给模型。
std::string utf8_to_ansi(const std::string& s);
std::string ansi_to_utf8(const std::string& s);

// exe 所在目录（UTF-8，不带末尾斜杠）
std::string exe_dir();

// 递归创建目录（已存在返回 true）
bool ensure_dir(const std::string& path);

// 列出目录下的文件名（不含子目录、不含 . 和 ..）
std::vector<std::string> list_dir(const std::string& dir);

}  // namespace util
