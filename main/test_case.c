#include "test_case.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/sockets.h"

static const char *TAG = "test_case";

// use it after esp_netif_attach
void static_ip_test_case(esp_netif_t *eth_netif)
{
    // 停止DHCP客户端
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));

    // 配置静态IP地址
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = ESP_IP4TOUINT32(105, 0, 168, 192);
    ip_info.gw.addr = ESP_IP4TOUINT32(105, 0, 168, 192);
    ip_info.netmask.addr = ESP_IP4TOUINT32(0, 255, 255, 255);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));
}

// DNS解析回调函数
static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
    if (ipaddr) {
        ESP_LOGI(TAG, "域名 %s 解析结果: " IPSTR, name, IP2STR(&ipaddr->u_addr.ip4));
    } else {
        ESP_LOGE(TAG, "域名 %s 解析失败", name);
    }
}

// DNS解析测试函数
void dns_test_case(esp_netif_t *eth_netif)
{
    // 配置DNS服务器
    esp_netif_dns_info_t dns_info;
    IP4_ADDR(&dns_info.ip.u_addr.ip4, 8, 8, 8, 8);  // 使用Google DNS服务器
    ESP_ERROR_CHECK(esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info));
    
    // 备用DNS服务器
    IP4_ADDR(&dns_info.ip.u_addr.ip4, 114, 114, 114, 114);  // 使用114 DNS服务器作为备用
    ESP_ERROR_CHECK(esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_BACKUP, &dns_info));
    
    // 测试域名解析
    const char* test_domains[] = {
        "www.baidu.com",
        "www.espressif.com",
        "www.github.com"
    };
    
    for (int i = 0; i < sizeof(test_domains)/sizeof(test_domains[0]); i++) {
        ESP_LOGI(TAG, "开始解析域名: %s", test_domains[i]);
        
        ip_addr_t addr;
        err_t err = dns_gethostbyname(test_domains[i], &addr, dns_callback, NULL);
        
        if (err == ERR_OK) {
            // 域名已经在缓存中，可以直接获取结果
            ESP_LOGI(TAG, "域名 %s 解析结果(缓存): " IPSTR, test_domains[i], IP2STR(&addr.u_addr.ip4));
        } else if (err == ERR_INPROGRESS) {
            // 解析请求已发送，等待异步回调
            ESP_LOGI(TAG, "域名 %s 解析中...", test_domains[i]);
        } else {
            ESP_LOGE(TAG, "域名 %s 解析请求失败，错误码: %d", test_domains[i], err);
        }
        
        // 等待一段时间，让DNS解析完成
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#define TCP_PORT 8080
#define BUFFER_SIZE 1024
#define PREFIX_LEN 20  // "服务器已收到: " 的长度

static void tcp_server_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "无法创建socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // 设置SO_REUSEADDR选项
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket绑定失败: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(sock, 1) < 0) {
        ESP_LOGE(TAG, "Socket监听失败: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP服务器启动在端口 %d", TCP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_sock = accept(sock, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (client_sock < 0) {
            ESP_LOGE(TAG, "无法接受连接: %d", errno);
            continue;
        }

        // 设置接收超时
        struct timeval timeout = {
            .tv_sec = 10,  // 10秒超时
            .tv_usec = 0
        };
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        ESP_LOGI(TAG, "客户端连接: %s:%d", 
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        char rx_buffer[BUFFER_SIZE];
        char tx_buffer[BUFFER_SIZE];
        int len;

        // 接收数据并回显
        while ((len = recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, 0)) > 0) {
            rx_buffer[len] = 0; // 确保字符串结束
            ESP_LOGI(TAG, "收到: %s", rx_buffer);
            
            // 检查是否是退出命令
            if (strcmp(rx_buffer, "quit") == 0) {
                ESP_LOGI(TAG, "收到退出命令");
                send(client_sock, "再见!", 7, 0);
                break;
            }
            
            // 安全地准备回复数据
            memset(tx_buffer, 0, sizeof(tx_buffer));
            strlcpy(tx_buffer, "服务器已收到: ", sizeof(tx_buffer));
            strlcat(tx_buffer, rx_buffer, sizeof(tx_buffer) - PREFIX_LEN);  // 确保有足够空间
            
            // 发送回复
            int sent = send(client_sock, tx_buffer, strlen(tx_buffer), 0);
            if (sent < 0) {
                ESP_LOGE(TAG, "发送失败: %d", errno);
                break;
            }
        }

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG, "接收超时");
            } else {
                ESP_LOGE(TAG, "接收错误: %d", errno);
            }
        }

        ESP_LOGI(TAG, "客户端断开连接");
        close(client_sock);
    }

    close(sock);
    vTaskDelete(NULL);
}

void tcp_server_test_case(void)
{
    xTaskCreate(tcp_server_task, "tcp_server", 8192, NULL, 5, NULL);
}

#define TCP_CLIENT_TARGET_PORT 8888
#define TCP_CLIENT_TARGET_HOST "192.168.0.100"

static void tcp_client_task(void *pvParameters)
{
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_CLIENT_TARGET_PORT)
    };
    inet_aton(TCP_CLIENT_TARGET_HOST, &dest_addr.sin_addr);

    while(1) {
        ESP_LOGI(TAG, "正在连接服务器 %s:%d...", TCP_CLIENT_TARGET_HOST, TCP_CLIENT_TARGET_PORT);
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        
        if (sock < 0) {
            ESP_LOGE(TAG, "无法创建socket: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // 设置接收和发送超时
        struct timeval timeout = {
            .tv_sec = 5,
            .tv_usec = 0
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
            ESP_LOGE(TAG, "连接服务器失败: %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "已连接到服务器");

        // 测试消息列表
        const char *test_messages[] = {
            "Hello from ESP32!",
            "这是一条测试消息",
            "Testing 123",
            "quit"
        };

        char rx_buffer[BUFFER_SIZE];
        
        for (int i = 0; i < sizeof(test_messages)/sizeof(test_messages[0]); i++) {
            ESP_LOGI(TAG, "发送: %s", test_messages[i]);
            
            int err = send(sock, test_messages[i], strlen(test_messages[i]), 0);
            if (err < 0) {
                ESP_LOGE(TAG, "发送失败: %d", errno);
                break;
            }

            // 接收服务器响应
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ESP_LOGW(TAG, "接收超时");
                } else {
                    ESP_LOGE(TAG, "接收错误: %d", errno);
                }
                break;
            } else if (len == 0) {
                ESP_LOGW(TAG, "连接已关闭");
                break;
            } else {
                rx_buffer[len] = 0; // 确保字符串结束
                ESP_LOGI(TAG, "收到: %s", rx_buffer);
            }

            // 如果是最后一条消息（quit），等待服务器的告别消息
            if (i == sizeof(test_messages)/sizeof(test_messages[0]) - 1) {
                len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
                if (len > 0) {
                    rx_buffer[len] = 0;
                    ESP_LOGI(TAG, "服务器告别: %s", rx_buffer);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1000));  // 每条消息间隔1秒
        }

        ESP_LOGI(TAG, "测试完成，关闭连接");
        close(sock);
        break;  // 测试完成后退出任务
    }

    vTaskDelete(NULL);
}

void tcp_client_test_case(void)
{
    xTaskCreate(tcp_client_task, "tcp_client", 8192, NULL, 5, NULL);
}

#define UDP_PORT 8889
#define UDP_SERVER_IP "192.168.0.100"  // Python服务器IP地址
#define UDP_PREFIX_LEN 24  // "UDP服务器已收到: " 的长度

static void udp_server_task(void *pvParameters)
{
    char rx_buffer[BUFFER_SIZE];
    char tx_buffer[BUFFER_SIZE];
    
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "无法创建UDP socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket绑定失败: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP服务器启动在端口 %d", UDP_PORT);

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (1) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, 
                          (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (len < 0) {
            ESP_LOGE(TAG, "接收错误: %d", errno);
            continue;
        }

        rx_buffer[len] = 0; // 确保字符串结束
        ESP_LOGI(TAG, "收到来自 %s:%d 的数据: %s", 
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), rx_buffer);

        // 检查是否是退出命令
        if (strcmp(rx_buffer, "quit") == 0) {
            const char *goodbye = "再见!";
            sendto(sock, goodbye, strlen(goodbye), 0, 
                   (struct sockaddr *)&client_addr, sizeof(client_addr));
            continue;
        }

        // 安全地准备回复数据
        memset(tx_buffer, 0, sizeof(tx_buffer));
        strlcpy(tx_buffer, "UDP服务器已收到: ", sizeof(tx_buffer));
        strlcat(tx_buffer, rx_buffer, sizeof(tx_buffer) - UDP_PREFIX_LEN);  // 确保有足够空间

        sendto(sock, tx_buffer, strlen(tx_buffer), 0, 
               (struct sockaddr *)&client_addr, sizeof(client_addr));
    }

    close(sock);
    vTaskDelete(NULL);
}

static void udp_client_task(void *pvParameters)
{
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT)
    };
    inet_aton(UDP_SERVER_IP, &dest_addr.sin_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "无法创建UDP socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // 设置接收超时
    struct timeval timeout = {
        .tv_sec = 5,
        .tv_usec = 0
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // 测试消息列表
    const char *test_messages[] = {
        "Hello from ESP32 UDP!",
        "这是UDP测试消息",
        "UDP Testing 123",
        "quit"
    };

    char rx_buffer[BUFFER_SIZE];
    struct sockaddr_in source_addr;
    socklen_t source_addr_len = sizeof(source_addr);

    for (int i = 0; i < sizeof(test_messages)/sizeof(test_messages[0]); i++) {
        ESP_LOGI(TAG, "发送UDP消息: %s", test_messages[i]);
        
        int err = sendto(sock, test_messages[i], strlen(test_messages[i]), 0,
                        (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "发送失败: %d", errno);
            // break;
        }

        // 接收服务器响应
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&source_addr, &source_addr_len);
        
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG, "接收超时");
            } else {
                ESP_LOGE(TAG, "接收错误: %d", errno);
            }
        } else {
            rx_buffer[len] = 0;
            ESP_LOGI(TAG, "收到来自 %s:%d 的响应: %s",
                     inet_ntoa(source_addr.sin_addr), 
                     ntohs(source_addr.sin_port), 
                     rx_buffer);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  // 每条消息间隔1秒
    }

    ESP_LOGI(TAG, "UDP测试完成");
    close(sock);
    vTaskDelete(NULL);
}

void udp_test_case(void)
{
    // 创建UDP服务器任务
    xTaskCreate(udp_server_task, "udp_server", 8192, NULL, 5, NULL);
    
    // 等待2秒让服务器启动
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // 创建UDP客户端任务
    xTaskCreate(udp_client_task, "udp_client", 8192, NULL, 5, NULL);
}