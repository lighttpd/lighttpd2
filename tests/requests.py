# -*- coding: utf-8 -*-

import pycurl
import StringIO

import sys

from base import *

class CurlRequestException(Exception):
	def __init__(self, value): self.value = value
	def __str__(self): return repr(self.value)

class CurlRequest(TestBase):
	URL = None
	SCHEME = "http"
	PORT = 0 # offset to Env.port
	AUTH = None

	EXPECT_RESPONSE_BODY = None
	EXPECT_RESPONSE_CODE = None
	EXPECT_RESPONSE_HEADERS = []

	def __init__(self):
		super(CurlRequest, self).__init__()
		self.resp_header_list = []
		self.resp_headers = { }
		self.resp_first_line = None

	def _recv_header(self, header):
		header = header.rstrip()
		if None == self.resp_first_line:
			self.resp_first_line = header
			return
		if header == "":
			return
		try:
			(key, value) = header.split(":", 1)
		except:
			print >> sys.stderr, "Couldn't parse header '%s'" % header
			raise
		key = key.strip()
		value = value.strip()
		self.resp_header_list.append((key, value))
		if self.resp_headers.has_key(key):
			self.resp_headers[key] += ", " + value
		else:
			self.resp_headers[key] = value

	def Run(self):
		if None == self.URL:
			raise BasicException("You have to set URL in your CurlRequest instance")
		c = pycurl.Curl()
		c.setopt(pycurl.URL, self.SCHEME + ("://127.0.0.1:%i" % (Env.port + self.PORT)) + self.URL)
		c.setopt(pycurl.HTTPHEADER, ["Host: " + self.vhost])
		b = StringIO.StringIO()
		c.setopt(pycurl.WRITEFUNCTION, b.write)
		c.setopt(pycurl.HEADERFUNCTION, self._recv_header)

		if None != self.AUTH:
			c.setopt(pycurl.USERPWD, self.AUTH)
			c.setopt(pycurl.FOLLOWLOCATION, 1)
			c.setopt(pycurl.MAXREDIRS, 5)

		self.curl = c
		self.buffer = b

		self.PrepareRequest()

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

	def PrepareRequest(self):
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
		print >> Env.log, self.buffer.getvalue()
		Env.log.flush()

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
			body = self.buffer.getvalue()
			if body != self.EXPECT_RESPONSE_BODY:
				raise CurlRequestException("Unexpected response body")

		for (k, v) in self.EXPECT_RESPONSE_HEADERS:
			if not self.resp_headers.has_key(k):
				raise CurlRequestException("Didn't get wanted response header '%s'" % (k))
			v1 = self.resp_headers[k]
			if v1 != v:
				raise CurlRequestException("Unexpected response header '%s' = '%s' (wanted '%s')" % (k, v1, v))

		return True

	def CheckResponse(self):
		return True
