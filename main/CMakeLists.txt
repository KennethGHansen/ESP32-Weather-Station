idf_component_register(
    SRCS
        "main_app.c" "weather_queue.c" "wifi_transport.c"
    INCLUDE_DIRS
        "." "include"
    
    REQUIRES
        baro_forecast
        air_quality
        st7789h2
        bme68x
        ui
        ui_controller
        minmax_stats
        pushbuttons
        esp_timer
        esp_wifi 
        esp_netif 
        esp_event
        esp_http_client
        nvs_flash        
        freertos
        driver
        mbedtls

)