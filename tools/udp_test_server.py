#!/usr/bin/env python3
import socket
import argparse

def udp_server(host='0.0.0.0', port=8889):
    """UDP服务器主函数"""
    # 创建UDP socket
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind((host, port))
    
    print(f"UDP服务器启动在 {host}:{port}")
    
    try:
        while True:
            data, addr = server.recvfrom(1024)
            message = data.decode('utf-8')
            print(f"收到来自 {addr} 的数据: {message}")
            
            # 检查是否是退出命令
            if message.strip() == "quit":
                print(f"客户端 {addr} 请求断开连接")
                server.sendto("再见!".encode('utf-8'), addr)
                continue
            
            # 发送响应
            response = f"Python UDP服务器已收到: {message}"
            server.sendto(response.encode('utf-8'), addr)
            
    except KeyboardInterrupt:
        print("\n服务器正在关闭...")
    finally:
        server.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP服务器测试工具')
    parser.add_argument('--host', default='0.0.0.0', help='服务器监听地址')
    parser.add_argument('--port', type=int, default=8889, help='服务器监听端口')
    
    args = parser.parse_args()
    udp_server(args.host, args.port) 