#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import asyncio
import dataclasses
import socket
import traceback
import typing


@dataclasses.dataclass
class ScgiRequest:
	headers: typing.Dict[bytes, bytes]
	body: bytes


async def parse_scgi_request(reader: asyncio.StreamReader) -> ScgiRequest:
	hlen = int((await reader.readuntil(b':'))[:-1])
	header_raw = await reader.readexactly(hlen + 1)
	assert len(header_raw) >= 16, "invalid request: too short (< 16)"
	assert header_raw[-2:] == b'\0,', f"Invalid request: missing header/netstring terminator '\\x00,', got {header_raw[-2:]!r}"
	header_list = header_raw[:-2].split(b'\0')
	assert len(header_list) % 2 == 0, f"Invalid request: odd numbers of header entries (must be pairs), got {len(header_list)}"
	assert header_list[0] == b'CONTENT_LENGTH', f"Invalid request: first header entry must be 'CONTENT_LENGTH', got {header_list[0]!r}"
	clen = int(header_list[1])
	headers = {}
	i = 0
	while i < len(header_list):
		key = header_list[i]
		value = header_list[i+1]
		i += 2
		assert not key in headers, f"Invalid request: duplicate header key {key!r}"
		headers[key] = value
	assert headers.get(b'SCGI') == b'1', "Invalid request: missing SCGI=1 header"
	body = await reader.readexactly(clen)
	return ScgiRequest(headers=headers, body=body)


async def handle_scgi(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
	print(f"scgi-envcheck: Incoming connection", flush=True)
	try:
		req = await parse_scgi_request(reader)
		envvar = req.headers[b'QUERY_STRING']
		result = req.headers[envvar]
	except KeyboardInterrupt:
		raise
	except Exception as e:
		print(traceback.format_exc())
		writer.write(b"Status: 500\r\nContent-Type: text/plain\r\n\r\n" + str(e).encode('utf-8'))
	else:
		writer.write(b"Status: 200\r\nContent-Type: text/plain\r\n\r\n" + result)
	await writer.drain()
	writer.close()
	await writer.wait_closed()


async def main():
	sock = socket.socket(fileno=0)
	if sock.type == socket.AF_UNIX:
		start_server = asyncio.start_unix_server
	else:
		start_server = asyncio.start_server

	server = await start_server(handle_scgi, sock=sock, start_serving=False)

	addr = server.sockets[0].getsockname()
	print(f'Serving on {addr}', flush=True)

	async with server:
		await server.serve_forever()


try:
	asyncio.run(main())
except KeyboardInterrupt:
	pass
