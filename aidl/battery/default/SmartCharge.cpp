/*
 * Copyright (C) 2023 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "SmartCharge.h"
#include "JSONParser.hpp"

#include <GetServiceSupport.h>
#include <SafeStoi.h>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <chrono>
#include <functional>
#include <sstream>
#include <type_traits>

namespace aidl {
namespace vendor {
namespace samsung_ext {
namespace framework {
namespace battery {

using ::android::base::GetProperty;
using ::android::base::SetProperty;

using ScopedLock = const std::lock_guard<std::mutex>;

using namespace std::chrono_literals;

static constexpr int kInvalidCfg = -1;

static const char kSmartChargeConfigProp[] = "persist.ext.smartcharge.config";
static const char kSmartChargeEnabledProp[] = "persist.ext.smartcharge.enabled";
static const char kComma = ',';

template <typename T>
using is_integral_or_bool =
    std::enable_if_t<std::is_integral_v<T> || std::is_same_v<T, bool>, bool>;

static inline bool isValidBool(const int val) { return val == !!val; }
static inline bool verifyConfig(const int lower, const int upper) {
  return !(upper <= lower || upper > 95 || (0 <= lower && lower < 50));
}

template <typename T, is_integral_or_bool<T> = true> struct ConfigPair {
  T first, second;
  std::string toString(void) {
    return std::to_string(first) + kComma + std::to_string(second);
  }
};

template <typename U>
bool fromString(const std::string &v, ConfigPair<U> *pair) {
  std::stringstream ss(v);
  std::string res;

  if (v.find(kComma) != std::string::npos) {
    getline(ss, res, kComma);
    pair->first = stoi_safe(res);
    getline(ss, res, kComma);
    pair->second = stoi_safe(res);
    return true;
  }
  return false;
}

template <> bool fromString(const std::string &v, ConfigPair<bool> *pair) {
  ConfigPair<int> tmp{};
  if (fromString<int>(v, &tmp) && isValidBool(tmp.first) &&
      isValidBool(tmp.second)) {
    pair->first = tmp.first;
    pair->second = tmp.second;
    return true;
  }
  return false;
}

template <typename U> bool getAndParse(const char *prop, ConfigPair<U> *pair) {
  std::string propval = GetProperty(prop, "");
  if (!propval.empty()) {
    return fromString(propval, pair);
  }
  return false;
}

const static auto kDisabledCfgStr = ConfigPair<bool>{0, 0}.toString();

static void onServiceDied(void *cookie) {
  reinterpret_cast<SmartCharge *>(cookie)->loadHealthImpl();
}

void SmartCharge::loadHealthImpl(void) {
  bool linkToDeathSuccess;
  std::string reason;
  ScopedLock _(hal_health_lock);

  // Try aidl
  health_aidl = waitServiceDefault<IHealthAIDL>();
  if (health_aidl == nullptr) {
    // hidl
    health_hidl = ::android::hardware::health::V2_0::get_health_service();
    if (health_hidl != nullptr) {
      healthState = USE_HEALTH_HIDL;
      ALOGD("%s: Connected to health HIDL V2.0 HAL", __func__);
      hidl_death_recp = new hidl_health_death_recipient(health_hidl);
      auto ret = health_hidl->linkToDeath(hidl_death_recp,
                                          reinterpret_cast<uint64_t>(this));
      linkToDeathSuccess = ret.isOk();
      reason = ret.description();
    } else {
      LOG_ALWAYS_FATAL("Failed to connect to any valid health HAL");
    }
  } else {
    healthState = USE_HEALTH_AIDL;
    ALOGD("%s: Connected to health AIDL HAL", __func__);
    aidl_death_recp = ndk::ScopedAIBinder_DeathRecipient(
        AIBinder_DeathRecipient_new(onServiceDied));
    auto ret = AIBinder_linkToDeath(health_aidl->asBinder().get(),
                                    aidl_death_recp.get(), this);
    linkToDeathSuccess = ret == STATUS_OK;
    reason = ndk::ScopedAStatus(AStatus_fromStatus(ret)).getDescription();
  }
  if (!linkToDeathSuccess)
    ALOGW("%s: linkToDeath failed: %s", __func__, reason.c_str());
}

bool SmartCharge::loadAndParseConfigProp(void) {
  ConfigPair<int> ret{};
  if (getAndParse(kSmartChargeConfigProp, &ret) &&
      verifyConfig(ret.first, ret.second)) {
    upper = ret.second;
    lower = ret.first;
    ALOGD("%s: upper: %d, lower: %d", __func__, upper, lower);
  } else {
    upper = kInvalidCfg;
    lower = kInvalidCfg;
    ALOGW("%s: Parsing config failed", __func__);
    return false;
  }
  return true;
}

void SmartCharge::loadConfiguration(void) {
  ConfigParser parser("/system_ext/etc/smartcharge_nodes.json");

  setChargableFunc = parser.findEntry({GetProperty("ro.product.device", ""),
                                       GetProperty("ro.product.manufacturer", "")});
  if (!setChargableFunc) {
    ALOGD("%s: Using stub for setChargableFunc", __func__);
    setChargableFunc = [](const bool) {};
  }
}

void SmartCharge::loadEnabledAndStart(void) {
  ConfigPair<bool> ret{};

  if (getAndParse(kSmartChargeEnabledProp, &ret)) {
    if (ret.first) {
      ALOGD("%s: Starting loop, withrestart: %d", __func__, ret.second);
      createLoopThread(ret.second);
    } else
      ALOGD("%s: Not starting loop", __func__);
  } else {
    ALOGE("%s: Enabled prop value invalid, resetting to valid one", __func__);
    SetProperty(kSmartChargeEnabledProp, kDisabledCfgStr);
  }
}

SmartCharge::SmartCharge(void) {
  bool ret;

  loadHealthImpl();
  loadConfiguration();

  ret = loadAndParseConfigProp();
  if (ret) {
    loadEnabledAndStart();
  }
}

void SmartCharge::startLoop(bool withrestart) {
  ChargeStatus current, policy;
  bool skip = false;

  ALOGD("%s: ++", __func__);
  std::unique_lock<std::mutex> lock(kCVLock);
  while (true) {
    int per;

    switch (healthState) {
    case USE_HEALTH_AIDL: {
      using android::hardware::health::BatteryStatus;

      ScopedLock _(hal_health_lock);
      BatteryStatus status_aidl = BatteryStatus::UNKNOWN;
      auto ret = health_aidl->getCapacity(&per);
      if (!ret.isOk()) {
        per = ret.getStatus();
        break;
      }
      ret = health_aidl->getChargeStatus(&status_aidl);
      if (!ret.isOk()) {
        per = ret.getStatus();
        break;
      }
      switch (status_aidl) {
      case BatteryStatus::CHARGING:
      case BatteryStatus::FULL:
        current = ChargeStatus::ON;
        break;
      case BatteryStatus::DISCHARGING:
      case BatteryStatus::NOT_CHARGING:
        current = ChargeStatus::OFF;
        break;
      default:
        break;
      };
      break;
    }
    case USE_HEALTH_HIDL: {
      using ::android::hardware::health::V1_0::BatteryStatus;
      using ::android::hardware::health::V2_0::Result;

      ScopedLock _(hal_health_lock);
      Result res = Result::UNKNOWN;
      BatteryStatus status_hidl = BatteryStatus::UNKNOWN;
      health_hidl->getCapacity([&res, &per](Result hal_res, int32_t hal_value) {
        res = hal_res;
        per = hal_value;
      });
      if (res != Result::SUCCESS) {
        per = -(static_cast<int>(res));
        break;
      }
      health_hidl->getChargeStatus(
          [&res, &status_hidl](Result hal_res, BatteryStatus hal_value) {
            res = hal_res;
            status_hidl = hal_value;
          });
      if (res != Result::SUCCESS) {
        per = -(static_cast<int>(res));
        break;
      }
      switch (status_hidl) {
      case BatteryStatus::CHARGING:
      case BatteryStatus::FULL:
        current = ChargeStatus::ON;
        break;
      case BatteryStatus::DISCHARGING:
      case BatteryStatus::NOT_CHARGING:
        current = ChargeStatus::OFF;
        break;
      default:
        break;
      };
      break;
    }
    default:
      __builtin_unreachable();
    }
    if (per < 0) {
      SetProperty(kSmartChargeEnabledProp, kDisabledCfgStr);
      ALOGE("%s: exit loop: retval: %d", __func__, per);
      break;
    }
    if (per > upper)
      policy = ChargeStatus::OFF;
    else if (withrestart && per < lower)
      policy = ChargeStatus::ON;
    else if (!withrestart && per < upper)
      policy = ChargeStatus::ON;
    else
      skip = true;

    if (current != policy && !skip) {
      ALOGD("%s: Updating current, current %d, policy %d", __func__, current,
            policy);
      switch (policy) {
      case ChargeStatus::OFF:
        setChargableFunc(false);
        break;
      case ChargeStatus::ON:
        setChargableFunc(true);
        break;
      default:
        break;
      }
      status = policy;
    }
    skip = false;
    if (cv.wait_for(lock, 5s) == std::cv_status::no_timeout) {
      // cv signaled, exit now if kRunning is false
      if (!kRunning)
        break;
    }
  }
  ALOGD("%s: --", __func__);
}

void SmartCharge::createLoopThread(bool restart) {
  ScopedLock _(thread_lock);
  ALOGD("%s: create thread", __func__);
  kLoopThread =
      std::make_shared<std::thread>(&SmartCharge::startLoop, this, restart);
  kRunning = true;
}

ndk::ScopedAStatus SmartCharge::setChargeLimit(int32_t upper_, int32_t lower_) {
  ALOGD("%s: upper: %d, lower: %d, kRun: %d", __func__, upper_, lower_,
        kRunning.load());
  if (!verifyConfig(lower_, upper_))
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  if (kRunning)
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
  if (lower_ < 0)
    lower_ = kInvalidCfg;
  auto pair = ConfigPair<int>{lower_, upper_};
  SetProperty(kSmartChargeConfigProp, pair.toString());
  {
    std::unique_lock<std::mutex> _(config_lock);
    lower = lower_;
    upper = upper_;
  }
  ALOGD("%s: Exit", __func__);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus SmartCharge::activate(bool enable, bool restart) {
  auto pair = ConfigPair<bool>{enable, restart};
  {
    std::unique_lock<std::mutex> _(config_lock);
    ALOGD("%s: upper: %d, lower: %d, enable: %d, restart: %d, kRun: %d",
          __func__, upper, lower, enable, restart, kRunning.load());
    if (!verifyConfig(lower, upper))
      return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    if (lower == kInvalidCfg && restart)
      return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  }
  if (kRunning == enable)
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  SetProperty(kSmartChargeEnabledProp, pair.toString());
  if (enable) {
    if (kRunning) {
      ALOGW("Thread is running?");
    } else {
      createLoopThread(restart);
    }
  } else {
    setChargableFunc(true);
    if (kRunning) {
      ScopedLock _(thread_lock);
      kRunning = false;
      if (kLoopThread->joinable()) {
        cv.notify_one();
        kLoopThread->join();
      }
      kLoopThread.reset();
    } else {
      ALOGW("No threads to stop?");
    }
  }
  ALOGD("%s: Exit", __func__);
  return ndk::ScopedAStatus::ok();
}

binder_status_t SmartCharge::dump(int fd, const char ** /* args */,
                                  uint32_t /* numArgs */) {
  Dl_info info;
  void *addr;
  auto tryLockFn = [](std::mutex &m) {
    const std::unique_lock<std::mutex> lk{m, std::try_to_lock};
    return !lk.owns_lock();
  };

  dprintf(fd, "Loop thread running: %d\n", kRunning.load());
  if (kRunning) {
    dprintf(fd, "Loop thread charge control state: ");
    switch (status) {
    case ChargeStatus::ON:
      dprintf(fd, "ON");
      break;
    case ChargeStatus::OFF:
      dprintf(fd, "OFF");
      break;
    }
    dprintf(fd, "\n");
  }
  dprintf(fd, "Configuration (upper/lower): %d %d\n", upper, lower);
  dprintf(fd, "Mutex locked (config/thread/cv) %d %d %d\n",
          tryLockFn(config_lock), tryLockFn(thread_lock), tryLockFn(kCVLock));
  dprintf(fd, "Connected Health HAL: ");
  switch (healthState) {
  case USE_HEALTH_AIDL:
    dprintf(fd, "AIDL Health HAL V1");
    break;
  case USE_HEALTH_HIDL:
    dprintf(fd, "HIDL Health HAL V2.0");
    break;
  default:
    break;
  };
  dprintf(fd, "\n");
  return STATUS_OK;
}

using ::android::hardware::interfacesEqual;

void hidl_health_death_recipient::serviceDied(
    uint64_t cookie, const wp<::android::hidl::base::V1_0::IBase> &who) {
  if (mHealth != nullptr && interfacesEqual(mHealth, who.promote())) {
    onServiceDied(reinterpret_cast<void *>(cookie));
  }
}

} // namespace battery
} // namespace framework
} // namespace samsung_ext
} // namespace vendor
} // namespace aidl
