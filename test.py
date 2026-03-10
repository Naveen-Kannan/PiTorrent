#!/usr/bin/env python3
"""
Mac-side test harness for bare-metal BitTorrent over Bluetooth.

Generates a random file, partitions chunks across Pis, sends each Pi its
assigned chunks over serial. The Pis then exchange chunks over Bluetooth
until all nodes have the complete file.

Usage:
    python3 test.py /dev/cu.usbserial-A /dev/cu.usbserial-B [...]
    python3 test.py --append /dev/cu.usbserial-C   # late joiner
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
    print("ERROR: pyserial not installed. Run: pip3 install pyserial")
    sys.exit(1)


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def generate_file(size):
    return os.urandom(size)


def partition_chunks(num_chunks, num_pis):
    """Randomly assign each chunk to exactly one Pi. Every Pi gets >= 1."""
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


def build_payload(file_data, chunk_size, chunk_hashes, file_id, assigned_chunks):
    """Build binary payload for one Pi."""
    num_chunks = len(file_data) // chunk_size

    payload = struct.pack('<I', file_id)
    payload += struct.pack('<H', num_chunks)
    payload += struct.pack('<H', chunk_size)

    for h in chunk_hashes:
        payload += struct.pack('<I', h)

    payload += struct.pack('B', len(assigned_chunks))

    for idx in assigned_chunks:
        payload += struct.pack('B', idx)
        offset = idx * chunk_size
        payload += file_data[offset:offset + chunk_size]

    checksum = crc32(payload)
    payload += struct.pack('<I', checksum)

    return payload


def send_to_pi(port_path, payload, pi_idx, baud=115200):
    """Open serial port, wait for ready byte, send payload, act as console."""
    print(f"[Pi {pi_idx}] Opening {port_path}...")

    try:
        ser = serial.Serial(port_path, baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"[Pi {pi_idx}] ERROR: {e}")
        return False

    time.sleep(0.3)
    ser.reset_input_buffer()

    print(f"[Pi {pi_idx}] Waiting for ready signal (0xAA)...")

    while True:
        b = ser.read(1)
        if len(b) == 0:
            continue
        if b[0] == 0xAA:
            print(f"[Pi {pi_idx}] Got ready signal!")
            break
        if 0x20 <= b[0] <= 0x7E or b[0] in (0x0A, 0x0D):
            sys.stdout.write(b.decode('ascii', errors='replace'))
            sys.stdout.flush()

    time.sleep(0.1)
    ser.reset_input_buffer()

    print(f"[Pi {pi_idx}] Sending {len(payload)} bytes...")
    ser.write(payload)
    ser.flush()

    print(f"[Pi {pi_idx}] Waiting for ACK (0x55)...")
    start = time.time()
    while time.time() - start < 30:
        b = ser.read(1)
        if len(b) == 0:
            continue
        if b[0] == 0x55:
            print(f"[Pi {pi_idx}] Chunks received OK!")
            break
        if 0x20 <= b[0] <= 0x7E or b[0] in (0x0A, 0x0D):
            sys.stdout.write(f"[Pi {pi_idx}] {chr(b[0])}")
            sys.stdout.flush()
        else:
            sys.stdout.write(f"[Pi {pi_idx}] <0x{b[0]:02X}>")
            sys.stdout.flush()
    else:
        print(f"[Pi {pi_idx}] WARNING: no ACK within 30s")

    print(f"[Pi {pi_idx}] --- Console output ---")
    try:
        while True:
            data = ser.read(256)
            if data:
                text = data.decode('ascii', errors='replace')
                for line in text.splitlines(True):
                    sys.stdout.write(f"[Pi {pi_idx}] {line}")
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    return True


STATE_FILE = ".bt_test_state.json"


def save_state(file_data, chunk_size, chunk_hashes, file_id, pi_chunks):
    """Save state so --append can add late joiners."""
    state = {
        'file_hex': file_data.hex(),
        'chunk_size': chunk_size,
        'chunk_hashes': chunk_hashes,
        'file_id': file_id,
        'pi_chunks': pi_chunks,
    }
    with open(STATE_FILE, 'w') as f:
        json.dump(state, f)


def load_state():
    with open(STATE_FILE, 'r') as f:
        state = json.load(f)
    state['file_data'] = bytes.fromhex(state.pop('file_hex'))
    return state


def main():
    parser = argparse.ArgumentParser(description='BitTorrent over Bluetooth Test Harness')
    parser.add_argument('ports', nargs='+', help='Serial ports for Pis')
    parser.add_argument('--file-size', type=int, default=4096)
    parser.add_argument('--chunk-size', type=int, default=256)
    parser.add_argument('--seed', type=int, default=None)
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--append', action='store_true',
                        help='Late joiner: load existing file state, assign remaining chunks')
    args = parser.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    if args.append:
        state = load_state()
        file_data = state['file_data']
        chunk_size = state['chunk_size']
        chunk_hashes = state['chunk_hashes']
        file_id = state['file_id']
        existing_assigned = set()
        for chunks in state['pi_chunks']:
            existing_assigned.update(chunks)

        num_chunks = len(file_data) // chunk_size
        unassigned = [i for i in range(num_chunks) if i not in existing_assigned]
        num_new = len(args.ports)

        new_pi_chunks = [[] for _ in range(num_new)]
        for pi in range(num_new):
            if unassigned:
                new_pi_chunks[pi].append(unassigned.pop(0))
        for c in unassigned:
            new_pi_chunks[random.randint(0, num_new - 1)].append(c)
        if not any(new_pi_chunks):
            for pi in range(num_new):
                new_pi_chunks[pi].append(random.randint(0, num_chunks - 1))

        for i, chunks in enumerate(new_pi_chunks):
            print(f"Late joiner Pi ({args.ports[i]}): chunks {chunks}")

        state['pi_chunks'].extend(new_pi_chunks)
        save_state(file_data, chunk_size, chunk_hashes, file_id, state['pi_chunks'])

        payloads = []
        for i in range(num_new):
            payload = build_payload(file_data, chunk_size, chunk_hashes, file_id, new_pi_chunks[i])
            payloads.append(payload)

        threads = []
        for i in range(num_new):
            t = threading.Thread(target=send_to_pi, args=(args.ports[i], payloads[i], i, args.baud))
            t.daemon = True
            t.start()
            threads.append(t)

        try:
            for t in threads:
                t.join()
        except KeyboardInterrupt:
            print("\nExiting...")
        return

    num_pis = len(args.ports)
    num_chunks = args.file_size // args.chunk_size

    assert args.file_size % args.chunk_size == 0, "file_size must be multiple of chunk_size"
    assert num_pis <= num_chunks, f"more Pis ({num_pis}) than chunks ({num_chunks})"

    file_data = generate_file(args.file_size)
    file_id = crc32(file_data)

    chunk_hashes = []
    for i in range(num_chunks):
        chunk = file_data[i * args.chunk_size:(i + 1) * args.chunk_size]
        chunk_hashes.append(crc32(chunk))

    print(f"Generated {args.file_size}-byte file, file_id={file_id:#010x}")
    print(f"{num_chunks} chunks of {args.chunk_size} bytes each")

    pi_chunks = partition_chunks(num_chunks, num_pis)

    for i, chunks in enumerate(pi_chunks):
        print(f"Pi {i} ({args.ports[i]}): chunks {sorted(chunks)}")

    save_state(file_data, args.chunk_size, chunk_hashes, file_id, pi_chunks)

    payloads = []
    for i in range(num_pis):
        payload = build_payload(file_data, args.chunk_size, chunk_hashes, file_id, pi_chunks[i])
        payloads.append(payload)

    threads = []
    for i in range(num_pis):
        t = threading.Thread(target=send_to_pi, args=(args.ports[i], payloads[i], i, args.baud))
        t.daemon = True
        t.start()
        threads.append(t)

    print(f"\nAll {num_pis} threads started. Press Ctrl-C to exit.")
    print(f"State saved to {STATE_FILE} -- use --append for late joiners.")

    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nExiting...")


if __name__ == '__main__':
    main()
