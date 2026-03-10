#!/usr/bin/env python3
"""
One-command BitTorrent demo: auto-discover Pis, bootload, send file, collect output.

    python3 run.py                        # uses input.txt, all connected Pis
    python3 run.py myfile.txt             # custom input file
    python3 run.py --listen               # listener mode (0 chunks, needs .demo_state.json)
    python3 run.py --ports /dev/cu.X ...  # manual port selection
"""

import argparse
import binascii
import glob
import json
import os
import random
import struct
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed.  Run: pip3 install pyserial")
    sys.exit(1)

FILE_SIZE = 4096
CHUNK_SIZE = 256
NUM_CHUNKS = FILE_SIZE // CHUNK_SIZE
STATE_FILE = ".demo_state.json"
BIN_FILE = "btnode.bin"
BAUD = 115200

output_lock = threading.Lock()
output_written = False


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def short_name(port):
    return port.split("-")[-1] if "-" in port else port


def discover_ports():
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    if not ports:
        print("ERROR: no /dev/cu.usbserial-* devices found. Are Pis plugged in?")
        sys.exit(1)
    return ports


def bootload(port, bin_path):
    """Run my-install on one Pi. Returns True on success."""
    s = short_name(port)
    print(f"  [@{s}] bootloading...")
    try:
        result = subprocess.run(
            ["my-install", port, bin_path],
            timeout=30,
            capture_output=True,
        )
        if result.returncode == 0:
            print(f"  [@{s}] OK")
            return True
        else:
            print(f"  [@{s}] FAILED (exit {result.returncode})")
            return False
    except FileNotFoundError:
        print("ERROR: my-install not found. Is it on your PATH?")
        sys.exit(1)
    except subprocess.TimeoutExpired:
        print(f"  [@{s}] timed out — is the Pi power-cycled?")
        return False


def pad_file(data):
    if len(data) >= FILE_SIZE:
        return data[:FILE_SIZE]
    return data + b'\x00' * (FILE_SIZE - len(data))


def partition_chunks(num_chunks, num_pis):
    indices = list(range(num_chunks))
    random.shuffle(indices)
    assignment = [0] * num_chunks
    for pi in range(num_pis):
        assignment[indices[pi]] = pi
    for i in range(num_pis, num_chunks):
        assignment[indices[i]] = random.randint(0, num_pis - 1)
    pi_chunks = [[] for _ in range(num_pis)]
    for chunk_idx, pi_idx in enumerate(assignment):
        pi_chunks[pi_idx].append(chunk_idx)
    return pi_chunks


def build_payload(file_data, chunk_hashes, file_id, assigned_chunks):
    payload = struct.pack('<I', file_id)
    payload += struct.pack('<H', NUM_CHUNKS)
    payload += struct.pack('<H', CHUNK_SIZE)
    for h in chunk_hashes:
        payload += struct.pack('<I', h)
    payload += struct.pack('B', len(assigned_chunks))
    for idx in assigned_chunks:
        payload += struct.pack('B', idx)
        offset = idx * CHUNK_SIZE
        payload += file_data[offset:offset + CHUNK_SIZE]
    checksum = crc32(payload)
    payload += struct.pack('<I', checksum)
    return payload


def safe_read(ser, n):
    try:
        return ser.read(n)
    except serial.SerialException:
        return b''


def write_output(file_bytes, original_len, label):
    global output_written
    with output_lock:
        if output_written:
            return
        trimmed = file_bytes[:original_len] if original_len else file_bytes.rstrip(b'\x00')
        with open("output.txt", 'wb') as f:
            f.write(trimmed)
        output_written = True
        print(f"\n>>> [{label}] Wrote output.txt ({len(trimmed)} bytes)")


def send_to_pi(port_path, payload, pi_idx, original_len):
    label = f"Pi {pi_idx}"
    s = short_name(port_path)

    try:
        ser = serial.Serial(port_path, BAUD, timeout=0.5)
    except serial.SerialException as e:
        print(f"[{label} @{s}] ERROR: {e}")
        return

    time.sleep(0.3)
    ser.reset_input_buffer()

    print(f"[{label} @{s}] Waiting for ready...")
    while True:
        b = safe_read(ser, 1)
        if len(b) == 0:
            continue
        if b[0] == 0xAA:
            print(f"[{label} @{s}] Ready!")
            break

    time.sleep(0.1)
    ser.reset_input_buffer()

    print(f"[{label} @{s}] Sending {len(payload)} bytes...")
    ser.write(payload)
    ser.flush()

    print(f"[{label} @{s}] Waiting for ACK...")
    start = time.time()
    while time.time() - start < 30:
        b = safe_read(ser, 1)
        if len(b) == 0:
            continue
        if b[0] == 0x55:
            print(f"[{label} @{s}] ACK!")
            break
    else:
        print(f"[{label} @{s}] WARNING: no ACK within 30s")

    print(f"[{label} @{s}] --- Console ---")
    capturing = False
    hex_buf = []
    line_buf = ""

    try:
        while True:
            data = safe_read(ser, 256)
            if not data:
                continue
            line_buf += data.decode('ascii', errors='replace')

            while '\n' in line_buf:
                line, line_buf = line_buf.split('\n', 1)
                stripped = line.strip()

                if stripped == "===FILE_START===":
                    capturing = True
                    hex_buf = []
                elif stripped == "===FILE_END===":
                    capturing = False
                    try:
                        file_bytes = bytes.fromhex(''.join(hex_buf))
                        write_output(file_bytes, original_len, label)
                    except Exception as e:
                        print(f"\n[{label}] decode error: {e}")
                elif capturing:
                    hex_buf.append(stripped)
                else:
                    sys.stdout.write(f"[{label} @{s}] {line}\n")
                    sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


def save_state(file_data, chunk_hashes, file_id, original_len):
    state = {
        'file_hex': file_data.hex(),
        'chunk_hashes': chunk_hashes,
        'file_id': file_id,
        'original_len': original_len,
    }
    with open(STATE_FILE, 'w') as f:
        json.dump(state, f)


def load_state():
    if not os.path.exists(STATE_FILE):
        print(f"ERROR: {STATE_FILE} not found. Run in send mode first.")
        sys.exit(1)
    with open(STATE_FILE, 'r') as f:
        return json.load(f)


def do_send(ports, input_file):
    if not os.path.exists(input_file):
        print(f"ERROR: {input_file} not found")
        sys.exit(1)

    with open(input_file, 'rb') as f:
        raw = f.read()
    original_len = len(raw)
    file_data = pad_file(raw)
    file_id = crc32(file_data)

    chunk_hashes = [crc32(file_data[i*CHUNK_SIZE:(i+1)*CHUNK_SIZE]) for i in range(NUM_CHUNKS)]

    num_pis = len(ports)
    pi_chunks = partition_chunks(NUM_CHUNKS, num_pis)

    print(f"\nInput:  {input_file} ({original_len} bytes, padded to {FILE_SIZE})")
    print(f"File ID: {file_id:#010x}  |  {NUM_CHUNKS} chunks x {CHUNK_SIZE} bytes")
    for i, chunks in enumerate(pi_chunks):
        print(f"  Pi {i} (@{short_name(ports[i])}): chunks {sorted(chunks)}")

    save_state(file_data, chunk_hashes, file_id, original_len)
    print(f"State saved to {STATE_FILE} (for listener mode)\n")

    payloads = [build_payload(file_data, chunk_hashes, file_id, pi_chunks[i]) for i in range(num_pis)]

    threads = []
    for i in range(num_pis):
        t = threading.Thread(target=send_to_pi, args=(ports[i], payloads[i], i, original_len))
        t.daemon = True
        t.start()
        threads.append(t)

    print(f"{num_pis} Pi(s) running.  Ctrl-C to stop.\n")
    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nStopping...")


def do_listen(ports):
    state = load_state()
    file_data = bytes.fromhex(state['file_hex'])
    chunk_hashes = state['chunk_hashes']
    file_id = state['file_id']
    original_len = state.get('original_len', FILE_SIZE)

    print(f"\nListener mode: file_id={file_id:#010x}, {NUM_CHUNKS} chunks")
    print(f"Joining with 0 chunks on {len(ports)} Pi(s)\n")

    payload = build_payload(file_data, chunk_hashes, file_id, [])

    threads = []
    for i, port in enumerate(ports):
        t = threading.Thread(target=send_to_pi, args=(port, payload, i, original_len))
        t.daemon = True
        t.start()
        threads.append(t)

    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nStopping...")


def main():
    parser = argparse.ArgumentParser(
        description='One-command BitTorrent demo',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  python3 run.py                          # send input.txt to all Pis
  python3 run.py myfile.txt               # send custom file
  python3 run.py --listen                 # listener (0 chunks)
  python3 run.py --ports /dev/cu.X ...    # manual port selection
""")
    parser.add_argument('input_file', nargs='?', default='input.txt',
                        help='File to distribute (default: input.txt)')
    parser.add_argument('--listen', action='store_true',
                        help='Listener mode: join with 0 chunks')
    parser.add_argument('--ports', nargs='+',
                        help='Manual serial port list (auto-detected if omitted)')

    args = parser.parse_args()

    if args.ports:
        ports = args.ports
    else:
        ports = discover_ports()

    print(f"=== BitTorrent over Bluetooth Demo ===")
    print(f"Found {len(ports)} Pi(s): {', '.join(short_name(p) for p in ports)}")

    # bootload each Pi sequentially
    bin_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), BIN_FILE)
    if not os.path.exists(bin_path):
        print(f"ERROR: {bin_path} not found. Run 'make' first.")
        sys.exit(1)

    print(f"\nBootloading {BIN_FILE} to {len(ports)} Pi(s)...")
    for port in ports:
        if not bootload(port, bin_path):
            print(f"ERROR: bootload failed for {port}. Power-cycle the Pi and retry.")
            sys.exit(1)
    print("All Pis bootloaded.\n")

    if args.listen:
        do_listen(ports)
    else:
        do_send(ports, args.input_file)


if __name__ == '__main__':
    main()
