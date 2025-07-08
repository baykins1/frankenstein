# File: web_server.py (FINAL VERSION)
from flask import Flask, render_template
from flask_socketio import SocketIO, emit
import socket
import threading
from datetime import datetime, timezone, timedelta
import time
import json
from collections import deque

# --- Configuration ---
HOST_IP = '0.0.0.0'
WEB_SERVER_PORT = 8080
UDP_LISTENER_PORT = 5000
# If a device is silent for this many seconds, we count a failed packet.
PACKET_TIMEOUT_SECONDS = 2.0 

# --- State Management (to store data) ---
data_lock = threading.Lock()
device_data = {}
device_stats = {}
total_stats = {
    'total_packets': 0,
    'total_devices': 0,
    'start_time': datetime.now(timezone.utc).isoformat()
}

packet_timestamps = deque()  # Store up to 10 minutes at 1Hz

# --- Flask & SocketIO Setup ---
app = Flask(__name__)
app.config['SECRET_KEY'] = 'a_very_secret_key!'
socketio = SocketIO(app, cors_allowed_origins="*")

# --- Web Server Route ---
@app.route('/')
def index():
    try:
        return render_template('index.html')
    except Exception as e:
        return f"Error: Could not find index.html. Make sure it is in a 'templates' subfolder. Details: {e}"

# --- SocketIO Event Handlers ---
@socketio.on('connect')
def handle_connect():
    print("Client connected")
    with data_lock:
        emit('initial_data', {
            'devices': device_data,
            'stats': device_stats,
            'total_stats': total_stats
        })

# --- Background Task for Missed Packets ---
def check_for_missed_packets():
    while True:
        time.sleep(1)
        with data_lock:
            now = datetime.now(timezone.utc)
            
            # Find benchmark device: active device with lowest failed_packets
            active_devices = [stats for stats in device_stats.values() if stats['total_packets'] > 10]
            if active_devices:
                benchmark = min(active_devices, key=lambda x: x['failed_packets'])
                avg_interval = PACKET_TIMEOUT_SECONDS  # Default fallback
                if benchmark['total_packets'] > 1:
                    # Estimate interval: total time / total packets
                    first_seen = datetime.fromisoformat(benchmark['last_seen']) - timedelta(
                        seconds=(benchmark['total_packets'] - 1) * PACKET_TIMEOUT_SECONDS)
                    total_time = (now - first_seen).total_seconds()
                    avg_interval = total_time / benchmark['total_packets']
                
                for device_id, stats in device_stats.items():
                    last_seen_dt = datetime.fromisoformat(stats['last_seen'])
                    elapsed = (now - last_seen_dt).total_seconds()
                    
                    expected_interval = avg_interval * 2  # tolerate 2×
                    missed = int(elapsed // expected_interval)
                    
                    if missed > 0:
                        stats['failed_packets'] += missed
                        stats['last_seen'] = now.isoformat()
                        print(f"Device {device_id} missed {missed} packet(s).")
                        socketio.emit('stats_update', {'total_stats': total_stats, 'device_stats': device_stats})

        
# --- Main UDP Listener for Data from the Bridge ---
def udp_listener():
    """Listens for UDP packets, parses JSON, and updates the state."""
    udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_socket.bind(('', UDP_LISTENER_PORT))
    print(f"UDP Listener is ready on port {UDP_LISTENER_PORT}")

    while True:
        try:
            data, addr = udp_socket.recvfrom(1024)
            payload = json.loads(data.decode('utf-8'))
            device_id = payload['device_id']
            message = payload['message']
            device_ts = payload.get('device_ts')  # <-- NEW

            with data_lock:
                now = datetime.now(timezone.utc)
                now_iso = now.isoformat()
                # Use device_ts if present, else fallback to now_iso
                msg_timestamp = device_ts if device_ts else now_iso

                # Track packet timestamps for rate calculation
                packet_timestamps.append(now)

                if device_id not in device_stats:
                    device_data[device_id] = []
                    now = datetime.now(timezone.utc)
                    elapsed = (now - datetime.fromisoformat(total_stats['start_time'])).total_seconds()
                    expected_interval = PACKET_TIMEOUT_SECONDS
                    estimated_missed = int(elapsed // expected_interval)
    
                    device_stats[device_id] = {
                        'total_packets': 0,
                        'failed_packets': estimated_missed,
                        'last_seen': msg_timestamp,
                        'failure_counted': False
                }
                total_stats['total_devices'] = len(device_data)

                message_payload = {
                    'timestamp': msg_timestamp,
                    'message': message,
                    'status': 'success'
                }

                device_data[device_id].append(message_payload)
                if len(device_data[device_id]) > 50: device_data[device_id].pop(0)

                device_stats[device_id]['total_packets'] += 1
                device_stats[device_id]['last_seen'] = msg_timestamp
                device_stats[device_id]['failure_counted'] = False
                total_stats['total_packets'] += 1

                # Calculate packets/min for the last 60 seconds
                cutoff = now - timedelta(seconds=60)
                packets_last_min = sum(1 for t in packet_timestamps if t > cutoff)
                total_stats['packets_per_min'] = packets_last_min

            update_package = {
                'device_mac': device_id,
                'stats': device_stats[device_id],
                'data': message_payload
            }
            
            socketio.emit('new_message', update_package)
            socketio.emit('stats_update', {'total_stats': total_stats, 'device_stats': device_stats})

        except json.JSONDecodeError:
            print(f"Warning: Received a non-JSON UDP packet.")
        except Exception as e:
            print(f"Error in UDP listener thread: {e}")
            time.sleep(1)

# --- Main Execution ---
if __name__ == '__main__':
    print("Starting the Web Server...")

    # Start the packet failure checker in a background thread
    failure_checker_thread = threading.Thread(target=check_for_missed_packets, daemon=True)
    failure_checker_thread.start()

    # Start the main UDP listener in a background thread
    listener_thread = threading.Thread(target=udp_listener, daemon=True)
    listener_thread.start()

    socketio.run(app, host=HOST_IP, port=WEB_SERVER_PORT, debug=True, use_reloader=False)