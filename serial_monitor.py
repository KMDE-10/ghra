import serial, sys, threading
ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.1)
ser.reset_input_buffer()
def reader():
    while True:
        d = ser.read(256)
        if d:
            sys.stdout.buffer.write(d)
            sys.stdout.flush()
t = threading.Thread(target=reader, daemon=True)
t.start()
try:
    while True:
        c = input()
        ser.write((c + '\n').encode())
except KeyboardInterrupt:
    pass
ser.close()
