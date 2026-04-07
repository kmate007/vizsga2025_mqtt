idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES 
        esp_eth 
        esp_netif 
        esp_event 
        esp_driver_gpio 
        nvs_flash 
        esp_http_server 
        log 
        espressif__mqtt
        espressif__lan87xx
)
