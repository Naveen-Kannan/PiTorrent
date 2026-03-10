#!/usr/bin/env python3
"""
Demo: distribute a file across Raspberry Pis via BitTorrent over Bluetooth.

    # Seeders: split input.txt across Pis, receive output.txt back
    python3 demo.py send input.txt /dev/cu.usbserial-A /dev/cu.usbserial-B ...

    # Listener: join with 0 chunks, receive the file from peers
    python3 demo.py listen /dev/cu.usbserial-X

The 'send' command saves state to .demo_state.json so 'listen' can load
the file metadata (hashes, file_id) without needing the original file.
"""

import argparse
import binascii
import json
import os
import random
import struct
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

output_lock = threading.Lock()
output_written = False


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


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


def safe_read(ser, n):
    try:
        return ser.read(n)
    except serial.SerialException:
        return b''


def send_to_pi(port_path, payload, pi_idx, original_len, baud=115200):
    label = f"Pi {pi_idx}"
    print(f"[{label}] Opening {port_path}...")

    try:
        ser = serial.Serial(port_path, baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"[{label}] ERROR: {e}")
        return

    time.sleep(0.3)
    ser.reset_input_buffer()

    print(f"[{label}] Waiting for ready (0xAA)...")
    while True:
        b = safe_read(ser, 1)
        if len(b) == 0:
            continue
        if b[0] == 0xAA:
            print(f"[{label}] Ready!")
            break

    time.sleep(0.1)
    ser.reset_input_buffer()

    print(f"[{label}] Sending {len(payload)} bytes...")
    ser.write(payload)
    ser.flush()

    print(f"[{label}] Waiting for ACK (0x55)...")
    start = time.time()
    while time.time() - start < 30:
        b = safe_read(ser, 1)
        if len(b) == 0:
            continue
        if b[0] == 0x55:
            print(f"[{label}] ACK!")
            break
    else:
        print(f"[{label}] WARNING: no ACK within 30s")

    print(f"[{label}] --- Console ---")
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
                    sys.stdout.write(f"[{label}] {line}\n")
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
    with open(STATE_FILE, 'r') as f:
        return json.load(f)


def cmd_send(args):
    with open(args.input_file, 'rb') as f:
        raw = f.read()
    original_len = len(raw)
    file_data = pad_file(raw)
    file_id = crc32(file_data)

    chunk_hashes = []
    for i in range(NUM_CHUNKS):
        chunk = file_data[i * CHUNK_SIZE:(i + 1) * CHUNK_SIZE]
        chunk_hashes.append(crc32(chunk))

    print(f"Input:  {args.input_file} ({original_len} bytes, padded to {FILE_SIZE})")
    print(f"File ID: {file_id:#010x}  |  {NUM_CHUNKS} chunks x {CHUNK_SIZE} bytes")

    num_pis = len(args.ports)
    pi_chunks = partition_chunks(NUM_CHUNKS, num_pis)
    for i, chunks in enumerate(pi_chunks):
        print(f"  Pi {i} ({args.ports[i]}): chunks {sorted(chunks)}")

    save_state(file_data, chunk_hashes, file_id, original_len)
    print(f"State saved to {STATE_FILE} (for listener)")

    payloads = []
    for i in range(num_pis):
        payloads.append(build_payload(file_data, chunk_hashes, file_id, pi_chunks[i]))

    threads = []
    for i in range(num_pis):
        t = threading.Thread(target=send_to_pi,
                             args=(args.ports[i], payloads[i], i, original_len, args.baud))
        t.daemon = True
        t.start()
        threads.append(t)

    print(f"\n{num_pis} seeder thread(s) started.  Ctrl-C to exit.\n")
    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nExiting...")


def cmd_listen(args):
    state = load_state()
    file_data = bytes.fromhex(state['file_hex'])
    chunk_hashes = state['chunk_hashes']
    file_id = state['file_id']
    original_len = state.get('original_len', FILE_SIZE)

    print(f"Loaded state: file_id={file_id:#010x}, {NUM_CHUNKS} chunks")
    print(f"Listener on {args.port} — starting with 0 chunks\n")

    payload = build_payload(file_data, chunk_hashes, file_id, [])
    send_to_pi(args.port, payload, 0, original_len, args.baud)


def main():
    parser = argparse.ArgumentParser(
        description='BitTorrent over Bluetooth Demo',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  python3 demo.py send input.txt /dev/cu.usbserial-10 /dev/cu.usbserial-110
  python3 demo.py listen /dev/cu.usbserial-210
""")
    parser.add_argument('--baud', type=int, default=115200)
    sub = parser.add_subparsers(dest='cmd')

    p_send = sub.add_parser('send', help='Split file across Pis (seeder mode)')
    p_send.add_argument('input_file', help='File to distribute (padded/truncated to 4096 bytes)')
    p_send.add_argument('ports', nargs='+', help='Serial ports for seeder Pis')

    p_listen = sub.add_parser('listen', help='Join with 0 chunks (listener mode)')
    p_listen.add_argument('port', help='Serial port for listener Pi')

    args = parser.parse_args()
    if args.cmd == 'send':
        cmd_send(args)
    elif args.cmd == 'listen':
        cmd_listen(args)
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
