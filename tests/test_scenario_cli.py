#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise scenario startup, validation and the no-scenario default."""
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
import json

host, scenario, case, data, profile = sys.argv[1:]
if case == "profile":
    scenario = str(pathlib.Path(scenario).with_name("valid.json"))
shutil.rmtree(data, ignore_errors=True)
pathlib.Path(data).mkdir(parents=True)
if case == 'oversize':
    scenario = str(pathlib.Path(data) / 'oversize.json')
    pathlib.Path(scenario).write_text('{"steps":[]}' + ' ' * (8 * 1024 * 1024))
command = [host, '--port', '0', '--data', data]
if case != 'default':
    command += ['--scenario', scenario]
if case == 'profile':
    command += ['--profile', profile]
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
lines = queue.Queue()
assert process.stdout is not None
def read_output():
    for line in process.stdout:
        lines.put(line)
reader = threading.Thread(target=read_output, daemon=True)
reader.start()
output = []
port = None

def receive(sock, size):
    result = b''
    while len(result) < size:
        part = sock.recv(size - len(result))
        assert part, 'WebSocket closed'
        result += part
    return result

def state(port):
    with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
        sock.settimeout(5)
        sock.sendall((f'GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
                      'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                      'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n').encode())
        response = b''
        while not response.endswith(b'\r\n\r\n'):
            response += receive(sock, 1)
            assert len(response) < 8192
        assert b' 101 ' in response, response
        payload = json.dumps({'id': 1, 'method': 'mock_game.state', 'params': {}}).encode()
        mask = b'\x31\x42\x53\x64'
        sock.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        while True:
            header = receive(sock, 2)
            length = header[1] & 127
            if length == 126:
                length = struct.unpack('!H', receive(sock, 2))[0]
            elif length == 127:
                length = struct.unpack('!Q', receive(sock, 8))[0]
            message = json.loads(receive(sock, length))
            if message.get('id') == 1:
                assert 'error' not in message, message
                return message['result']

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
    if case in ('valid', 'profile'):
        assert process.wait(timeout=5) == 0, ''.join(output)
        reader.join(timeout=1)
        while not lines.empty():
            output.append(lines.get_nowait())
        result = ''.join(output)
        for n in range(1, 5):
            assert f'SCENARIO step {n} ok' in result, result
        assert 'SCENARIO pass 4/4' in result, result
        if case == 'profile':
            selected = json.loads(pathlib.Path(profile).read_text())['plugins']
            assert all(plugin in result for plugin in selected), result
    elif case == 'default':
        assert port is not None, 'host did not reach listening state'
        scene = state(port)
        if case == 'default':
            assert scene['player']['spawned'] is True
            assert scene['player']['zone'] == 4
            assert scene['player']['pos'] == [0, 0, 0]
            assert not any(e['class'] == 'Scenario_Test_Entity' for e in scene['entities'])
        else:
            matching = [e for e in scene['entities'] if e['class'] == 'Scenario_Test_Entity']
            assert len(matching) == 1 and matching[0]['zone'] == 4 and matching[0]['pos'] == [4, 5, 6], matching
            removed = [e for e in scene['entities'] if e['class'] == 'Scenario_Removed_Entity']
            assert not removed or removed[0]['removing'], removed
    else:
        assert port is None, 'invalid scenario reached listening state'
        assert process.poll() is not None and process.returncode != 0, 'invalid scenario did not fail'
        diagnostic = ''.join(output).lower()
        assert not any(line.startswith('usage:') for line in output), diagnostic
        if case == 'oversize':
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
