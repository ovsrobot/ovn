#!/usr/bin/env python3

import argparse
import atexit
import os
import socket
import sys
import threading

import binascii
from scapy.all import *  # noqa: F401,F403
from scapy.all import raw


_MAX_PATTERN_LEN = 1024 * 32


def write_pidfile(pidfile):
    pid = str(os.getpid())
    with open(pidfile, 'w') as f:
        f.write(pid)


def remove_pidfile(pidfile):
    os.remove(pidfile)


def check_pidfile(pidfile):
    if os.path.exists(pidfile):
        with open(pidfile, 'r') as f:
            pid = f.read().strip()
            try:
                pid = int(pid)
                if os.path.exists(f"/proc/{pid}"):
                    print("Scapy server is already running:", pid)
                    sys.exit(1)
            except ValueError:
                sys.exit(1)


def fork():
    try:
        pid = os.fork()
        if pid > 0:
            sys.exit(0)
    except OSError as e:
        print("Fork failed:", e)
        sys.exit(1)


def daemonize(pidfile):
    fork()
    check_pidfile(pidfile)
    write_pidfile(pidfile)
    atexit.register(remove_pidfile, pidfile)


def process(data):
    try:
        return binascii.hexlify(raw(eval(data)))
    except Exception:
        return ""


def handle_client(client_socket):
    data = client_socket.recv(_MAX_PATTERN_LEN)
    data = data.strip()
    if data:
        client_socket.send(process(data))
    client_socket.close()


def main(pidfile, socket_path):
    daemonize(pidfile)

    if os.path.exists(socket_path):
        os.remove(socket_path)

    server_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server_socket.bind(socket_path)
    server_socket.listen()

    while True:
        client_socket, _ = server_socket.accept()
        handler = threading.Thread(target=handle_client, args=(client_socket,))
        handler.start()


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pidfile", help="Path to PID file",
                        default="/tmp/scapy.pid")
    parser.add_argument("--sockfile", help="Path to Unix socket file",
                        default="/tmp/scapy.sock")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    main(args.pidfile, args.sockfile)
