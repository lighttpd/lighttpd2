#!/usr/bin/env python
# -*- coding: utf-8 -*-

import socket
import traceback

servsocket = socket.fromfd(0, socket.AF_UNIX, socket.SOCK_STREAM)

def parsereq(data):
	a = data.split(':', 1)
	if len(a) != 2: return False
	hlen, rem = a
	hlen = int(hlen)
	if hlen < 16: raise Exception("invalid request")
	if len(rem) < hlen + 1: return False
	if rem[hlen] != ',' or rem[hlen-1] != '\0': raise Exception("invalid request")
	header = rem[:hlen-1]
	body = rem[hlen+1:]
	header = header.split('\0')
	if len(header) < 4: raise Exception("invalid request: not enough header entries")
	if header[0] != "CONTENT_LENGTH": raise Exception("invalid request: missing CONTENT_LENGTH")
	clen = int(header[1])
	if len(body) < clen: return False
	env = dict()
	while len(header) > 0:
		if len(header) == 1: raise Exception("invalid request: missing value for key")
		key, value = header[0:2]
		header = header[2:]
		if '' == key: raise Exception("invalid request: empty key")
		if env.has_key(key): raise Exception("invalid request: duplicate key")
		env[key] = value
	if not env.has_key('SCGI') or env['SCGI'] != '1':
		raise Exception("invalid request: missing/broken SCGI=1 header")
	return {'env': env, 'body': body}

try:
	while 1:
		conn, addr = servsocket.accept()
		result_status = 200
		result = ''
		try:
			print 'Accepted connection'
			data = ''
			header = False
			while not header:
				newdata = conn.recv(1024)
				if len(newdata) == 0: raise Exception("invalid request: unexpected EOF")
				data += newdata
				header = parsereq(data)
			envvar = header['env']['QUERY_STRING']
			result = header['env'][envvar]
		except KeyboardInterrupt:
			raise
		except Exception as e:
			print traceback.format_exc()
			result_status = 500
			result = str(e)
		try:
			conn.sendall("Status: " + str(result_status) + "\r\nContent-Type: text/plain\r\n\r\n")
			conn.sendall(result)
			conn.close()
		except:
			print traceback.format_exc()
except KeyboardInterrupt:
	pass
