#!/usr/bin/env python3
import socket

HOST = "0.0.0.0"
PORT = 8080
BUFFER_SIZE = 65535

def start_server():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((HOST, PORT))

        while True:
            try:
                data, addr = sock.recvfrom(BUFFER_SIZE)
                log_entry(addr, data)
            except KeyboardInterrupt:
                break
            except Exception as e:
                print(f"ERROR: {e}")


def log_entry(addr: tuple, data: bytes):
    try:
        print(data.decode("utf-8"), end="", flush=True)
    except UnicodeDecodeError:
        print(data, flush=True)


if __name__ == "__main__":
    start_server()