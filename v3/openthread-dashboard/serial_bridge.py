# File: serial_bridge.py (IMPROVED VERSION)
import serial
import socket
import threading
import time
import re
import json # We will use JSON to send structured data

class SerialBridge:
    def __init__(self, serial_port='/dev/ttyACM0', baud_rate=115200, 
                 web_server_ip='127.0.0.1', web_server_port=5000):
        self.serial_port = serial_port
        self.baud_rate = baud_rate
        self.web_server_ip = web_server_ip
        self.web_server_port = web_server_port
        
        self.serial_conn = None
        self.web_socket = None
        self.running = False
        # Regex to remove ANSI escape codes (for colors, etc.)
        self.ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
        
    def init_serial(self):
        """Initialize serial connection"""
        try:
            self.serial_conn = serial.Serial(
                port=self.serial_port,
                baudrate=self.baud_rate,
                timeout=1
            )
            print(f"Serial connection established on {self.serial_port}")
            return True
        except Exception as e:
            print(f"Failed to open serial port: {e}")
            return False
    
    def init_web_socket(self):
        """Initialize UDP socket for web server"""
        try:
            self.web_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            print(f"Web socket created for {self.web_server_ip}:{self.web_server_port}")
            return True
        except Exception as e:
            print(f"Failed to create web socket: {e}")
            return False
    
    def send_to_web(self, device_id, message, device_ts=None):
        """Send structured JSON message to web dashboard"""
        if self.web_socket:
            try:
                payload = {
                    'device_id': device_id,
                    'message': message,
                }
                if device_ts:
                    payload['device_ts'] = device_ts
                self.web_socket.sendto(
                    json.dumps(payload).encode('utf-8'),
                    (self.web_server_ip, self.web_server_port)
                )
            except Exception as e:
                print(f"Failed to send to web: {e}")
    
    def parse_and_clean_line(self, line):
        """Cleans line, parses for relevant data, and extracts device ID and timestamp"""
        # 1. Clean the line by removing ANSI escape codes
        cleaned_line = self.ansi_escape.sub('', line).strip()
        
        # 2. Extract Zephyr timestamp (e.g., [00:18:20.910,888])
        ts_match = re.match(r'\[(\d{2}:\d{2}:\d{2}\.\d{3}),\d+\]', cleaned_line)
        if ts_match:
            device_ts = ts_match.group(1)  # e.g., "00:18:20.910"
        else:
            device_ts = None

        # 3. Find our message and device ID
        match = re.search(r'hello world (\w+)', cleaned_line)
        if match:
            full_message = match.group(0)
            device_id = match.group(1)
            return device_id, full_message, device_ts
            
        return None, None, None
    
    def serial_reader(self):
        """Read from serial port, parse, and forward to web"""
        while self.running:
            try:
                if self.serial_conn and self.serial_conn.in_waiting > 0:
                    line = self.serial_conn.readline().decode('utf-8', errors='ignore')
                    if line:
                        device_id, message, device_ts = self.parse_and_clean_line(line)
                        if device_id and message:
                            self.send_to_web(device_id, message, device_ts)
                
                time.sleep(0.01)
                
            except Exception as e:
                print(f"Serial reader error: {e}")
                time.sleep(1)
    
    def start(self):
        """Start the bridge"""
        if not self.init_serial() or not self.init_web_socket():
            return
        
        self.running = True
        
        serial_thread = threading.Thread(target=self.serial_reader, daemon=True)
        serial_thread.start()
        
        print("Serial bridge started. Press Ctrl+C to stop.")
        
        try:
            while self.running:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\nStopping serial bridge...")
            self.stop()
    
    def stop(self):
        """Stop the bridge"""
        self.running = False
        if self.serial_conn: self.serial_conn.close()
        if self.web_socket: self.web_socket.close()
        print("Serial bridge stopped.")

# --- Main execution block remains the same ---
if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description='OpenThread Serial Bridge')
    parser.add_argument('--port', '-p', help='Serial port (e.g., /dev/ttyACM0, COM3)')
    parser.add_argument('--baud', '-b', type=int, default=115200, help='Baud rate')
    parser.add_argument('--web-ip', default='127.0.0.1', help='Web server IP')
    parser.add_argument('--web-port', type=int, default=5000, help='Web server port')
    parser.add_argument('--list-ports', action='store_true', help='List available ports')
    
    args = parser.parse_args()
    
    if args.list_ports:
        import serial.tools.list_ports
        ports = serial.tools.list_ports.comports()
        print("Available serial ports:")
        for port in ports:
            print(f"  {port.device} - {port.description}")
        exit(0)
    
    if not args.port:
        print("No serial port specified. Please use --port COMx")
        exit(1)
    
    bridge = SerialBridge(
        serial_port=args.port,
        baud_rate=args.baud,
        web_server_ip=args.web_ip,
        web_server_port=args.web_port
    )
    
    bridge.start()