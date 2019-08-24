#!/usr/bin/env python
# -*- coding: utf-8 -*-

import asyncore
import socket
import time
import random
import traceback

class MemcacheEntry:
	def __init__(self, flags, exptime, data, cas):
		self.flags = flags
		self.data = data
		self.cas = cas
		self.setExptime(exptime)

	def _ttl(self, str):
		v = int(str)
		if v < 0: raise ValueError('exptime not an unsigned integer')
		if v > 0 and v < 365*24*3600: v += time.time()
		return v

	def setExptime(self, exptime):
		exptime = self._ttl(exptime)
		if exptime > 0:
			self.expire = exptime
		else:
			self.expire = None

	def flush(self, exptime):
		exptime = self._ttl(exptime)
		if self.expire == None or self.expire > expire:
			self.expire = expire

	def expired(self):
		if self.expire != None: return self.expire < time.time()
		return False

class MemcacheDB:
	def __init__(self):
		self.d = dict()
		self.cas = random.randint(0, 2**64-1)

	def _uint64value(self, str):
		v = int(str)
		if v < 0 or v >= 2**64: raise ValueError('not an unsigned 64-bit integer')
		return v

	def _flags(self, str):
		v = int(str)
		if v < 0 or v >= 2**32: raise ValueError('flags not an unsigned 32-bit integer')
		return v

	def _next_cas(self):
		cas = self.cas
		self.cas = (self.cas + 1) % 2**64
		return cas

	def get(self, key):
		if not key in self.d: return None
		entry = self.d[key]
		if entry.expired():
			self.d.pop(key)
			return None
		return entry

	def set(self, key, flags, exptime, data):
		self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
		return "STORED"

	def add(self, key, flags, exptime, data):
		if None != self.get(key): return "NOT_STORED"
		self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
		return "STORED"

	def replace(self, key, flags, exptime, data):
		if None == self.get(key): return "NOT_STORED"
		self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
		return "STORED"

	def append(self, key, data):
		entry = self.get(key)
		if None == entry: return "NOT_FOUND"
		entry.data += data
		entry.cas = _next_cas()
		return "STORED"

	def prepend(self, key, data):
		entry = self.get(key)
		if None == entry: return "NOT_FOUND"
		entry.data = data + entry.data
		entry.cas = _next_cas()
		return "STORED"

	def cas(self, key, flags, exptime, cas, data):
		entry = self.get(key)
		if None == entry: return "NOT_FOUND"
		if entry.cas != cas: return "EXISTS"
		self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
		return "STORED"

	def delete(self, key):
		entry = self.get(key)
		if None == entry: return "NOT_FOUND"
		self.d.pop(key)
		return "DELETED"

	def incr(self, key, value):
		try:
			value = _uint64value(value)
		except ValueError as e:
			return "CLIENT_ERROR " + str(e)
		entry = self.get(key)
		if None == entry: return "NOT_FOUND"
		try:
			v = _uint64value(entry.data)
			v = (v + value) % 2**64
			entry.data = str(v)
			entry.cas = _next_cas()
		except ValueError as e:
			return "SERVER_ERROR " + str(e)
		return entry.data

	def decr(self, key, value):
		try:
			value = _uint64value(value)
		except ValueError as e:
			return "CLIENT_ERROR " + str(e)
		entry = self.get(key)
		if None == entry: return "NOT_FOUND"
		try:
			v = _uint64value(entry.data)
			v = v - value
			if v < 0: v = 0
			entry.data = str(v)
			entry.cas = _next_cas()
		except ValueError as e:
			return "SERVER_ERROR " + str(e)
		return entry.data

	def touch(self, key, exptime):
		entry = self.get(key)
		if None == entry: return "NOT_FOUND"
		entry.setExptime(exptime)
		return "TOUCHED"

	def stats(self):
		return []

	def flush_all(self, exptime = None):
		if exptime == None:
			self.d = dict()
		else:
			for key in self.d.keys:
				entry = self.get(key)
				if entry != None: entry.flush(exptime)
		return "OK"

	def version(self):
		return "VERSION python memcached stub 0.1"

	def verbosity(self, level):
		return "OK"

class MemcachedHandler(asyncore.dispatcher_with_send):
	def __init__(self, sock, db):
		asyncore.dispatcher_with_send.__init__(self, sock)
		self.db = db
		self.data = ''
		self.want_binary = None

	def _server_error(self, msg):
		self.send('SERVER_ERROR ' + msg + '\r\n')
		self.data = ''
		self.close()

	def _client_error(self, msg):
		self.send('CLIENT_ERROR ' + msg + '\r\n')
		self.data = ''
		self.close()

	def _error(self, msg):
		self.send('ERROR\r\n')
		self.data = ''
		self.close()

	def _handle_binary(self, b):
		args = self.args + [b]
		cmd = self.cmd
		noreply = self.noreply
		self.cmd = self.args = self.noreply = None
		r = getattr(self.db, cmd).__call__(*args)
		if not noreply: self.send('%s\r\n' % r)

	def _handle_line(self, line):
		args = line.split()
		if len(args) == 0: return _client_error("empty command")
		cmd = args[0]
		args = args[1:]
		noreply = False
		if args[-1] == "noreply":
			args.pop()
			noreply = True
		if cmd in ['set', 'add', 'replace', 'append', 'prepend']:
			if len(args) != 4: return _client_error("wrong number %i of arguments for command" % 4)
			self.want_binary = int(args[3])
			if self.want_binary < 0: return _client_error("negative bytes length")
			self.args = args[:3]
			self.cmd = cmd
			self.noreply = noreply
		elif cmd == 'cas':
			if len(args) != 5: return _client_error("wrong number %i of arguments for command" % 5)
			self.want_binary = args[3]
			args = args[:3] + args[4:]
			self.cmd = cmd
			self.noreply = noreply
		elif cmd == 'get':
			for key in args:
				entry = self.db.get(key)
				if entry != None:
					self.send('VALUE %s %s %s\r\n%s\r\n' % (key, entry.flags, len(entry.data), entry.data))
			self.send('END\r\n')
		elif cmd == 'gets':
			for key in args:
				entry = self.db.get(key)
				if entry != None:
					self.send('VALUE %s %s %s %s\r\n%s\r\n' % (key, entry.flags, len(entry.data), entry.cas, entry.data))
			self.send('END\r\n')
		elif cmd == 'stats':
			for (name, value) in self.db.stats():
				self.send('STAT ' + name + ' ' + value + '\r\n')
			self.send('END\r\n')
		elif cmd in ['delete', 'incr', 'decr', 'touch', 'flush_all', 'version', 'verbosity']:
			r = getattr(self.db, cmd).__call__(*args)
			if not noreply: self.send(r + '\r\n')
		else:
			return self._error()

	def _handle_data(self):
		while len(self.data) > 0:
			if self.want_binary != None:
				if len(self.data) >= self.want_binary + 2:
					b = self.data[:self.want_binary]
					if self.data[self.want_binary:self.want_binary+2] != '\r\n':
						return self._parse_error("wrong termination of binary data")
					self.data = self.data[self.want_binary+2:]
					self._handle_binary(b)
				else:
					return # wait for more data
			else:
				pos = self.data.find('\r\n')
				if pos < 0:
					if len(self.data) > 512:
						return self._parse_error("command too long")
					return # wait for more data
				l = self.data[:pos]
				self.data = self.data[pos+2:]
				self._handle_line(l)

	def handle_read(self):
		self.data += self.recv(8192)
		try:
			self._handle_data()
		except TypeError as e:
			self._client_error("wrong number of arguments for command: %s" % e)
			print traceback.format_exc()


class MemcachedServer(asyncore.dispatcher):

	def __init__(self, sock):
		asyncore.dispatcher.__init__(self)
		sock.setblocking(0)
		self.set_socket(sock)
		self.accepting = True
		self.db = MemcacheDB()

	def handle_accept(self):
		pair = self.accept()
		if pair is not None:
			sock, addr = pair
			print 'Memcached: Incoming connection'
			handler = MemcachedHandler(sock, self.db)

server = MemcachedServer(socket.fromfd(0, socket.AF_UNIX, socket.SOCK_STREAM))
try:
	asyncore.loop()
except KeyboardInterrupt:
	pass
