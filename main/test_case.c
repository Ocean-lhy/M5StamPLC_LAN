#include "test_case.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

#define IPERF_DEFAULT_PORT 5001
#define IPERF_TCP_BUF_SIZE 2048
#define IPERF_UDP_BUF_SIZE 1472       // UDP MTU大小优化
#define IPERF_DEFAULT_INTERVAL 1       // 每秒报告一次
#define IPERF_DEFAULT_TIME 30          // 测试30秒

typedef struct {
    bool is_server;           // 服务器模式
    bool is_udp;              // UDP模式
    uint16_t port;            // 端口
    uint32_t time;            // 测试持续时间(秒)
    uint32_t interval;        // 报告间隔(秒)
    char *remote_host;        // 远程主机IP(客户端模式)
    uint32_t buffer_len;      // 缓冲区长度
} iperf_cfg_t;

static uint64_t iperf_get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
}

static void iperf_report_stats(uint32_t bytes, uint64_t start_time, uint64_t end_time, bool is_udp)
{
    float duration = (end_time - start_time) / 1000.0;
    float bandwidth = (bytes * 8.0 / duration) / 1000.0; // kbps
    
    ESP_LOGI(TAG, "传输 %u 字节，耗时 %.2f 秒", (unsigned int)bytes, duration);
    ESP_LOGI(TAG, "带宽: %.2f kbps (%.2f KB/s)", bandwidth, bandwidth / 8.0);
    
    if (is_udp) {
        ESP_LOGI(TAG, "UDP模式，数据包大小: %d 字节", IPERF_UDP_BUF_SIZE);
    }
}

static void iperf_tcp_server_task(void *pvParameters)
{
    iperf_cfg_t *cfg = (iperf_cfg_t *)pvParameters;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "无法创建socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket绑定失败: %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    
    if (listen(listen_sock, 5) < 0) {
        ESP_LOGE(TAG, "Socket监听失败: %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "iperf TCP服务器启动在端口 %d", cfg->port);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (sock < 0) {
            ESP_LOGE(TAG, "无法接受连接: %d", errno);
            continue;
        }
        
        ESP_LOGI(TAG, "客户端已连接: %s:%d", 
                inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        char *buffer = malloc(IPERF_TCP_BUF_SIZE);
        if (!buffer) {
            ESP_LOGE(TAG, "内存分配失败");
            close(sock);
            continue;
        }
        
        uint64_t start_time = iperf_get_time_ms();
        uint64_t last_report_time = start_time;
        uint64_t current_time;
        uint32_t total_bytes = 0;
        uint32_t interval_bytes = 0;
        
        while (1) {
            int len = recv(sock, buffer, IPERF_TCP_BUF_SIZE, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "接收错误: %d", errno);
                break;
            } else if (len == 0) {
                break;
            }
            
            total_bytes += len;
            interval_bytes += len;
            
            current_time = iperf_get_time_ms();
            
            if (current_time - last_report_time >= cfg->interval * 1000) 
            {
                iperf_report_stats(interval_bytes, last_report_time, current_time, false);
                last_report_time = current_time;
                interval_bytes = 0;
            }
            
            if (cfg->time > 0 && current_time - start_time >= cfg->time * 1000) 
            {
                break;
            }
        }
        
        current_time = iperf_get_time_ms();
        ESP_LOGI(TAG, "测试结束 - 总结");
        iperf_report_stats(total_bytes, start_time, current_time, false);
        
        free(buffer);
        close(sock);
        break;
    }
    
    close(listen_sock);
    free(cfg);
    vTaskDelete(NULL);
}

// 针对性能测试优化的TCP客户端任务
static void iperf_tcp_client_task(void *pvParameters)
{
    iperf_cfg_t *cfg = (iperf_cfg_t *)pvParameters;
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    
    if (sock < 0) {
        ESP_LOGE(TAG, "无法创建socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // TCP性能优化设置
    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)); // 禁用Nagle算法
    
    // 增大发送和接收缓冲区
    int snd_buf = 256 * 1024;  // 256KB发送缓冲区
    int rcv_buf = 256 * 1024;  // 256KB接收缓冲区
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &snd_buf, sizeof(snd_buf));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcv_buf, sizeof(rcv_buf));

    // 设置KEEPALIVE
    int keepalive = 1;
    int keepidle = 1;
    int keepintvl = 1;
    int keepcnt = 3;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    struct sockaddr_in remote_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->port),
    };
    
    if (inet_aton(cfg->remote_host, &remote_addr.sin_addr) == 0) {
        ESP_LOGE(TAG, "无效的服务器地址: %s", cfg->remote_host);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "连接到iperf服务器: %s:%d", cfg->remote_host, cfg->port);
    
    if (connect(sock, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0) {
        ESP_LOGE(TAG, "连接服务器失败: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    char *buffer = malloc(IPERF_TCP_BUF_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "内存分配失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    // 填充发送缓冲区
    memset(buffer, 0, IPERF_TCP_BUF_SIZE);
    
    ESP_LOGI(TAG, "开始iperf TCP带宽测试: %d秒", (unsigned int)cfg->time);
    
    uint64_t start_time = iperf_get_time_ms();
    uint64_t last_report_time = start_time;
    uint64_t current_time;
    uint32_t total_bytes = 0;
    uint32_t interval_bytes = 0;
    
    uint64_t end_time = start_time + cfg->time * 1000;
    
    while (iperf_get_time_ms() < end_time) {
        int sent = send(sock, buffer, IPERF_TCP_BUF_SIZE, 0);
        if (sent < 0) {
            ESP_LOGE(TAG, "发送失败: %d", errno);
            break;
        }
        
        total_bytes += sent;
        interval_bytes += sent;
        
        current_time = iperf_get_time_ms();
        
        if (current_time - last_report_time >= cfg->interval * 1000) {
            iperf_report_stats(interval_bytes, last_report_time, current_time, false);
            last_report_time = current_time;
            interval_bytes = 0;
        }
    }
    
    current_time = iperf_get_time_ms();
    ESP_LOGI(TAG, "测试结束 - 总结");
    iperf_report_stats(total_bytes, start_time, current_time, false);
    
    free(buffer);
    close(sock);
    free(cfg);
    vTaskDelete(NULL);
}

static void iperf_udp_server_task(void *pvParameters)
{
    iperf_cfg_t *cfg = (iperf_cfg_t *)pvParameters;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    
    if (sock < 0) {
        ESP_LOGE(TAG, "无法创建socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // 增加接收缓冲区大小，以支持分片数据包
    int rcv_buf = 256 * 1024;  // 256KB接收缓冲区
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcv_buf, sizeof(rcv_buf));
    
    // 设置UDP数据包大小
    #define UDP_LARGE_PACKET_SIZE 32768  // 支持32KB的UDP数据包
    
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket绑定失败: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "iperf UDP服务器启动在端口 %d，支持分片数据包", cfg->port);
    
    // 使用更大的缓冲区
    char *buffer = malloc(UDP_LARGE_PACKET_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "内存分配失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    uint64_t start_time = 0;
    uint64_t last_report_time = 0;
    uint64_t current_time;
    uint32_t total_bytes = 0;
    uint32_t interval_bytes = 0;
    bool test_running = false;
    
    while (1) {
        // 使用更大的接收缓冲区
        int len = recvfrom(sock, buffer, UDP_LARGE_PACKET_SIZE, 0, 
                         (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (len < 0) {
            ESP_LOGE(TAG, "接收错误: %d", errno);
            break;
        }
        
        if (!test_running) {
            test_running = true;
            start_time = iperf_get_time_ms();
            last_report_time = start_time;
            ESP_LOGI(TAG, "UDP测试开始，接收来自 %s:%d 的数据", 
                     inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        }
        
        total_bytes += len;
        interval_bytes += len;
        
        current_time = iperf_get_time_ms();
        
        if (current_time - last_report_time >= cfg->interval * 1000) {
            iperf_report_stats(interval_bytes, last_report_time, current_time, true);
            ESP_LOGI(TAG, "最大包大小: %d 字节", len);  // 记录最大包大小
            last_report_time = current_time;
            interval_bytes = 0;
        }
        
        // 如果超过测试时间，则退出
        if (cfg->time > 0 && test_running && current_time - start_time >= cfg->time * 1000) {
            break;
        }
    }
    
    if (test_running) {
        current_time = iperf_get_time_ms();
        ESP_LOGI(TAG, "测试结束 - 总结");
        iperf_report_stats(total_bytes, start_time, current_time, true);
    }
    
    free(buffer);
    close(sock);
    free(cfg);
    vTaskDelete(NULL);
}
// 针对性能测试优化的UDP客户端任务
static void iperf_udp_client_task(void *pvParameters)
{
    iperf_cfg_t *cfg = (iperf_cfg_t *)pvParameters;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    
    if (sock < 0) {
        ESP_LOGE(TAG, "无法创建socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // 增加：设置发送缓冲区大小
    int snd_buf = 512 * 1024;  // 256KB发送缓冲区
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &snd_buf, sizeof(snd_buf));
    
    struct sockaddr_in remote_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->port),
    };
    
    if (inet_aton(cfg->remote_host, &remote_addr.sin_addr) == 0) {
        ESP_LOGE(TAG, "无效的服务器地址: %s", cfg->remote_host);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    char *buffer = malloc(IPERF_UDP_BUF_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "内存分配失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    // 填充发送缓冲区
    memset(buffer, 0, IPERF_UDP_BUF_SIZE);
    
    ESP_LOGI(TAG, "开始iperf UDP带宽测试: %d秒", (unsigned int)cfg->time);
    ESP_LOGI(TAG, "发送到服务器: %s:%d", cfg->remote_host, cfg->port);
    
    uint64_t start_time = iperf_get_time_ms();
    uint64_t last_report_time = start_time;
    uint64_t current_time;
    uint32_t total_bytes = 0;
    uint32_t interval_bytes = 0;
    uint32_t consecutive_errors = 0;  // 增加：连续错误计数
    
    uint64_t end_time = start_time + cfg->time * 1000;
    
    while (iperf_get_time_ms() < end_time) {
        int sent = sendto(sock, buffer, IPERF_UDP_BUF_SIZE, 0,
                        (struct sockaddr *)&remote_addr, sizeof(remote_addr));
        if (sent < 0) {
            ESP_LOGE(TAG, "发送失败: %d, errno: %d", sent, errno);
            consecutive_errors++;
            if (consecutive_errors > 10) {  // 如果连续失败10次，退出测试
                ESP_LOGE(TAG, "连续发送失败次数过多，退出测试");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));  // 发送失败时增加延时
            continue;
        }
        
        consecutive_errors = 0;  // 重置连续错误计数
        total_bytes += sent;
        interval_bytes += sent;
        
        current_time = iperf_get_time_ms();
        
        if (current_time - last_report_time >= cfg->interval * 1000) {
            iperf_report_stats(interval_bytes, last_report_time, current_time, true);
            last_report_time = current_time;
            interval_bytes = 0;
        }
        
        // 增加动态延时控制
        // vTaskDelay(pdMS_TO_TICKS(1));  // 增加延时到1ms
    }
    
    current_time = iperf_get_time_ms();
    ESP_LOGI(TAG, "测试结束 - 总结");
    iperf_report_stats(total_bytes, start_time, current_time, true);
    
    free(buffer);
    close(sock);
    free(cfg);
    vTaskDelete(NULL);
}

void iperf_tcp_test_case(bool is_server, char *remote_host, uint16_t port, uint32_t time)
{
    iperf_cfg_t *cfg = malloc(sizeof(iperf_cfg_t));
    if (!cfg) {
        ESP_LOGE(TAG, "内存分配失败");
        return;
    }
    
    cfg->is_server = is_server;
    cfg->is_udp = false;
    cfg->port = (port > 0) ? port : IPERF_DEFAULT_PORT;
    cfg->time = (time > 0) ? time : IPERF_DEFAULT_TIME;
    cfg->interval = IPERF_DEFAULT_INTERVAL;
    cfg->remote_host = remote_host;
    cfg->buffer_len = IPERF_TCP_BUF_SIZE;
    
    if (is_server) {
        ESP_LOGI(TAG, "启动iperf TCP服务器，端口: %d", cfg->port);
        xTaskCreate(iperf_tcp_server_task, "iperf_tcp_server", 8192, cfg, (configMAX_PRIORITIES - 1), NULL);
    } else {
        if (!remote_host) {
            ESP_LOGE(TAG, "客户端模式需要指定服务器地址");
            free(cfg);
            return;
        }
        ESP_LOGI(TAG, "启动iperf TCP客户端，连接到: %s:%d", remote_host, cfg->port);
        xTaskCreate(iperf_tcp_client_task, "iperf_tcp_client", 32*1024, cfg, (configMAX_PRIORITIES - 1), NULL);
    }
}

void iperf_udp_test_case(bool is_server, char *remote_host, uint16_t port, uint32_t time)
{
    iperf_cfg_t *cfg = malloc(sizeof(iperf_cfg_t));
    if (!cfg) {
        ESP_LOGE(TAG, "内存分配失败");
        return;
    }
    
    cfg->is_server = is_server;
    cfg->is_udp = true;
    cfg->port = (port > 0) ? port : IPERF_DEFAULT_PORT;
    cfg->time = (time > 0) ? time : IPERF_DEFAULT_TIME;
    cfg->interval = IPERF_DEFAULT_INTERVAL;
    cfg->remote_host = remote_host;
    cfg->buffer_len = IPERF_UDP_BUF_SIZE;
    
    if (is_server) 
    {
        ESP_LOGI(TAG, "启动iperf UDP服务器，端口: %d", cfg->port);
        xTaskCreate(iperf_udp_server_task, "iperf_udp_server", 8192, cfg, (configMAX_PRIORITIES - 1), NULL);
    }
    else 
    {
        if (!remote_host) {
            ESP_LOGE(TAG, "客户端模式需要指定服务器地址");
            free(cfg);
            return;
        }
        ESP_LOGI(TAG, "启动iperf UDP客户端，连接到: %s:%d", remote_host, cfg->port);
        xTaskCreate(iperf_udp_client_task, "iperf_udp_client", 8192, cfg, (configMAX_PRIORITIES - 1), NULL);
    }
}

void iperf_test_case(void)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // 设置iperf服务器IP地址
    char *server_ip = "192.168.0.100";  // 确保这是正确的服务器IP
    
    // iperf_tcp_test_case(true, server_ip, 5001, 120);

    // 启动TCP客户端测试
    // ESP_LOGI(TAG, "开始TCP带宽测试...");
    iperf_tcp_test_case(false, server_ip, 5001, 2*60*60);

    // iperf_udp_test_case(true, server_ip, 5003, 120);

    // 启动UDP客户端测试
    // iperf_udp_test_case(false, server_ip, 5003, 120);
}

// 数据传输模式
typedef enum {
    SEND_ONLY,      // 仅发送数据
    RECEIVE_ONLY,   // 仅接收数据
    SEND_RECEIVE    // 发送并接收数据
} transfer_mode_t;

// socket配置结构体
typedef struct {
    bool is_enabled;       // 是否启用此socket
    bool is_server;        // 服务器模式(true)或客户端模式(false)
    bool is_udp;           // UDP(true)或TCP(false)
    uint16_t port;         // 端口
    char *remote_host;     // 远程主机IP
    transfer_mode_t mode;  // 传输模式
    uint32_t data_size;    // 每次发送数据大小
    uint32_t interval_ms;  // 发送间隔(ms)
    uint32_t test_time;    // 测试时间(秒)
} socket_config_t;

// 通用数据收发任务 - 简化版，只发送文字消息
static void socket_simple_task(void *pvParameters) {
    socket_config_t *cfg = (socket_config_t *)pvParameters;
    int sock = -1;
    char buffer[256];  // 较小的缓冲区用于文本消息
    
    // 准备特定的消息
    char message[128];
    snprintf(message, sizeof(message), "来自 %s %s 的消息 (端口:%d)",
             cfg->is_server ? "服务器" : "客户端",
             cfg->is_udp ? "UDP" : "TCP",
             cfg->port);
    
    if (cfg->is_udp) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    } else {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    }
    
    if (sock < 0) {
        ESP_LOGE(TAG, "无法创建socket: %d", errno);
        goto cleanup;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    
    if (cfg->is_server) {
        // 服务器模式
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "绑定端口失败: %d", errno);
            goto cleanup;
        }
        
        if (!cfg->is_udp) {
            // TCP服务器需要listen和accept
            if (listen(sock, 5) < 0) {
                ESP_LOGE(TAG, "监听失败: %d", errno);
                goto cleanup;
            }
            
            ESP_LOGI(TAG, "TCP服务器等待连接，端口: %d", cfg->port);
            
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_sock = accept(sock, (struct sockaddr*)&client_addr, &addr_len);
            
            if (client_sock < 0) {
                ESP_LOGE(TAG, "接受连接失败: %d", errno);
                goto cleanup;
            }
            
            ESP_LOGI(TAG, "客户端已连接: %s:%d", 
                    inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            
            close(sock);
            sock = client_sock;
        } else {
            ESP_LOGI(TAG, "UDP服务器已启动，端口: %d", cfg->port);
        }
    } else {
        // 客户端模式
        inet_aton(cfg->remote_host, &addr.sin_addr);
        
        if (!cfg->is_udp) {
            ESP_LOGI(TAG, "TCP客户端连接到: %s:%d", cfg->remote_host, cfg->port);
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                ESP_LOGE(TAG, "连接失败: %d", errno);
                goto cleanup;
            }
        }
    }
    
    // 处理发送和接收
    if (cfg->mode == SEND_ONLY || cfg->mode == SEND_RECEIVE) {
        // 发送消息
        ESP_LOGI(TAG, "发送消息: %s", message);
        
        int sent = 0;
        if (cfg->is_udp && !cfg->is_server) {
            sent = sendto(sock, message, strlen(message), 0, 
                        (struct sockaddr*)&addr, sizeof(addr));
        } else {
            sent = send(sock, message, strlen(message), 0);
        }
        
        if (sent < 0) {
            ESP_LOGE(TAG, "发送失败: %d", errno);
        } else {
            ESP_LOGI(TAG, "成功发送 %d 字节", sent);
        }
    }
    
    // 接收消息
    if (cfg->mode == RECEIVE_ONLY || cfg->mode == SEND_RECEIVE) {
        ESP_LOGI(TAG, "等待接收消息...");
        
        // 设置接收超时
        struct timeval timeout = {
            .tv_sec = 10,  // 10秒超时
            .tv_usec = 0
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
        int received = 0;
        if (cfg->is_udp && cfg->is_server) {
            struct sockaddr_in src_addr;
            socklen_t addr_len = sizeof(src_addr);
            received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                               (struct sockaddr*)&src_addr, &addr_len);
                               
            if (received > 0) {
                buffer[received] = '\0';
                ESP_LOGI(TAG, "收到来自 %s:%d 的UDP消息: %s", 
                        inet_ntoa(src_addr.sin_addr), ntohs(src_addr.sin_port), buffer);
            }
        } else {
            received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (received > 0) {
                buffer[received] = '\0';
                ESP_LOGI(TAG, "收到消息: %s", buffer);
            }
        }
        
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG, "接收超时");
            } else {
                ESP_LOGE(TAG, "接收错误: %d", errno);
            }
        } else if (received == 0) {
            ESP_LOGW(TAG, "连接已关闭");
        }
    }
    
    // 等待一段时间后关闭
    vTaskDelay(pdMS_TO_TICKS(5000));
    
cleanup:
    if (sock >= 0) close(sock);
    free(cfg);
    vTaskDelete(NULL);
}

void multi_socket_send_receive_test(void)
{
    // 服务器IP地址
    char *server_ip = "192.168.0.101";
    
    // 配置8个socket，每个socket发送不同文字
    socket_config_t socket_cfgs[8] = {
        // socket 0: TCP服务器，发送文字
        {
            .is_enabled = true,
            .is_server = true,
            .is_udp = false,
            .port = 5001,
            .remote_host = NULL,
            .mode = SEND_ONLY,
            .data_size = 0,  // 不需要，使用预设文本
            .interval_ms = 0,
            .test_time = 0   // 不需要测试时间
        },
        
        // socket 1: TCP客户端，接收文字
        {
            .is_enabled = true,
            .is_server = false,
            .is_udp = false,
            .port = 5001,
            .remote_host = server_ip,
            .mode = RECEIVE_ONLY,
            .data_size = 0,
            .interval_ms = 0,
            .test_time = 0
        },
        
        // socket 2: UDP服务器，发送文字
        {
            .is_enabled = true,
            .is_server = true,
            .is_udp = true,
            .port = 5002,
            .remote_host = NULL,
            .mode = SEND_ONLY,
            .data_size = 0,
            .interval_ms = 0,
            .test_time = 0
        },
        
        // socket 3: UDP客户端，接收文字
        {
            .is_enabled = true,
            .is_server = false,
            .is_udp = true,
            .port = 5002,
            .remote_host = server_ip,
            .mode = RECEIVE_ONLY,
            .data_size = 0,
            .interval_ms = 0,
            .test_time = 0
        },
        
        // socket 4: TCP客户端，发送文字
        {
            .is_enabled = true,
            .is_server = false,
            .is_udp = false,
            .port = 5003,
            .remote_host = server_ip,
            .mode = SEND_ONLY,
            .data_size = 0,
            .interval_ms = 0,
            .test_time = 0
        },
        
        // socket 5: TCP服务器，接收文字
        {
            .is_enabled = true,
            .is_server = true,
            .is_udp = false,
            .port = 5003,
            .remote_host = NULL,
            .mode = RECEIVE_ONLY,
            .data_size = 0,
            .interval_ms = 0,
            .test_time = 0
        },
        
        // socket 6: UDP客户端，发送文字
        {
            .is_enabled = true,
            .is_server = false,
            .is_udp = true,
            .port = 5004,
            .remote_host = server_ip,
            .mode = SEND_ONLY,
            .data_size = 0,
            .interval_ms = 0,
            .test_time = 0
        },
        
        // socket 7: UDP服务器，接收文字
        {
            .is_enabled = true,
            .is_server = true,
            .is_udp = true,
            .port = 5004,
            .remote_host = NULL,
            .mode = RECEIVE_ONLY,
            .data_size = 0,
            .interval_ms = 0,
            .test_time = 0
        }
    };
    
    ESP_LOGI(TAG, "启动多socket文字通信测试...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // 先启动所有服务器
    for (int i = 0; i < 8; i++) {
        if (!socket_cfgs[i].is_enabled || !socket_cfgs[i].is_server) {
            continue;
        }
        
        socket_config_t *cfg = malloc(sizeof(socket_config_t));
        if (!cfg) {
            ESP_LOGE(TAG, "内存分配失败");
            continue;
        }
        
        // 复制配置
        memcpy(cfg, &socket_cfgs[i], sizeof(socket_config_t));
        
        // 启动服务器任务
        char task_name[32];
        snprintf(task_name, sizeof(task_name), "sock%d_%s_%s", 
                i, cfg->is_udp ? "udp" : "tcp", 
                cfg->mode == SEND_ONLY ? "send" : "recv");
        
        ESP_LOGI(TAG, "启动%s服务器 #%d, 端口: %d, 模式: %s", 
                cfg->is_udp ? "UDP" : "TCP", i, cfg->port,
                cfg->mode == SEND_ONLY ? "发送" : "接收");
        
        xTaskCreate(socket_simple_task, task_name, 4096, cfg, 5, NULL);
    }
    
    // 等待服务器启动
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 启动所有客户端
    for (int i = 0; i < 8; i++) {
        if (!socket_cfgs[i].is_enabled || socket_cfgs[i].is_server) {
            continue;
        }
        
        socket_config_t *cfg = malloc(sizeof(socket_config_t));
        if (!cfg) {
            ESP_LOGE(TAG, "内存分配失败");
            continue;
        }
        
        // 复制配置
        memcpy(cfg, &socket_cfgs[i], sizeof(socket_config_t));
        
        // 启动客户端任务
        char task_name[32];
        snprintf(task_name, sizeof(task_name), "sock%d_%s_%s", 
                i, cfg->is_udp ? "udp" : "tcp", 
                cfg->mode == SEND_ONLY ? "send" : "recv");
        
        ESP_LOGI(TAG, "启动%s客户端 #%d, 连接到: %s:%d, 模式: %s", 
                cfg->is_udp ? "UDP" : "TCP", i, cfg->remote_host, cfg->port,
                cfg->mode == SEND_ONLY ? "发送" : "接收");
        
        xTaskCreate(socket_simple_task, task_name, 4096, cfg, 5, NULL);
    }
}
