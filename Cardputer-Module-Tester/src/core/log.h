#pragma once
// Tagged serial logging. Level is set by -DAPP_LOG_LEVEL=n in platformio.ini
//   0 = off, 1 = error, 2 = warn, 3 = info, 4 = debug
#include <Arduino.h>

#ifndef APP_LOG_LEVEL
#define APP_LOG_LEVEL 3
#endif

#define APP_LOG(level, name, tag, fmt, ...)                                    \
  do {                                                                         \
    if ((level) <= APP_LOG_LEVEL) {                                            \
      Serial.printf("%s [%s] " fmt "\n", name, tag, ##__VA_ARGS__);            \
    }                                                                          \
  } while (0)

#define LOGE(tag, fmt, ...) APP_LOG(1, "E", tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) APP_LOG(2, "W", tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) APP_LOG(3, "I", tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) APP_LOG(4, "D", tag, fmt, ##__VA_ARGS__)
