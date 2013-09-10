# -*- coding: utf-8 -*-

import pycurl
import StringIO

import sys
import zlib
import bz2
import os

from base import *

TEST_TXT="""Hi!
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
"""

class CurlRequestException(Exception):
	def __init__(self, value): self.value = value
	def __str__(self): return repr(self.value)

class CurlRequest(TestBase):
	URL = None
	SCHEME = "http"
	PORT = 0 # offset to Env.port
	AUTH = None
	POST = None
	REQUEST_HEADERS = []
	ACCEPT_ENCODING = "deflate, gzip"

	EXPECT_RESPONSE_BODY = None
	EXPECT_RESPONSE_CODE = None
	EXPECT_RESPONSE_HEADERS = []

	def __init__(self, parent):
		super(CurlRequest, self).__init__(parent)
		self.resp_header_list = []
		self.resp_headers = { }
		self.resp_first_line = None
		self.resp_body = None

	def _recv_header(self, header):
		header = header.rstrip()
		if None == self.resp_first_line:
			self.resp_first_line = header
			return
		if header == "":
			if None != self.resp_first_line and self.resp_first_line.split(" ", 2)[1] == '100':
				if Env.debugRequests:
					print >> Env.log, "Handling 100 Continue: '%s'" % self.resp_first_line
				self.resp_first_line = None
			return
		try:
			(key, value) = header.split(":", 1)
		except:
			print >> sys.stderr, "Couldn't parse header '%s'" % header
			raise
		key = key.strip()
		value = value.strip()
		self.resp_header_list.append((key, value))
		key = key.lower()
		if self.resp_headers.has_key(key):
			self.resp_headers[key] += ", " + value
		else:
			self.resp_headers[key] = value

	def Run(self):
		if None == self.URL:
			raise BasicException("You have to set URL in your CurlRequest instance")
		reqheaders = ["Host: " + self.vhost] + self.REQUEST_HEADERS
		if None != self.ACCEPT_ENCODING:
			reqheaders += ["Accept-Encoding: " + self.ACCEPT_ENCODING]
		c = pycurl.Curl()
		c.setopt(pycurl.CAINFO, os.path.join(Env.sourcedir, "tests", "ca", "ca.crt"))
		if hasattr(pycurl, 'RESOLVE'):
			c.setopt(pycurl.URL, self.SCHEME + ("://%s:%i" % (self.vhost or '127.0.0.2', Env.port + self.PORT)) + self.URL)
			c.setopt(pycurl.RESOLVE, ['%s:%i:127.0.0.2' % (self.vhost, Env.port + self.PORT)])
		else:
			c.setopt(pycurl.URL, self.SCHEME + ("://127.0.0.2:%i" % (Env.port + self.PORT)) + self.URL)
			c.setopt(pycurl.SSL_VERIFYHOST, 0)
		c.setopt(pycurl.HTTPHEADER, reqheaders)
		c.setopt(pycurl.NOSIGNAL, 1)
		c.setopt(pycurl.TIMEOUT, 2)
		b = StringIO.StringIO()
		c.setopt(pycurl.WRITEFUNCTION, b.write)
		c.setopt(pycurl.HEADERFUNCTION, self._recv_header)
		if None != self.POST: c.setopt(pycurl.POSTFIELDS, self.POST)

		if None != self.AUTH:
			c.setopt(pycurl.USERPWD, self.AUTH)
			c.setopt(pycurl.FOLLOWLOCATION, 1)
			c.setopt(pycurl.MAXREDIRS, 5)

		self.curl = c
		self.buffer = b

		self.PrepareRequest(reqheaders)

		c.perform()

		try:
			if not self._checkResponse():
				raise CurlRequestException("Response check failed")
		except:
			if not Env.debugRequests:
				self.dump()
			raise
		finally:
			c.close()
			self.curl = None

		return True

	def PrepareRequest(self, reqheaders):
		pass

	def dump(self):
		c = self.curl
		Env.log.flush()
		print >> Env.log, "Dumping request for test '%s'" % self.name
		print >> Env.log, "Curl request: URL = %s://%s:%i%s" % (self.SCHEME, self.vhost, Env.port + self.PORT, self.URL)
		print >> Env.log, "Curl response code: %i " % (c.getinfo(pycurl.RESPONSE_CODE))
		print >> Env.log, "Curl response headers:"
		for (k, v) in self.resp_header_list:
			print >> Env.log, "  %s: %s" % (k, v)
		print >> Env.log, "Curl response body:"
		print >> Env.log, self.ResponseBody()
		Env.log.flush()

	def _decode(self, method, data):
		if 'x-gzip' == method or 'gzip' == method:
			header = data[:10]
			if "\x1f\x8b\x08\x00\x00\x00\x00\x00" != header[:8]:
				raise CurlRequestException("Unsupported content-encoding gzip header")
			return zlib.decompress(data[10:], -15)
		elif 'deflate' == method:
			return zlib.decompress(data, -15)
		elif 'compress' == method:
			raise CurlRequestException("Unsupported content-encoding %s" % method)
		elif 'x-bzip2' == method or 'bzip2' == method:
			return bz2.decompress(data)
		else:
			raise CurlRequestException("Unsupported content-encoding %s" % method)

	def ResponseBody(self):
		if None == self.resp_body:
			body = self.buffer.getvalue()
			if self.resp_headers.has_key("content-encoding"):
				cenc = self.resp_headers["content-encoding"]
				body = self._decode(cenc, body)
			self.resp_body = body
		return self.resp_body

	def _checkResponse(self):
		c = self.curl
		if Env.debugRequests:
			self.dump()

		if not self.CheckResponse():
			return False

		if None != self.EXPECT_RESPONSE_CODE:
			code = c.getinfo(pycurl.RESPONSE_CODE)
			if code != self.EXPECT_RESPONSE_CODE:
				raise CurlRequestException("Unexpected response code %i (wanted %i)" % (code, self.EXPECT_RESPONSE_CODE))

		if None != self.EXPECT_RESPONSE_BODY:
			if self.ResponseBody() != self.EXPECT_RESPONSE_BODY:
				raise CurlRequestException("Unexpected response body")

		for (k, v) in self.EXPECT_RESPONSE_HEADERS:
			if v == None:
				if self.resp_headers.has_key(k.lower()):
					raise CurlRequestException("Got unwanted response header '%s' = '%s'" % (k, self.resp_headers[k.lower()]))
			else:
				if not self.resp_headers.has_key(k.lower()):
					raise CurlRequestException("Didn't get wanted response header '%s'" % (k))
				v1 = self.resp_headers[k.lower()]
				if v1 != v:
					raise CurlRequestException("Unexpected response header '%s' = '%s' (wanted '%s')" % (k, v1, v))

		return True

	def CheckResponse(self):
		return True
