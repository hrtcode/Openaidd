// tools.hpp - 工具定义与执行
#pragma once
#include <string>
#include <vector>
#include <functional>
#include "json.hpp"

namespace tools {

struct ToolDef {
  std::string name;
  std::string description;
  json::Value schema;   // JSON Schema 对象（type:object + properties + required）
};

struct ToolResult {
  std::string content;
  bool is_error = false;
};

// 全部可用工具
std::vector<ToolDef> all_tools();

// 执行工具。input 为已解析的 JSON 参数对象；cwd 为工作目录（用于解析相对路径）。
// on_output 非空时，run_command 的每一行输出会实时回调（供流式显示）。
ToolResult execute(const std::string& name, const json::Value& input, const std::string& cwd,
                   const std::function<void(const std::string&)>& on_output = {});

}  // namespace tools
