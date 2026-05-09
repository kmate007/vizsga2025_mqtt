#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_eth.h"
#include "esp_eth_phy_lan87xx.h"  // Az új komponens ezt a fejlécet használja

static const char *TAG = "eth_mqtt_static";

// --- STATIKUS IP KONFIGURÁCIÓ ---
#define DEVICE_IP          "192.168.50.101"
#define DEVICE_GW          "192.168.50.100"
#define DEVICE_NETMASK     "255.255.255.0"

// --- MQTT KONFIGURÁCIÓ ---
#define MQTT_BROKER_URL    "mqtt://192.168.25.249"

static esp_mqtt_client_handle_t mqtt_client = NULL;

//a folyamatos üzenetküldés
void mqtt_heartbeat_task(void *pvParameters) {
	int iii=1;
	char uzenet[32];
    while (1) {
	    iii++;
snprintf(uzenet, sizeof(uzenet), "onlinevagyok %d", iii);
        if (mqtt_client != NULL) {
            // Üzenet küldése 5 másodpercenként
            esp_mqtt_client_publish(mqtt_client, "/topic/status", uzenet, 0, 1, 0);
            ESP_LOGI("HEARTBEAT", "Státusz üzenet elküldve");
        }
        // 5000 milliszekundum várakozás
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// MQTT Eseménykezelő
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            esp_mqtt_client_subscribe(mqtt_client, "/topic/test", 0);
            esp_mqtt_client_publish(mqtt_client, "/topic/status", "onlinevagyok", 0, 1, 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        default:
            break;
    }
}

static void start_mqtt(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
    };
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// Statikus IP beállítása 
static esp_err_t set_static_ip(esp_netif_t *netif) {
    if (netif == NULL) return ESP_FAIL;

    // DHCP kliens leállítása kötelező a statikus IP előtt
    esp_netif_dhcpc_stop(netif);

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));

    esp_netif_str_to_ip4(DEVICE_IP, &ip_info.ip);
    esp_netif_str_to_ip4(DEVICE_GW, &ip_info.gw);
    esp_netif_str_to_ip4(DEVICE_NETMASK, &ip_info.netmask);

    ESP_LOGI(TAG, "Setting static IP: %s", DEVICE_IP);
    return esp_netif_set_ip_info(netif, &ip_info);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    esp_netif_t *eth_netif = (esp_netif_t *)arg;
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            // Amint megvan a fizikai kapcsolat, beállítjuk az IP-t
            set_static_ip(eth_netif);
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
        default:
            break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Static IP Assigned: " IPSTR, IP2STR(&event->ip_info.ip));
    
    // Csak akkor indítjuk az MQTT-t, ha már van IP címünk
    if (mqtt_client == NULL) {
        start_mqtt();
    }
}

void init_ethernet(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_config);

    // GPIO16 táp/oszcillátor engedélyezés (LAN8720 specifikus)
    gpio_reset_pin(GPIO_NUM_16);
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_16, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); // Kis várakozás a stabilizálódáshoz

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = GPIO_NUM_23;
    esp32_emac_config.smi_gpio.mdio_num = GPIO_NUM_18;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1; 
    phy_config.reset_gpio_num = -1; // Ha a 16-os pin a reset, ide is írhatod

    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, eth_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}

void app_main(void) {
    // NVS inicializálás
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Ethernet indítása
    init_ethernet();

    // Itt hozzuk létre a külön szálat az 5 másodperces küldéshez
    xTaskCreate(mqtt_heartbeat_task, "mqtt_heartbeat_task", 4096, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
