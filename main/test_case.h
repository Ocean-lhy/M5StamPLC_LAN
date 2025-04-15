#ifndef TEST_CASE_H
#define TEST_CASE_H

#include "esp_netif.h"

void static_ip_test_case(esp_netif_t *eth_netif);

void dns_test_case(esp_netif_t *eth_netif);

void tcp_server_test_case(void);

void tcp_client_test_case(void);

void udp_test_case(void);

void iperf_test_case(void); 

void multi_socket_send_receive_test(void);

#endif
