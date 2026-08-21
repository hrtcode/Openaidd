// provider.cpp - Provider 工厂
#include "provider.hpp"
#include "config.hpp"

// 各实现的构造器在对应 cpp 中定义
std::unique_ptr<prov::Provider> make_anthropic(const cfg::Config& c);
std::unique_ptr<prov::Provider> make_openai(const cfg::Config& c);

namespace prov {

std::unique_ptr<Provider> make_provider(const cfg::Config& c) {
  if (c.is_anthropic()) return make_anthropic(c);
  return make_openai(c);
}

}  // namespace prov
