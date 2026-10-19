#pragma once

#include <string>
#include <vector>

struct WifiCredential {
  std::string ssid;
  std::string password;
};

class WifiCredentialStore {
 public:
  const std::string& getLastConnectedSsid() const { return lastConnectedSsid_; }
  const WifiCredential* findCredential(const std::string&) const { return nullptr; }
  const std::vector<WifiCredential>& getCredentials() const { return credentials_; }

 private:
  std::string lastConnectedSsid_;
  std::vector<WifiCredential> credentials_;
};

extern WifiCredentialStore WIFI_STORE;
