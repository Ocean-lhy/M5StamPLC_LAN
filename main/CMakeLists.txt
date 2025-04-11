idf_component_register(SRCS "main.c" "test_case.c"
                    PRIV_REQUIRES spi_flash esp_netif esp_eth
                    INCLUDE_DIRS ".")
