#include "crowdy/graphql/auth_state.hpp"

#include <cstdio>
#include <fstream>

namespace crowdy::graphql {

std::string FileTokenStore::load() {
  std::ifstream in(path_);
  if (!in) return {};
  std::string token;
  std::getline(in, token);
  return token;
}

void FileTokenStore::save(const std::string& token) {
  std::ofstream out(path_, std::ios::trunc);
  out << token;
}

void FileTokenStore::clear() { std::remove(path_.c_str()); }

void AuthState::setToken(const std::string& token) {
  std::vector<std::function<void(const std::string&)>> listeners;
  {
    std::lock_guard lock(mutex_);
    if (token_ == token) return;
    token_ = token;
    store_->save(token);
    listeners = listeners_;
  }
  for (auto& l : listeners) l(token);
}

void AuthState::clearToken() {
  std::vector<std::function<void(const std::string&)>> listeners;
  {
    std::lock_guard lock(mutex_);
    if (token_.empty()) return;
    token_.clear();
    store_->clear();
    listeners = listeners_;
  }
  for (auto& l : listeners) l(std::string());
}

bool AuthState::restore() {
  std::string token = store_->load();
  if (token.empty()) return false;
  setToken(token);
  return true;
}

void AuthState::onChange(std::function<void(const std::string&)> listener) {
  std::lock_guard lock(mutex_);
  listeners_.push_back(std::move(listener));
}

}  // namespace crowdy::graphql
