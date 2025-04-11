#!/usr/bin/env python3
import socket
import time
import argparse

def udp_client_test(host, port=8889):
    """UDP客户端测试程序"""
    # 创建UDP socket
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    # 设置超时
    client.settimeout(5)
    
    print(f"UDP客户端启动，目标服务器: {host}:{port}")
    
    # 测试消息列表
    test_messages = [
        "Hello from Python UDP!",
        "测试中文消息",
        "这是一条很长的测试消息" * 5,  # 测试长消息
        "Test message 1",
        "Test message 2",
        "quit"
    ]
    
    try:
        for message in test_messages:
            print(f"\n发送: {message}")
            client.sendto(message.encode('utf-8'), (host, port))
            
            try:
                # 接收服务器响应
                data, server = client.recvfrom(1024)
                response = data.decode('utf-8')
                print(f"接收: {response}")
                print(f"来自: {server}")
                
                # 如果是最后一条消息（quit），等待服务器的告别消息
                if message == "quit":
                    try:
                        data, _ = client.recvfrom(1024)
                        print(f"服务器告别: {data.decode('utf-8')}")
                    except socket.timeout:
                        print("未收到服务器告别消息")
                
            except socket.timeout:
                print("等待响应超时")
            
            time.sleep(1)  # 每条消息间隔1秒
            
    except Exception as e:
        print(f"发生错误: {e}")
    finally:
        client.close()
        print("\nUDP客户端已关闭")

def udp_stress_test(host, port=8889, message_count=100):
    """UDP压力测试"""
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.settimeout(1)
    
    print(f"开始UDP压力测试，将发送 {message_count} 条消息")
    
    success_count = 0
    timeout_count = 0
    start_time = time.time()
    
    try:
        for i in range(message_count):
            message = f"Stress test message {i}"
            client.sendto(message.encode('utf-8'), (host, port))
            
            try:
                data, _ = client.recvfrom(1024)
                success_count += 1
            except socket.timeout:
                timeout_count += 1
                
            if i % 10 == 0:  # 每10条消息显示一次进度
                print(f"进度: {i+1}/{message_count}")
                
    except Exception as e:
        print(f"压力测试出错: {e}")
    finally:
        end_time = time.time()
        duration = end_time - start_time
        
        print("\n压力测试结果:")
        print(f"总消息数: {message_count}")
        print(f"成功接收: {success_count}")
        print(f"超时次数: {timeout_count}")
        print(f"总耗时: {duration:.2f} 秒")
        print(f"平均每秒处理: {message_count/duration:.2f} 条消息")
        
        client.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP客户端测试工具')
    parser.add_argument('host', help='服务器IP地址')
    parser.add_argument('--port', type=int, default=8889, help='服务器端口号(默认: 8889)')
    parser.add_argument('--stress', action='store_true', help='执行压力测试')
    parser.add_argument('--count', type=int, default=100, help='压力测试消息数量(默认: 100)')
    
    args = parser.parse_args()
    
    if args.stress:
        udp_stress_test(args.host, args.port, args.count)
    else:
        udp_client_test(args.host, args.port) 