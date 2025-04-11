#!/usr/bin/env python3
import socket
import threading
import argparse
import time

def handle_client(client_socket, addr):
    """处理单个客户端连接"""
    print(f"新客户端连接: {addr}")
    
    try:
        while True:
            # 接收数据
            data = client_socket.recv(1024).decode('utf-8')
            if not data:
                break
                
            print(f"收到来自 {addr} 的数据: {data}")
            
            # 检查是否是退出命令
            if data.strip() == "quit":
                print(f"客户端 {addr} 请求断开连接")
                client_socket.send("再见!".encode('utf-8'))
                break
                
            # 发送响应
            response = f"服务器已收到: {data}"
            client_socket.send(response.encode('utf-8'))
            
    except Exception as e:
        print(f"处理客户端 {addr} 时发生错误: {e}")
    finally:
        client_socket.close()
        print(f"客户端 {addr} 断开连接")

def tcp_server(host='0.0.0.0', port=8888):
    """TCP服务器主函数"""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server.bind((host, port))
        server.listen(5)
        print(f"服务器启动在 {host}:{port}")
        
        while True:
            client_sock, addr = server.accept()
            # 为每个客户端创建新线程
            client_thread = threading.Thread(
                target=handle_client,
                args=(client_sock, addr)
            )
            client_thread.start()
            
    except KeyboardInterrupt:
        print("\n服务器正在关闭...")
    except Exception as e:
        print(f"服务器错误: {e}")
    finally:
        server.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='TCP服务器测试工具')
    parser.add_argument('--host', default='0.0.0.0', help='服务器监听地址')
    parser.add_argument('--port', type=int, default=8888, help='服务器监听端口')
    
    args = parser.parse_args()
    tcp_server(args.host, args.port) 