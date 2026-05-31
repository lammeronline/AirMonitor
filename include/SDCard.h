#pragma once
#include <Arduino.h>

// Функция вернет размер карты в МБ, либо 0, если ошибка
uint64_t initSDCard();