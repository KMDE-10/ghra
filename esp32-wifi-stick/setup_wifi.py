#!/usr/bin/env python3
"""
ESP32-S3 WiFi Adapter — Host Setup Script

1. Connects ESP32 to WiFi via serial commands
2. Switches ESP32 to SLIP mode
3. Creates a TUN interface and bridges packets via SLIP over serial
4. Configures IP routing and DNS

Run as root: sudo python3 setup_wifi.py [options]
"""

import sys
import os
import json
import time
import signal
import struct
import fcntl
import argparse
import select
import serial
import serial.tools.list_ports

BAUD = 4000000
SERIAL_PORT = "/dev/ttyACM0"

# SLIP framing (RFC 1055)
SLIP_END     = 0xC0
SLIP_ESC     = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

# TUN device constants
TUNSETIFF = 0x400454CA
IFF_TUN   = 0x0001
IFF_NO_PI = 0x1000


def find_esp32():
    for p in serial.tools.list_ports.comports():
        vid = p.vid or 0
        if vid in (0x1A86, 0x303A):
            return p.device
    for p in serial.tools.list_ports.comports():
        if "ttyACM" in p.device or "ttyUSB" in p.device:
            return p.device
    return None


def serial_command(ser, cmd, timeout=25):
    ser.reset_input_buffer()
    ser.write((json.dumps(cmd, separators=(",", ":")) + "\n").encode())
    ser.flush()
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ser.in_waiting:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
                if msg.get("event") in ("scanning", "connecting"):
                    print(f"    ... {msg.get('event', '')}")
                    continue
                return msg
            except json.JSONDecodeError:
                pass
        else:
            time.sleep(0.05)
    return {"error": "timeout"}


def run(cmd, check=True):
    import subprocess
    print(f"  $ {cmd}")
    return subprocess.run(cmd, shell=True, check=check, capture_output=True, text=True)


def open_tun(name="espwifi0"):
    """Create and return a TUN device file descriptor."""
    tun_fd = os.open("/dev/net/tun", os.O_RDWR)
    ifr = struct.pack("16sH", name.encode(), IFF_TUN | IFF_NO_PI)
    fcntl.ioctl(tun_fd, TUNSETIFF, ifr)
    return tun_fd, name


def slip_encode(packet):
    """SLIP-encode an IP packet."""
    out = bytearray([SLIP_END])
    for b in packet:
        if b == SLIP_END:
            out.append(SLIP_ESC)
            out.append(SLIP_ESC_END)
        elif b == SLIP_ESC:
            out.append(SLIP_ESC)
            out.append(SLIP_ESC_ESC)
        else:
            out.append(b)
    out.append(SLIP_END)
    return bytes(out)


class SlipDecoder:
    """Decode SLIP frames from a byte stream."""
    def __init__(self):
        self.buf = bytearray()
        self.esc = False

    def feed(self, data):
        """Feed raw bytes, yield complete IP packets."""
        packets = []
        for b in data:
            if self.esc:
                self.esc = False
                if b == SLIP_ESC_END:
                    self.buf.append(SLIP_END)
                elif b == SLIP_ESC_ESC:
                    self.buf.append(SLIP_ESC)
                else:
                    self.buf.append(b)
            elif b == SLIP_END:
                if len(self.buf) > 0:
                    packets.append(bytes(self.buf))
                    self.buf = bytearray()
            elif b == SLIP_ESC:
                self.esc = True
            else:
                self.buf.append(b)
        return packets


def bridge_loop(ser, tun_fd):
    """Main bridge: TUN <-> SLIP <-> Serial."""
    decoder = SlipDecoder()
    ser_fd = ser.fileno()
    tx_packets = 0
    rx_packets = 0

    while True:
        readable, _, _ = select.select([tun_fd, ser_fd], [], [], 1.0)

        for fd in readable:
            if fd == tun_fd:
                # Host -> ESP32: read IP packet from TUN, SLIP-encode, send over serial
                packet = os.read(tun_fd, 2048)
                if packet:
                    ser.write(slip_encode(packet))
                    tx_packets += 1

            elif fd == ser_fd:
                # ESP32 -> Host: read serial, SLIP-decode, write to TUN
                data = ser.read(ser.in_waiting or 1)
                if data:
                    for packet in decoder.feed(data):
                        # Validate: must be an IP packet (version 4 or 6)
                        if len(packet) >= 20 and (packet[0] >> 4) in (4, 6):
                            try:
                                os.write(tun_fd, packet)
                                rx_packets += 1
                            except OSError:
                                pass  # skip malformed packets


def main():
    parser = argparse.ArgumentParser(description="ESP32-S3 WiFi Adapter Setup")
    parser.add_argument("-p", "--port", help="Serial port")
    parser.add_argument("-s", "--ssid", help="WiFi SSID")
    parser.add_argument("--pass", dest="password", help="WiFi password (WPA2-PSK)")
    parser.add_argument("--user", help="Enterprise username (e.g. user@domain)")
    parser.add_argument("--identity", help="Enterprise outer identity")
    parser.add_argument("--scan", action="store_true", help="Just scan and exit")
    parser.add_argument("--metric", type=int, default=100, help="Route metric (default 100)")
    parser.add_argument("--iface", default="espwifi0", help="TUN interface name (default espwifi0)")
    args = parser.parse_args()

    if os.geteuid() != 0 and not args.scan:
        print("[-] This script needs root. Run with: sudo python3 setup_wifi.py ...")
        sys.exit(1)

    port = args.port or find_esp32() or SERIAL_PORT
    print(f"[*] Opening {port} @ {BAUD} baud...")

    import termios
    ser = serial.Serial(port, BAUD, timeout=1)

    # Disable hangup-on-close via termios (prevents DTR reset)
    fd = ser.fileno()
    attrs = termios.tcgetattr(fd)
    attrs[2] = attrs[2] & ~termios.HUPCL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    time.sleep(1)

    ser.reset_input_buffer()
    time.sleep(0.5)

    # Try ping a few times
    resp = {}
    for attempt in range(3):
        resp = serial_command(ser, {"action": "ping"}, timeout=5)
        if resp.get("response") == "pong":
            break
        print(f"    ... waiting for ESP32 (attempt {attempt + 1})")
        ser.reset_input_buffer()
        time.sleep(2)

    if resp.get("response") != "pong":
        print(f"[-] No response from ESP32: {resp}")
        ser.close()
        sys.exit(1)
    print("[+] ESP32 WiFi adapter responding")

    if args.scan:
        resp = serial_command(ser, {"action": "scan"}, timeout=30)
        if "networks" in resp:
            print(f"\n{'SSID':<32} {'RSSI':>5} {'CH':>3} {'ENC':>4}")
            print("-" * 48)
            for n in sorted(resp["networks"], key=lambda x: x["rssi"], reverse=True):
                print(f"{n['ssid']:<32} {n['rssi']:>5} {n.get('ch', '?'):>3} {n.get('enc', '?'):>4}")
            print(f"\n{resp['count']} networks found")
        else:
            print(json.dumps(resp, indent=2))
        ser.close()
        return

    if not args.ssid:
        print("[-] No SSID specified. Use --ssid. Run with --scan to see networks.")
        ser.close()
        sys.exit(1)

    # Scan first (wakes up radio, helps with hotspot discovery)
    print(f"\n[*] Scanning for '{args.ssid}'...")
    serial_command(ser, {"action": "scan"}, timeout=15)

    # Connect to WiFi (with retries)
    cmd = {"action": "connect", "ssid": args.ssid}
    if args.password:
        cmd["pass"] = args.password
    if args.user:
        cmd["user"] = args.user
    if args.identity:
        cmd["identity"] = args.identity

    resp = {}
    for attempt in range(3):
        print(f"[*] Connecting to '{args.ssid}' (attempt {attempt + 1}/3)...")
        resp = serial_command(ser, cmd, timeout=25)
        if resp.get("status") == "connected":
            break
        print(f"    ... failed, retrying in 3s")
        time.sleep(3)

    if resp.get("status") != "connected":
        print(f"[-] WiFi connection failed: {json.dumps(resp)}")
        ser.close()
        sys.exit(1)

    wifi_ip = resp.get("ip", "?")
    dns_server = resp.get("dns", resp.get("gw", ""))
    print(f"[+] WiFi connected!")
    print(f"    WiFi IP:  {wifi_ip}")
    print(f"    Gateway:  {resp.get('gw', '?')}")
    print(f"    DNS:      {dns_server}")
    print(f"    RSSI:     {resp.get('rssi', '?')} dBm")

    # Switch to SLIP mode
    print(f"\n[*] Switching to SLIP mode...")
    resp = serial_command(ser, {"action": "startslip"}, timeout=10)
    if resp.get("status") != "ok":
        print(f"[-] Failed to start SLIP: {json.dumps(resp)}")
        ser.close()
        sys.exit(1)

    slip_local = resp.get("slip_ip", "10.0.0.1")
    slip_peer = resp.get("peer_ip", "10.0.0.2")
    dns1 = resp.get("dns1", dns_server)
    dns2 = resp.get("dns2", "")
    print(f"[+] SLIP ready — ESP32={slip_local}, Host={slip_peer}")

    # Create TUN interface
    print(f"\n[*] Creating TUN interface {args.iface}...")
    tun_fd, iface = open_tun(args.iface)
    print(f"[+] TUN device {iface} created")

    # Configure interface manually
    print(f"\n[*] Configuring {iface}...")
    nm_conn = "ESP32 WiFi"
    run(f"ip addr add {slip_peer}/32 peer {slip_local} dev {iface}")
    run(f"ip link set {iface} up")
    run(f"ip link set {iface} mtu 1500", check=False)

    # Add default route
    print(f"\n[*] Adding default route (metric {args.metric})...")
    run(f"ip route add default via {slip_local} dev {iface} metric {args.metric}")

    # Tell NM about it (unmanaged, just for display in the indicator)
    run(f"nmcli connection delete '{nm_conn}'", check=False)
    dns_str = dns1
    if dns2 and dns2 != "0.0.0.0":
        dns_str += f" {dns2}"
    run(f"nmcli connection add type tun ifname {iface} con-name '{nm_conn}' "
        f"ipv4.method manual ipv4.addresses {slip_peer}/32 "
        f"ipv4.gateway {slip_local} ipv4.dns '{dns_str}' "
        f"ipv4.route-metric {args.metric} "
        f"connection.autoconnect no", check=False)

    # Configure DNS
    resolv_backup = None
    if dns1 and dns1 != "0.0.0.0":
        print(f"\n[*] Configuring DNS ({dns1})...")
        try:
            with open("/etc/resolv.conf", "r") as f:
                resolv_backup = f.read()
            if dns1 not in resolv_backup:
                with open("/etc/resolv.conf", "w") as f:
                    f.write(f"# Added by ESP32 WiFi adapter\n")
                    f.write(f"nameserver {dns1}\n")
                    if dns2 and dns2 != "0.0.0.0":
                        f.write(f"nameserver {dns2}\n")
                    f.write(resolv_backup)
        except Exception:
            pass

    print(f"\n{'='*50}")
    print(f"  ESP32-S3 WiFi Adapter is UP!")
    print(f"  Interface:  {iface}")
    print(f"  Host IP:    {slip_peer}")
    print(f"  Gateway:    {slip_local} (ESP32)")
    print(f"  WiFi IP:    {wifi_ip}")
    print(f"  DNS:        {dns1}")
    print(f"  Baud:       {BAUD} (~400 KB/s max)")
    print(f"{'='*50}")
    print(f"\nBridging packets... Press Ctrl+C to stop.\n")

    def cleanup(sig=None, frame=None):
        print(f"\n[*] Shutting down...")
        run(f"ip route del default via {slip_local} dev {iface}", check=False)
        run(f"ip link set {iface} down", check=False)
        run(f"nmcli connection delete '{nm_conn}'", check=False)
        if resolv_backup:
            try:
                with open("/etc/resolv.conf", "w") as f:
                    f.write(resolv_backup)
            except Exception:
                pass
        os.close(tun_fd)
        ser.close()
        print("[+] WiFi adapter stopped.")
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    try:
        bridge_loop(ser, tun_fd)
    except KeyboardInterrupt:
        cleanup()


if __name__ == "__main__":
    main()
