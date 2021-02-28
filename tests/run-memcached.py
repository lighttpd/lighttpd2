#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import asyncio
import socket
import time
import random
import traceback
import typing


class MemcacheEntry:
	def __init__(self, flags: bytes, exptime: bytes, data: bytes, cas: bytes):
		self.flags: int = self._flags(flags)
		self.data: bytes = data
		self.cas: bytes = cas
		self.expire: typing.Optional[float] = self._ttl(exptime)

	@staticmethod
	def _flags(flags: bytes) -> int:
		v = int(flags)
		if v < 0 or v >= 2**32: raise ValueError('flags not an unsigned 32-bit integer')
		return v

	@staticmethod
	def _ttl(value: bytes) -> typing.Optional[float]:
		v = int(value)
		if v < 0: raise ValueError('exptime not an unsigned integer')
		if v > 0 and v < 365*24*3600:
			return v + time.time()
		else:
			return None

	def setExptime(self, exptime: bytes):
		self.expire = self._ttl(exptime)

	def flush(self, exptime: float):
		# make sure entry expires at `exptime` (or before)
		if self.expire is None or self.expire > exptime:
			self.expire = exptime

	def expired(self):
		return self.expire != None and self.expire < time.time()


class MemcacheDB:
	def __init__(self):
		self.d = dict()
		self._cas = random.randint(0, 2**64-1)

	@staticmethod
	def _uint64value(str):
		v = int(str)
		if v < 0 or v >= 2**64: raise ValueError('not an unsigned 64-bit integer')
		return v

	def _next_cas(self) -> bytes:
		cas = self._cas
		self._cas = (cas + 1) % 2**64
		return b'%d' % cas

	def get(self, key: bytes):
		if not key in self.d: return None
		entry = self.d[key]
		if entry.expired():
			self.d.pop(key)
			return None
		return entry

	def set(self, key: bytes, flags: bytes, exptime: bytes, data: bytes):
		self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
		return b"STORED"

	def add(self, key: bytes, flags: bytes, exptime: bytes, data: bytes):
		if None != self.get(key): return b"NOT_STORED"
		self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
		return b"STORED"

	def replace(self, key: bytes, flags: bytes, exptime: bytes, data: bytes):
		if None == self.get(key): return b"NOT_STORED"
		self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
		return b"STORED"

	def append(self, key: bytes, data: bytes):
		entry = self.get(key)
		if None == entry: return b"NOT_FOUND"
		entry.data += data
		entry.cas = self._next_cas()
		return b"STORED"

	def prepend(self, key: bytes, data: bytes):
		entry = self.get(key)
		if None == entry: return b"NOT_FOUND"
		entry.data = data + entry.data
		entry.cas = self._next_cas()
		return b"STORED"

	def cas(self, key: bytes, flags: bytes, exptime: bytes, cas: bytes, data: bytes):
		entry = self.get(key)
		if None == entry: return b"NOT_FOUND"
		if entry.cas != cas: return b"EXISTS"
		self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
		return b"STORED"

	def delete(self, key: bytes):
		entry = self.get(key)
		if None == entry: return b"NOT_FOUND"
		self.d.pop(key)
		return b"DELETED"

	def incr(self, key: bytes, value: bytes):
		try:
			value = self._uint64value(value)
		except ValueError as e:
			return b"CLIENT_ERROR %s" % str(e).encode('utf-8')
		entry = self.get(key)
		if None == entry: return b"NOT_FOUND"
		try:
			v = self._uint64value(entry.data)
			v = (v + value) % 2**64
			entry.data = str(v)
			entry.cas = self._next_cas()
		except ValueError as e:
			return b"SERVER_ERROR %s" % str(e).encode('utf-8')
		return entry.data

	def decr(self, key: bytes, value: bytes):
		try:
			value = self._uint64value(value)
		except ValueError as e:
			return b"CLIENT_ERROR %s" % str(e).encode('utf-8')
		entry = self.get(key)
		if None == entry: return b"NOT_FOUND"
		try:
			v = self._uint64value(entry.data)
			v = v - value
			if v < 0: v = 0
			entry.data = str(v)
			entry.cas = self._next_cas()
		except ValueError as e:
			return b"SERVER_ERROR %s" % str(e).encode('utf-8')
		return entry.data

	def touch(self, key: bytes, exptime: bytes):
		entry = self.get(key)
		if None == entry: return b"NOT_FOUND"
		entry.setExptime(exptime)
		return b"TOUCHED"

	def stats(self):
		return []

	def flush_all(self, exptime: typing.Optional[bytes] = None):
		if exptime is None:
			self.d = dict()
		else:
			expire_at = MemcacheEntry._ttl(exptime) or time.time()
			for key in self.d.keys():
				entry = self.get(key)
				if entry != None: entry.flush(expire_at)
		return b"OK"

	def version(self):
		return b"VERSION python memcached stub 0.1"

	def verbosity(self, level: bytes):
		return b"OK"


class MemcachedHandler:
	def __init__(self, *, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, db: MemcacheDB):
		self.reader = reader
		self.writer = writer
		self.db = db
		self.data = b''
		self.want_binary = None
		self.closed = False

	def _server_error(self, msg: str):
		self.writer.write(b'SERVER_ERROR %s\r\n' % msg.encode('utf-8'))
		self.data = b''
		self.closed = True

	def _client_error(self, msg: str):
		self.writer.write(b'CLIENT_ERROR %s\r\n' % msg.encode('utf-8'))
		self.data = b''
		self.closed = True

	def _error(self):
		self.writer.write(b'ERROR\r\n')
		self.data = b''
		self.closed = True

	def _handle_binary(self, b):
		args = self.args
		cmd = self.cmd
		noreply = self.noreply
		self.cmd = self.args = self.noreply = None
		r = (getattr(self.db, cmd))(*args, b)
		if not noreply: self.writer.write(b'%s\r\n' % r)

	def _handle_line(self, line):
		args = line.split()
		if len(args) == 0: return self._client_error("empty command")
		cmd = args[0].decode('ascii')
		args = args[1:]
		noreply = False
		if args[-1] == b"noreply":
			args.pop()
			noreply = True
		if cmd in ['set', 'add', 'replace', 'append', 'prepend']:
			if len(args) != 4: return self._client_error("wrong number %i of arguments for command" % 4)
			self.want_binary = int(args[3])
			if self.want_binary < 0: return self._client_error("negative bytes length")
			self.args = args[:3]
			self.cmd = cmd
			self.noreply = noreply
		elif cmd == 'cas':
			if len(args) != 5: return self._client_error("wrong number %i of arguments for command" % 5)
			self.want_binary = args[3]
			args = args[:3] + args[4:]
			self.cmd = cmd
			self.noreply = noreply
		elif cmd == 'get':
			for key in args:
				entry = self.db.get(key)
				if entry != None:
					self.writer.write(b'VALUE %s %d %d\r\n%s\r\n' % (key, entry.flags, len(entry.data), entry.data))
			self.writer.write(b'END\r\n')
		elif cmd == 'gets':
			for key in args:
				entry = self.db.get(key)
				if entry != None:
					self.writer.write(b'VALUE %s %d %d %d\r\n%s\r\n' % (key, entry.flags, len(entry.data), entry.cas, entry.data))
			self.writer.write(b'END\r\n')
		elif cmd == 'stats':
			for (name, value) in self.db.stats():
				self.writer.write(b'STAT %s %s\r\n' % (name, value))
			self.writer.write(b'END\r\n')
		elif cmd in ['delete', 'incr', 'decr', 'touch', 'flush_all', 'version', 'verbosity']:
			r = (getattr(self.db, cmd))(*args)
			if not noreply: self.writer.write(r + b'\r\n')
		else:
			return self._error()

	def _handle_data(self):
		while len(self.data) > 0:
			if self.want_binary != None:
				if len(self.data) >= self.want_binary + 2:
					b = self.data[:self.want_binary]
					if self.data[self.want_binary:self.want_binary+2] != b'\r\n':
						return self._client_error("wrong termination of binary data")
					self.data = self.data[self.want_binary+2:]
					self._handle_binary(b)
				else:
					return # wait for more data
			else:
				pos = self.data.find(b'\r\n')
				if pos < 0:
					if len(self.data) > 512:
						return self._client_error("command too long")
					return # wait for more data
				l = self.data[:pos]
				self.data = self.data[pos+2:]
				self._handle_line(l)

	async def handle(self):
		while not self.closed:
			await self.writer.drain()
			next_buf = await self.reader.read(8192)
			if len(next_buf) == 0:
				# received EOF, close immediately.
				self.writer.close()
				return
			self.data += next_buf
			try:
				self._handle_data()
			except TypeError as e:
				self._client_error("wrong number of arguments for command: %s" % e)
				print(traceback.format_exc())
		# close
		await self.writer.drain()
		self.writer.close()
		await self.writer.wait_closed()


async def main():
	sock = socket.socket(fileno=0)
	if sock.type == socket.AF_UNIX:
		start_server = asyncio.start_unix_server
	else:
		start_server = asyncio.start_server
	db = MemcacheDB()

	async def handle_memcache_client(reader, writer):
		print(f"Memcached: Incoming connection", flush=True)
		await MemcachedHandler(reader=reader, writer=writer, db=db).handle()

	server = await start_server(handle_memcache_client, sock=sock, start_serving=False)

	addr = server.sockets[0].getsockname()
	print(f'Serving on {addr}', flush=True)

	async with server:
		await server.serve_forever()


try:
	asyncio.run(main())
except KeyboardInterrupt:
	pass
