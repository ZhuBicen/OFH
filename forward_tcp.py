import os
import socket
import sys
import time

# --- Configuration ---
FIFO_PATH = "/tmp/video_tunnel_out"  # The path to your FIFO. Make sure it exists!
REMOTE_IP = "172.16.26.124"  # Replace with your remote server's IP address
REMOTE_PORT = 1234        # Replace with your remote server's port
BUFFER_SIZE = 4096         # How much data to read from the FIFO at a time

def create_fifo_if_not_exists(fifo_path):
    """Creates the FIFO if it doesn't already exist."""
    if not os.path.exists(fifo_path):
        try:
            os.mkfifo(fifo_path)
            print(f"FIFO '{fifo_path}' created successfully.")
        except OSError as e:
            print(f"Error creating FIFO '{fifo_path}': {e}", file=sys.stderr)
            sys.exit(1)
    else:
        print(f"FIFO '{fifo_path}' already exists.")

def main():
    # create_fifo_if_not_exists(FIFO_PATH)
    fifo_file = open(FIFO_PATH, 'rb')
    print(f"Attempting to connect to {REMOTE_IP}:{REMOTE_PORT}...")
    while True:
        try:
            # Create a TCP/IP socket
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.connect((REMOTE_IP, REMOTE_PORT))
                print(f"Successfully connected to {REMOTE_IP}:{REMOTE_PORT}")

                print(f"Opening FIFO '{FIFO_PATH}' for reading...")
                # Open the FIFO in read-binary mode
                # The 'with' statement ensures the FIFO is closed properly
                print(f"FIFO '{FIFO_PATH}' opened. Waiting for data...")
                while True:
                    data = fifo_file.read(BUFFER_SIZE)
                    if data:
                        try:
                            sock.sendall(data)
                                # print(f"Sent {len(data)} bytes to remote.") # Uncomment for debugging
                        except BrokenPipeError:
                            print("Remote disconnected unexpectedly. Attempting to reconnect...")
                            break # Break inner loop to try reconnecting
                        except Exception as e:
                            print(f"Error sending data: {e}", file=sys.stderr)
                            break # Break inner loop to try reconnecting
        except ConnectionRefusedError:
            print(f"Connection refused by {REMOTE_IP}:{REMOTE_PORT}. Retrying in 5 seconds...")
            time.sleep(5)
        except socket.timeout:
            print("Connection timed out. Retrying in 5 seconds...")
            time.sleep(5)
        except OSError as e:
            print(f"OS error: {e}. Retrying in 5 seconds...", file=sys.stderr)
            time.sleep(5)
        except Exception as e:
            print(f"An unexpected error occurred: {e}. Retrying in 5 seconds...", file=sys.stderr)
            time.sleep(5)

if __name__ == "__main__":
    main()
