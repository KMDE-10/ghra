#!/usr/bin/env python3
"""
ESP32-S3 WiFi Stick - Host Controller

Communicates with the ESP32-S3 WiFi stick firmware over serial.
Provides a CLI to scan, connect, make HTTP requests, etc.
"""

import sys
import json
import time
import argparse
import serial
import serial.tools.list_ports

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200
READ_TIMEOUT = 20


def find_esp32_port():
    """Auto-detect ESP32 serial port."""
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = (p.description or "").lower()
        vid = p.vid or 0
        # CH340/CH9102 or ESP32 native USB
        if vid in (0x1A86, 0x303A) or "ch340" in desc or "cp210" in desc or "esp32" in desc:
            return p.device
    # Fallback
    for p in ports:
        if "ttyACM" in p.device or "ttyUSB" in p.device:
            return p.device
    return None


class WiFiStick:
    def __init__(self, port=None, baud=DEFAULT_BAUD):
        self.port = port or find_esp32_port() or DEFAULT_PORT
        self.baud = baud
        self.ser = None

    def open(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=1)
        time.sleep(2)  # Wait for ESP32 reset after serial connection
        # Drain any startup messages
        while self.ser.in_waiting:
            line = self.ser.readline().decode("utf-8", errors="replace").strip()
            if line:
                try:
                    msg = json.loads(line)
                    if msg.get("event") == "ready":
                        print(f"[+] ESP32 WiFi Stick ready (v{msg.get('version', '?')})")
                except json.JSONDecodeError:
                    pass

    def close(self):
        if self.ser:
            self.ser.close()

    def send(self, cmd_dict, timeout=READ_TIMEOUT):
        """Send a JSON command and return the parsed response."""
        cmd = json.dumps(cmd_dict, separators=(",", ":"))
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()

        deadline = time.time() + timeout
        responses = []
        while time.time() < deadline:
            if self.ser.in_waiting:
                line = self.ser.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                    # Skip intermediate events
                    if msg.get("event") in ("scanning", "connecting"):
                        print(f"    ... {msg['event']}")
                        continue
                    return msg
                except json.JSONDecodeError:
                    pass
            else:
                time.sleep(0.05)
        return {"error": "timeout"}

    def scan(self):
        return self.send({"action": "scan"}, timeout=30)

    def connect(self, ssid, password="", user="", identity=""):
        cmd = {"action": "connect", "ssid": ssid}
        if password:
            cmd["pass"] = password
        if user:
            cmd["user"] = user
        if identity:
            cmd["identity"] = identity
        return self.send(cmd, timeout=25)

    def disconnect(self):
        return self.send({"action": "disconnect"})

    def status(self):
        return self.send({"action": "status"})

    def http(self, url, method="GET", body=""):
        cmd = {"action": "http", "url": url, "method": method}
        if body:
            cmd["body"] = body
        return self.send(cmd, timeout=30)

    def dns(self, host):
        return self.send({"action": "dns", "host": host})

    def ping(self):
        return self.send({"action": "ping"})


def interactive(stick):
    """Interactive CLI mode."""
    print("\nESP32-S3 WiFi Stick - Interactive Mode")
    print("Commands: scan, connect <ssid> [pass], eap <ssid> <user> <pass>, disconnect,")
    print("          status, http <url>, dns <host>, ping, quit\n")

    while True:
        try:
            raw = input("wifi> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not raw:
            continue

        parts = raw.split(maxsplit=2)
        cmd = parts[0].lower()

        if cmd in ("quit", "exit", "q"):
            break
        elif cmd == "scan":
            result = stick.scan()
            if "networks" in result:
                print(f"\n{'SSID':<32} {'RSSI':>5} {'CH':>3}")
                print("-" * 44)
                for net in sorted(result["networks"], key=lambda n: n["rssi"], reverse=True):
                    print(f"{net['ssid']:<32} {net['rssi']:>5} {net['channel']:>3}")
                print(f"\n{result['count']} networks found\n")
            else:
                print(json.dumps(result, indent=2))

        elif cmd == "connect":
            if len(parts) < 2:
                print("Usage: connect <ssid> [password]")
                continue
            ssid = parts[1]
            password = parts[2] if len(parts) > 2 else ""
            result = stick.connect(ssid, password)
            if result.get("status") == "connected":
                print(f"[+] Connected to {result['ssid']}")
                print(f"    IP:      {result.get('ip')}")
                print(f"    Gateway: {result.get('gateway')}")
                print(f"    DNS:     {result.get('dns')}")
                print(f"    RSSI:    {result.get('rssi')} dBm")
            else:
                print(f"[-] Connection failed: {json.dumps(result)}")

        elif cmd == "eap":
            if len(parts) < 3:
                print("Usage: eap <ssid> <user@domain> <password>")
                continue
            eap_parts = raw.split(maxsplit=3)
            ssid = eap_parts[1]
            user = eap_parts[2]
            password = eap_parts[3] if len(eap_parts) > 3 else ""
            if not password:
                import getpass
                password = getpass.getpass("Password: ")
            result = stick.connect(ssid, password=password, user=user)
            if result.get("status") == "connected":
                print(f"[+] Connected to {result['ssid']} (enterprise)")
                print(f"    IP:      {result.get('ip')}")
                print(f"    Gateway: {result.get('gateway')}")
                print(f"    DNS:     {result.get('dns')}")
                print(f"    RSSI:    {result.get('rssi')} dBm")
            else:
                print(f"[-] Connection failed: {json.dumps(result)}")

        elif cmd == "disconnect":
            result = stick.disconnect()
            print("[+] Disconnected" if result.get("status") == "ok" else json.dumps(result))

        elif cmd == "status":
            result = stick.status()
            if result.get("connected"):
                print(f"[+] Connected to {result['ssid']}")
                print(f"    IP:      {result.get('ip')}")
                print(f"    RSSI:    {result.get('rssi')} dBm")
                print(f"    Channel: {result.get('channel')}")
                print(f"    MAC:     {result.get('mac')}")
            else:
                print(f"[-] Not connected (MAC: {result.get('mac')})")

        elif cmd == "http":
            if len(parts) < 2:
                print("Usage: http <url>")
                continue
            result = stick.http(parts[1])
            print(f"HTTP {result.get('code', '?')}")
            if "body" in result:
                body = result["body"]
                if len(body) > 500:
                    print(body[:500] + "...[truncated]")
                else:
                    print(body)

        elif cmd == "dns":
            if len(parts) < 2:
                print("Usage: dns <hostname>")
                continue
            result = stick.dns(parts[1])
            if "ip" in result:
                print(f"{result['host']} -> {result['ip']}")
            else:
                print(f"DNS lookup failed: {json.dumps(result)}")

        elif cmd == "ping":
            result = stick.ping()
            print(f"pong (uptime: {result.get('uptime', '?')} ms)")

        else:
            print(f"Unknown command: {cmd}")


def main():
    parser = argparse.ArgumentParser(description="ESP32-S3 WiFi Stick Host Controller")
    parser.add_argument("-p", "--port", help="Serial port (auto-detect if omitted)")
    parser.add_argument("-b", "--baud", type=int, default=DEFAULT_BAUD, help="Baud rate")
    parser.add_argument("command", nargs="?", help="One-shot command (scan/status/disconnect)")
    parser.add_argument("args", nargs="*", help="Command arguments")
    args = parser.parse_args()

    stick = WiFiStick(port=args.port, baud=args.baud)

    print(f"[*] Opening {stick.port} @ {stick.baud} baud...")
    try:
        stick.open()
    except serial.SerialException as e:
        print(f"[-] Failed to open serial port: {e}")
        sys.exit(1)

    try:
        if args.command:
            cmd = args.command.lower()
            if cmd == "scan":
                result = stick.scan()
                print(json.dumps(result, indent=2))
            elif cmd == "connect":
                if not args.args:
                    print("Usage: wifi_host.py connect <ssid> [password]")
                    sys.exit(1)
                ssid = args.args[0]
                password = args.args[1] if len(args.args) > 1 else ""
                result = stick.connect(ssid, password)
                print(json.dumps(result, indent=2))
            elif cmd == "status":
                result = stick.status()
                print(json.dumps(result, indent=2))
            elif cmd == "disconnect":
                result = stick.disconnect()
                print(json.dumps(result, indent=2))
            elif cmd == "http":
                if not args.args:
                    print("Usage: wifi_host.py http <url>")
                    sys.exit(1)
                result = stick.http(args.args[0])
                print(json.dumps(result, indent=2))
            else:
                print(f"Unknown command: {cmd}")
                sys.exit(1)
        else:
            interactive(stick)
    finally:
        stick.close()


if __name__ == "__main__":
    main()
