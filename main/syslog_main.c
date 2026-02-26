#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h" 
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "nvs_flash.h"

// === Настройки Wi-Fi ===
#define WIFI_SSID       "TP-Link_BDF2"          // Замените на имя вашей сети
#define WIFI_PASS       "12182541"      // Замените на пароль
#define WIFI_MAX_RETRY  5                   // Максимальное число попыток подключения

// === Настройки syslog ===
#define SYSLOG_SERVER_IP   "192.168.0.105"  // IP-адрес компьютера с syslog-сервером
#define SYSLOG_PORT        514
#define LOG_INTERVAL_SEC   10                // Интервал отправки (секунд)
#define HOSTNAME           "esp32c6_wifi"      // Имя устройства в логах

// === Идентификаторы событий ===
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "syslog_wifi";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// === Обработчик событий Wi-Fi и IP ===
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Попытка переподключения (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Потеряно соединение с Wi-Fi");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Получен IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// === Инициализация Wi-Fi и подключение ===
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Ожидание подключения к Wi-Fi...");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Подключено к Wi-Fi");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Не удалось подключиться к Wi-Fi");
    } else {
        ESP_LOGE(TAG, "Неизвестное событие");
    }

    // Обработчики можно не удалять, они останутся до конца работы
}

// === Колбэк синхронизации времени ===
void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "=== NTP СИНХРОНИЗАЦИЯ УСПЕШНА! ===");
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Текущее время: %s", strftime_buf);
}

// === Функция синхронизации времени ===
static void obtain_time(void)
{
    ESP_LOGI(TAG, "Инициализация SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG();
    config.num_of_servers = 4;
    config.servers[0] = "ntp.msk-ix.ru";
    config.servers[1] = "ru.pool.ntp.org";
    config.servers[2] = "pool.ntp.org";
    config.servers[3] = "time.google.com";
    config.sync_cb = time_sync_notification_cb;

    esp_netif_sntp_init(&config);

    int retry = 0;
    const int retry_count = 30;
    while (esp_netif_sntp_sync_wait(5000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retry < retry_count) {
        ESP_LOGI(TAG, "Ожидание синхронизации NTP... (%d/%d)", retry, retry_count);
    }

    if (retry < retry_count) {
        ESP_LOGI(TAG, "Время успешно синхронизировано");
    } else {
        ESP_LOGE(TAG, "Не удалось синхронизировать время");
    }
}

// === Отправка syslog-сообщения ===
static void send_syslog(const char *hostname, int facility, int severity, const char *tag, const char *message)
{
    char buffer[256];
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%b %d %H:%M:%S", &timeinfo);

    int pri = facility * 8 + severity;

    int len = snprintf(buffer, sizeof(buffer),
                       "<%d>%s %s %s: %s",
                       pri, timestamp, hostname, tag, message);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Ошибка создания сокета: errno %d", errno);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SYSLOG_SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SYSLOG_PORT);

    int err = sendto(sock, buffer, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Ошибка отправки syslog: errno %d", errno);
    } else {
        ESP_LOGI(TAG, "Syslog отправлен (%d байт): %s", len, buffer);
    }

    close(sock);
}

// === Задача отправки логов ===
static void syslog_task(void *pvParameters)
{
    const char *hostname = HOSTNAME;
    int counter = 0;
    while (1) {
        counter++;
        char msg[64];
        snprintf(msg, sizeof(msg), "Test syslog message #%d", counter);
        send_syslog(hostname, 16, 6, "test_syslog", msg);
        vTaskDelay(pdMS_TO_TICKS(LOG_INTERVAL_SEC * 1000));
    }
}

void app_main(void)
{
        // Инициализация NVS 
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Если раздел NVS заполнен или требует обновления, стираем его и инициализируем заново
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // 1. Подключение к Wi-Fi
    wifi_init_sta();

    // 2. Синхронизация времени
    obtain_time();

    // 3. Запуск задачи отправки логов
    xTaskCreate(syslog_task, "syslog_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Программа запущена. Отправка syslog каждые %d секунд на %s:%d",
             LOG_INTERVAL_SEC, SYSLOG_SERVER_IP, SYSLOG_PORT);
}