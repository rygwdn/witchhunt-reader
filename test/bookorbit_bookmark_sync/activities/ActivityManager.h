#pragma once

class ActivityManager {
 public:
  bool wifiActivityOnStack() const { return currentActivityUsesWifi_; }
  bool currentActivityUsesWifi_ = false;
};

extern ActivityManager activityManager;
