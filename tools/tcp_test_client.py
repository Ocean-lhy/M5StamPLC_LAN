#!/usr/bin/env python3
import socket
import time
import sys
import argparse

def tcp_client_test(host, port=8080):
    """TCP客户端测试程序"""
    try:
        # 创建TCP连接
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print(f"已连接到服务器 {host}:{port}")

        test_messages = [
            "Hello from Python!",
            "测试中文消息",
            "Test message 1",
            "Test message 2",
            "quit"
        ]

        for message in test_messages:
            print(f"\n发送: {message}")
            sock.send(message.encode())
            
            # 接收服务器响应
            response = sock.recv(1024).decode()
            print(f"接收: {response}")
            
            time.sleep(1)  # 等待1秒再发送下一条消息

    except ConnectionRefusedError:
        print(f"无法连接到服务器 {host}:{port}")
    except Exception as e:
        print(f"发生错误: {e}")
    finally:
        sock.close()
        print("连接已关闭")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='TCP客户端测试工具')
    parser.add_argument('host', help='服务器IP地址')
    parser.add_argument('--port', type=int, default=8080, help='服务器端口号(默认: 8080)')
    
    args = parser.parse_args()
    tcp_client_test(args.host, args.port) 