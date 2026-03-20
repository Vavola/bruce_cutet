#ifndef __NETCUT_H__
#define __NETCUT_H__

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>

// Структура для хранения информации об устройстве в сети
struct NetcutDevice {
    IPAddress ip;
    uint8_t mac[6];
    String vendor;
    String hostname; // Имя устройства (NBNS)
    bool is_blocked;
    bool is_gateway;
};

// Глобальные переменные модуля
extern std::vector<NetcutDevice> netcut_devices;
extern bool netcut_running;
extern bool warden_enabled; // Статус Надзирателя (Белый список)
extern SemaphoreHandle_t netcut_mutex;

// Основные функции модуля
bool netcut_init();
void netcut_start_scan(bool slow_mode = false);
void netcut_resolve_names();
void netcut_app();
void arp_spoof_task(void *pvParameters);
void netcut_heal_network();

#endif // __NETCUT_H__
