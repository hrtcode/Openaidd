// providers.hpp - 厂商与模型/token 定价数据库
#pragma once
#include <string>
#include <vector>

namespace prov {

struct ModelInfo {
  std::string name;   // 模型名
  std::string plan;   // token 定价/上下文说明
};

struct ProviderInfo {
  std::string id;         // 内部 provider：anthropic | openai | custom
  std::string display;    // 厂商显示名
  std::string tag;        // 菜单右侧提示
  std::string base_url;   // 默认请求地址（custom 时给出建议值）
  bool custom_base = false;  // 是否强制用户填写地址
  std::vector<ModelInfo> models;  // custom 时为空，模型名由用户输入
};

// 全部厂商（向导菜单顺序）
const std::vector<ProviderInfo>& all();

// 从官方 API 拉取可用模型列表（GET /models，兼容 OpenAI 与 Anthropic 两种响应格式）
// 成功返回 true，out 填模型名；失败返回 false，err 描述原因（调用方应回退内置表）
bool fetch_models(const ProviderInfo& p, const std::string& api_key,
                  std::vector<ModelInfo>& out, std::string& err);

}  // namespace prov
