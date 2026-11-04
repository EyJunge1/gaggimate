#pragma once
#include <cstdio>

#define ESP_LOGI(tag, fmt, ...) std::printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) std::printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
