// session.hpp - 会话持久化：保存/恢复对话历史（/resume 用）
#pragma once
#include <string>
#include <vector>
#include "provider.hpp"

namespace session {

struct Meta {
  std::string file;      // 完整路径
  std::string id;        // 文件名（去 .json）
  std::string saved_at;  // 保存时间
  std::string model;     // 当时用的模型
  std::string preview;   // 首条用户消息预览
  size_t turns = 0;      // 用户消息轮数
};

// 会话目录：<exe目录>/resume（发布版即 d:/openaidd/resume；REPL 与 WebUI 共用）
std::string sessions_dir();

// 列出全部会话（按时间倒序，最新在前）
std::vector<Meta> list();

// 新建一个会话文件路径（时间戳命名；同秒冲突自动加序号）
std::string new_session_file();

// 保存对话历史（整文件覆盖写）
bool save(const std::string& file, const std::string& model,
          const std::vector<prov::Message>& history);

// 加载会话；成功返回 true 并填充 history / model / saved_at（后两个可传 nullptr）
bool load(const std::string& file, std::vector<prov::Message>& history,
          std::string& model, std::string* saved_at = nullptr);

}  // namespace session
