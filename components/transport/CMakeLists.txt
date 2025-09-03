set(TRANSPORT_SRCS)

# message(STATUS ">>> CONFIG_PROTO_BT_ENABLED = ${CONFIG_PROTO_BT_ENABLED}")

# list(APPEND TRANSPORT_SRCS src/transport_wifi.c)
# list(APPEND TRANSPORT_SRCS src/transport_ble.c)
# list(APPEND TRANSPORT_SRCS src/transport_espnow.c)
list(APPEND TRANSPORT_SRCS src/transport_zigbee.c)

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