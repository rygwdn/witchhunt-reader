#pragma once
#include <string>

class KOReaderCredentialStore {
 public:
  bool hasCredentials() const { return !username_.empty(); }
  const std::string& getUsername() const { return username_; }
  std::string getBaseUrl() const { return base_; }
  void set(const std::string& base, const std::string& user) {
    base_ = base;
    username_ = user;
  }
  static KOReaderCredentialStore& getInstance() {
    static KOReaderCredentialStore value;
    return value;
  }

 private:
  std::string base_ = "https://bookorbit.test";
  std::string username_ = "reader";
};
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
