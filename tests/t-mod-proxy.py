# -*- coding: utf-8 -*-

from base import *
from requests import *
import socketserver
import threading

class HttpBackendHandler(socketserver.StreamRequestHandler):
	def handle(self):
		keepalive = True
		while True:
			reqline = self.rfile.readline().decode('utf-8').rstrip()
			# eprint("Request line: " + repr(reqline))
			reqline = reqline.split(' ', 3)
			if len(reqline) != 3 or reqline[0].upper() != 'GET':
				self.wfile.write(b"HTTP/1.0 400 Bad request\r\n\r\n")
				return
			keepalive_default = True
			if reqline[2].upper() != "HTTP/1.1":
				keepalive = False
				keepalive_default = False
			if reqline[1].startswith("/keepalive"):
				# simulate broken backend (HTTP/1.0 incompatible)
				keepalive = True
				keepalive_default = True
			# read headers; and GET has no body
			while True:
				hdr = self.rfile.readline().decode('utf-8').rstrip()
				if hdr == "": break
				hdr = hdr.split(':', 2)
				if hdr[0].lower() == "connection":
					keepalive = (hdr[1].strip().lower() == "keep-alive")
			if reqline[1].startswith("/upgrade/custom"):
				self.wfile.write(b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: custom\r\n\r\nHello World!")
				return
			if reqline[1].startswith("/chunked/delay"):
				import time
				self.wfile.write(b"HTTP/1.1 200 Ok\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nHi")
				time.sleep(0.1)
				self.wfile.write(b"!\n")
				time.sleep(0.1)
				self.wfile.write(b"\r\n0\r\n\r\n")
				continue
			if reqline[1].startswith("/nolength"):
				self.wfile.write(b"HTTP/1.1 200 OK\r\n\r\nHello world")
				self.finish()
				return

			# send response
			resp_body = reqline[1].encode('utf-8')
			clen = "Content-Length: {}\r\n".format(len(resp_body)).encode('utf-8')
			ka = b""
			if keepalive != keepalive_default:
				if keepalive:
					ka = b"Connection: keep-alive\r\n"
				else:
					ka = b"Connection: close\r\n"
			resp = b"HTTP/1.1 200 OK\r\n" + ka + clen + b"\r\n" + resp_body
			# eprint("Backend response: " + repr(resp_body))
			self.wfile.write(resp)
			if not keepalive:
				return


class HttpBackend(socketserver.ThreadingMixIn, socketserver.TCPServer):
	allow_reuse_address = True
	def __init__(self):
		self.port = Env.port + 3
		super().__init__(('127.0.0.2', self.port), HttpBackendHandler)

		self.listen_thread = threading.Thread(target = self.serve_forever, name = "HttpBackend-{}".format(self.port))
		self.listen_thread.daemon = True
		self.listen_thread.start()

class TestSimple(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
	config = """
req_header.overwrite "Host" => "basic-gets";
self_proxy;
"""
	no_docroot = True

# backend gets encoded %2F
class TestProxiedRewrittenEncodedURL(CurlRequest):
	URL = "/foo%2Ffile?abc"
	EXPECT_RESPONSE_BODY = "/dest%2Ffile?abc"
	EXPECT_RESPONSE_CODE = 200
	no_docroot = True
	config = """
rewrite_raw "/foo(.*)" => "/dest$1";
backend_proxy;
"""

# backend gets decoded %2F
class TestProxiedRewrittenDecodedURL(CurlRequest):
	URL = "/foo%2Ffile?abc"
	EXPECT_RESPONSE_BODY = "/dest/file?abc"
	EXPECT_RESPONSE_CODE = 200
	no_docroot = True
	config = """
rewrite "/foo(.*)" => "/dest$1";
backend_proxy;
"""

# fake a backend forcing keep-alive mode
class TestBackendForcedKeepalive(CurlRequest):
	URL = "/keepalive"
	EXPECT_RESPONSE_BODY = "/keepalive"
	EXPECT_RESPONSE_CODE = 200
	no_docroot = True
	config = """
backend_proxy;
"""

# have backend "Upgrade"
class TestBackendUpgrade(CurlRequest):
	URL = "/upgrade/custom"
	EXPECT_RESPONSE_BODY = "Hello World!"
	EXPECT_RESPONSE_CODE = 101
	no_docroot = True
	config = """
backend_proxy;
"""

class TestBackendDelayedChunk(CurlRequest):
	URL = "/chunked/delay"
	EXPECT_RESPONSE_BODY = "Hi!\n"
	EXPECT_RESPONSE_CODE = 200
	no_docroot = True
	config = """
backend_proxy;
"""

class TestBackendNoLength(CurlRequest):
	URL = "/nolength"
	EXPECT_RESPONSE_BODY = "Hello world"
	EXPECT_RESPONSE_CODE = 200
	no_docroot = True
	config = """
backend_proxy;
"""

class Test(GroupTest):
	group = [
		TestSimple,
		TestProxiedRewrittenEncodedURL,
		TestProxiedRewrittenDecodedURL,
		TestBackendForcedKeepalive,
		TestBackendUpgrade,
		TestBackendDelayedChunk,
		TestBackendNoLength,
	]

	def Prepare(self):
		self.http_backend = HttpBackend()
		self.plain_config = """
setup {{ module_load "mod_proxy"; }}

self_proxy = {{
	proxy "127.0.0.2:{self_port}";
}};
backend_proxy = {{
	proxy "127.0.0.2:{backend_port}";
}};
""".format(
		self_port = Env.port,
		backend_port = self.http_backend.port,
	)

	def Cleanup(self):
		self.http_backend.shutdown()
