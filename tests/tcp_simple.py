#!/usr/bin/env python3
"""
Simple TCP client/server for testing network connectivity and data integrity.

This module provides TCP echo server and client functionality for testing
network connections, data transmission, and MD5 checksum verification.
"""
import socket
import time
import argparse
import sys
import hashlib
import random

DEFAULT_SRC_IP = "0.0.0.0"
DEFAULT_DST_IP = "0.0.0.0"
DEFAULT_PORT = 8080


def calculate_md5(data):
    """Calculate MD5 checksum of data."""
    if isinstance(data, str):
        data = data.encode('utf-8')
    return hashlib.md5(data).hexdigest()


def generate_random_bytes(size):
    """Generate random bytes that are valid UTF-8."""
    # Generate random printable ASCII characters (32-126) which are valid UTF-8
    return bytes([random.randint(32, 126) for _ in range(size)])


class TCPServer:
    """Simple TCP echo server using standard sockets."""

    def __init__(self, bind_ip, port):
        self.bind_ip = bind_ip
        self.port = port
        self.running = False
        self.server_socket = None

    def start(self):
        """Start the TCP server."""
        exit_code = 0
        self.running = True
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET,
                                      socket.SO_REUSEADDR, 1)

        try:
            self.server_socket.bind((self.bind_ip, self.port))
            self.server_socket.listen(5)
            print(f"TCP Server listening on {self.bind_ip}:{self.port}")

            while self.running:
                try:
                    client_socket, client_addr = self.server_socket.accept()
                    print(f"Connection from {client_addr[0]}:{client_addr[1]}")

                    # Handle client directly (single-threaded)
                    self._handle_client(client_socket, client_addr)

                except socket.error as e:
                    if self.running:
                        print(f"Socket error: {e}")

        except OSError as e:
            if e.errno == 99:  # Cannot assign requested address
                print(f"Error: Cannot bind to {self.bind_ip}:{self.port}")
            elif e.errno == 98:  # Address already in use
                print(f"Error: Port {self.port} is already in use.")
            else:
                print(f"Error binding to {self.bind_ip}:{self.port}: {e}")
        except KeyboardInterrupt:
            print("\nServer shutting down...")
            exit_code = 0
        except Exception as e:
            print(f"Unexpected server error: {e}")
            exit_code = 1
        finally:
            self.running = False
            if self.server_socket:
                self.server_socket.close()
        sys.exit(exit_code)

    def _handle_client(self, client_socket, client_addr):
        """Handle individual client connection."""
        total_bytes_received = 0
        try:
            while self.running:
                data = client_socket.recv(4096)
                if not data:
                    break
                client_socket.send(data)
                total_bytes_received += len(data)

            print(f"Total bytes received: {total_bytes_received}")
        except socket.error as e:
            print(f"Client {client_addr[0]}:{client_addr[1]} error: {e}")
        finally:
            client_socket.close()
            print(f"Connection closed with {client_addr[0]}:{client_addr[1]}")


class TCPClient:
    """Simple TCP client using standard sockets."""

    def __init__(self, src_ip, dst_ip, dst_port):
        self.src_ip = src_ip
        self.dst_ip = dst_ip
        self.dst_port = dst_port

    def connect_and_send(self, data_len, iterations=1, interval=0.1,
                         unique_data=False):
        """Connect to server and send data."""
        print(f"TCP Client connecting to {self.dst_ip}:{self.dst_port}")

        success_count = 0
        correct_responses = 0

        # Generate data once if not using unique data per iteration
        shared_data_bytes = None
        shared_md5 = None
        if not unique_data:
            shared_data_bytes = generate_random_bytes(data_len)
            shared_md5 = calculate_md5(shared_data_bytes)
            print(f"Generated shared buffer: {len(shared_data_bytes)} bytes, "
                  f"MD5: {shared_md5}")

        return_code = 0
        for i in range(iterations):
            iteration_result = self._process_iteration(
                i, data_len, unique_data, shared_data_bytes, shared_md5)
            if iteration_result['success']:
                success_count += 1
                if iteration_result['checksum_match']:
                    correct_responses += 1
            else:
                return_code = 1

            if i < iterations - 1:
                time.sleep(interval)

        print(f"Client completed: {success_count}/{iterations} "
              f"successful connections")
        print(f"MD5 checksum verification: "
              f"{correct_responses}/{success_count} correct")

        return_code = 0 if (correct_responses ==
                           success_count and success_count > 0) else 1
        return return_code

    def _process_iteration(self, iteration_idx, data_len, unique_data,
                          shared_data_bytes, shared_md5):
        """Process a single iteration of the client test."""
        try:
            # Create socket and connect
            client_socket = socket.socket(socket.AF_INET,
                                          socket.SOCK_STREAM)

            # Bind to specific source IP if provided
            if self.src_ip != "0.0.0.0":
                client_socket.bind((self.src_ip, 0))

            client_socket.connect((self.dst_ip, self.dst_port))
            print(f"Iteration {iteration_idx + 1}: Connected to server")

            # Prepare data and calculate MD5 checksum
            if unique_data:
                data_bytes = generate_random_bytes(data_len)
                original_md5 = calculate_md5(data_bytes)
                print(f"Iteration {iteration_idx + 1}: Generated unique data, "
                      f"MD5: {original_md5}")
            else:
                data_bytes = shared_data_bytes
                original_md5 = shared_md5
                print(f"Iteration {iteration_idx + 1}: Using shared buffer, "
                      f"MD5: {original_md5}")

            client_socket.send(data_bytes)
            print(f"Iteration {iteration_idx + 1}: Sent {len(data_bytes)} "
                  f"bytes")

            # Receive echo response
            response = self._receive_response(client_socket, len(data_bytes))
            print(f"Iteration {iteration_idx + 1}: Received {len(response)} "
                  f"bytes")

            # Calculate MD5 of received data
            received_md5 = calculate_md5(response)
            print(f"Iteration {iteration_idx + 1}: Received MD5: "
                  f"{received_md5}")

            # Verify checksum
            checksum_match = original_md5 == received_md5

            if checksum_match:
                print(f"Iteration {iteration_idx + 1}: ✓ MD5 checksum "
                      f"verified correctly")
            else:
                print(f"Iteration {iteration_idx + 1}: ✗ MD5 checksum "
                      f"mismatch!")

            client_socket.close()
            return {'success': True, 'checksum_match': checksum_match}

        except ConnectionRefusedError:
            print(f"Iteration {iteration_idx + 1}: Connection refused to "
                  f"{self.dst_ip}:{self.dst_port}")
            return {'success': False, 'checksum_match': False}
        except socket.timeout:
            print(f"Iteration {iteration_idx + 1}: Connection timeout to "
                  f"{self.dst_ip}:{self.dst_port}")
            return {'success': False, 'checksum_match': False}
        except OSError as os_error:
            self._handle_os_error(iteration_idx, os_error)
            return {'success': False, 'checksum_match': False}
        except socket.error as sock_error:
            print(f"Iteration {iteration_idx + 1}: Socket error: {sock_error}")
            return {'success': False, 'checksum_match': False}
        except Exception as general_error:
            print(f"Iteration {iteration_idx + 1}: Unexpected error: "
                  f"{general_error}")
            return {'success': False, 'checksum_match': False}

    def _receive_response(self, client_socket, bytes_to_receive):
        """Receive response data from server."""
        response = b""
        while len(response) < bytes_to_receive:
            chunk = client_socket.recv(
                min(4096, bytes_to_receive - len(response)))
            if not chunk:
                break
            response += chunk
        return response

    def _handle_os_error(self, iteration_idx, os_error):
        """Handle OS-specific errors."""
        if os_error.errno == 99:  # Cannot assign requested address
            print(f"Iteration {iteration_idx + 1}: Cannot bind to source IP "
                  f"{self.src_ip}")
        elif os_error.errno == 101:  # Network is unreachable
            print(f"Iteration {iteration_idx + 1}: Network unreachable to "
                  f"{self.dst_ip}:{self.dst_port}")
        else:
            print(f"Iteration {iteration_idx + 1}: Network error: {os_error}")


def main():
    """Main function to parse arguments and run TCP client or server."""
    parser = argparse.ArgumentParser(
        description="Simple TCP client/server for testing",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
COMMON OPTIONS:
  --mode {client,server}  Run in client or server mode (required)
  -p, --port PORT         TCP port (default: 8080)

CLIENT MODE OPTIONS:
  -s, --src-ip IP         Source IPv4 address (default: 0.0.0.0)
  -d, --dst-ip IP         Destination IPv4 address (default: 0.0.0.0)
  -n, --iterations N      Number of connections to make (default: 1)
  -I, --interval SECS     Seconds between connections (default: 0.1)
  -B, --payload-bytes N   Total TCP payload bytes (minimum: 500)
  --unique-data           Generate unique random data for each iteration

SERVER MODE OPTIONS:
  --bind-ip IP            Server bind IP address (default: 172.16.1.2)

""")

    # Mode selection (required)
    parser.add_argument("--mode", choices=['client', 'server'], required=True,
                       help=argparse.SUPPRESS)

    # Common arguments
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT,
                       help=argparse.SUPPRESS)

    # Client mode arguments
    parser.add_argument("-B", "--payload-bytes", type=int, default=None,
                       help=argparse.SUPPRESS)
    parser.add_argument("-s", "--src-ip", default=DEFAULT_SRC_IP,
                       help=argparse.SUPPRESS)
    parser.add_argument("-d", "--dst-ip", default=DEFAULT_DST_IP,
                       help=argparse.SUPPRESS)
    parser.add_argument("-I", "--interval", type=float, default=0.1,
                       help=argparse.SUPPRESS)
    parser.add_argument("-n", "--iterations", type=int, default=1,
                       help=argparse.SUPPRESS)
    parser.add_argument("--unique-data", action="store_true",
                       help=argparse.SUPPRESS)

    # Server mode arguments
    parser.add_argument("--bind-ip", default="0.0.0.0",
                       help=argparse.SUPPRESS)

    args = parser.parse_args()

    # Validate arguments
    if args.port < 1 or args.port > 65535:
        print(f"Error: Port {args.port} is out of valid range (1-65535)")
        sys.exit(1)

    if args.mode == 'client':
        if args.iterations < 1:
            print(f"Error: Iterations must be at least 1, "
                  f"got {args.iterations}")
            sys.exit(1)
        if args.interval < 0:
            print(f"Error: Interval cannot be negative, "
                  f"got {args.interval}")
            sys.exit(1)
        if args.payload_bytes is not None and args.payload_bytes < 500:
            print(f"Error: Payload bytes must be at least 500, "
                  f"got {args.payload_bytes}")
            sys.exit(1)

    if args.mode == 'server':
        # Run server
        server = TCPServer(args.bind_ip, args.port)
        server.start()
    elif args.mode == 'client':
        # Run client
        client = TCPClient(args.src_ip, args.dst_ip, args.port)
        success = client.connect_and_send(
            max(0, int(args.payload_bytes)), args.iterations,
            args.interval, args.unique_data)

        # Summary
        print(f"Client summary: payload_bytes={args.payload_bytes} "
              f"iterations={args.iterations} success={success}")

        sys.exit(success)


if __name__ == "__main__":
    main()
