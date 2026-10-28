#!/usr/bin/env python3
"""Exercise profile loading and observable service state through hostd."""
import json
import pathlib
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

host, profile, case, data = sys.argv[1:]
shutil.rmtree(data, ignore_errors=True)
pathlib.Path(data).mkdir(parents=True)
if case in ("oversize", "depth"):
    profile = str(pathlib.Path(data) / "generated-profile.json")
    if case == "oversize":
        # Valid schema plus whitespace; the rejection must be a file-size limit.
        pathlib.Path(profile).write_text('{"plugins":[],"mocks":[],"services":[]}' + ' ' * (8 * 1024 * 1024))
    else:
        # Nest inside a known field so unknown-field validation cannot hide the depth check.
        pathlib.Path(profile).write_text('{"plugins":[],"mocks":[],"services":' + '[' * 100 + '0' + ']' * 100 + '}')

command = [host, "--port", "0", "--data", data]
if case != "default":
    command += ["--profile", profile]
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
assert process.stdout is not None
lines = queue.Queue()

def read_output():
    for line in process.stdout:
        lines.put(line)

reader = threading.Thread(target=read_output, daemon=True)
reader.start()
output = []
port = None

def receive_exact(sock, size):
    result = b''
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise AssertionError('WebSocket closed before response')
        result += chunk
    return result

def rpc(sock, method):
    payload = json.dumps({'id': 1, 'method': method, 'params': {}}).encode()
    mask = b'\x31\x42\x53\x64'
    assert len(payload) < 126
    sock.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
    while True:
        header = receive_exact(sock, 2)
        length = header[1] & 127
        if length == 126:
            length = struct.unpack('!H', receive_exact(sock, 2))[0]
        elif length == 127:
            length = struct.unpack('!Q', receive_exact(sock, 8))[0]
        message = json.loads(receive_exact(sock, length))
        if message.get('id') == 1:
            assert 'error' not in message, message
            return message['result']

def services(port):
    with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
        sock.settimeout(5)
        sock.sendall((f'GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
                      'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                      'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n').encode())
        response = b''
        while not response.endswith(b'\r\n\r\n'):
            response += receive_exact(sock, 1)
            assert len(response) < 8192
        assert b' 101 ' in response, response
        return {item['name']: item['published'] for item in rpc(sock, 'mock_game.services.list')}

try:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=0.1)
            output.append(line)
            match = re.search(r'scuba-hostd: listening port=(\d+)', line)
            if match:
                port = int(match.group(1))
                break
        except queue.Empty:
            if process.poll() is not None:
                reader.join(timeout=1)
                while not lines.empty():
                    output.append(lines.get_nowait())
                break
    if case.startswith('valid_') or case == 'default':
        assert port is not None, 'valid profile did not reach listening state'
        if case in ('valid_service', 'default'):
            published = services(port)
            assert published.get('game.actors') is True, f'game.actors not published: {published}'
            if case == 'default':
                assert all(published.values()) and len(published) > 1, f'default services changed: {published}'
    else:
        assert port is None, 'invalid profile reached listening state'
        assert process.poll() is not None, 'invalid profile did not exit'
        assert process.returncode != 0, 'invalid profile exited successfully'
        assert not any(line.startswith('usage:') for line in output), 'profile option rejected before validation'
        diagnostic = ''.join(output).lower()
        if case == 'depth':
            assert 'depth' in diagnostic or 'deep' in diagnostic or 'nest' in diagnostic, diagnostic
        elif case == 'oversize':
            assert any(word in diagnostic for word in ('size', 'large', 'limit', 'bytes')), diagnostic
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    reader.join(timeout=1)
    while not lines.empty():
        output.append(lines.get_nowait())
    print(''.join(output), end='')
