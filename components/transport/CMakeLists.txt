set(TRANSPORT_SRCS)

message(STATUS ">>> CONFIG_PROTO_BT_ENABLED = ${CONFIG_PROTO_BT_ENABLED}")

# К Wi-Fi или к BLE добавляем свой файл
# if(CONFIG_PROTO_BT_ENABLED)
    # list(APPEND TRANSPORT_SRCS src/transport_ble.c)
# else()
    # list(APPEND TRANSPORT_SRCS src/transport_wifi.c)
# endif()

list(APPEND TRANSPORT_SRCS src/transport_espnow.c)

idf_component_register(
    SRCS
        ${TRANSPORT_SRCS}
    INCLUDE_DIRS
        include
    REQUIRES
        bt 
        led_ctrl
        esp_wifi 
        esp_netif 
        nvs_flash 
        lwip 
)