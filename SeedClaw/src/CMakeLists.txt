idf_component_register(
    SRCS
        "seedclaw.c"
        "wifi.c"
        "discord.c"
        "llm.c"
        "gpio_ctrl.c"
        "tools.c"
        "cli.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client
        esp_event esp-tls driver esp_timer esp_adc
        vfs
    PRIV_REQUIRES
        json console
)
