# -*- coding: utf-8 -*-

import socketserver
import threading

from pylt.base import Tests, ModuleTest
from pylt.requests import CurlRequest, TEST_TXT


class HttpBackendHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        keepalive = True
        while True:
            reqline_full = self.rfile.readline().decode().rstrip()
            # eprint("Request line: " + repr(reqline))
            reqline = reqline_full.split(' ', 3)
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
                hdr_line = self.rfile.readline().decode().rstrip()
                if hdr_line == "":
                    break
                hdr = hdr_line.split(':', 2)
                if hdr[0].lower() == "connection":
                    keepalive = (hdr[1].strip().lower() == "keep-alive")
            if reqline[1].startswith("/upgrade/custom"):
                self.wfile.write(
                    b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
                    b"Upgrade: custom\r\n\r\nHello World!"
                )
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
            resp_body = reqline[1].encode()
            clen = f"Content-Length: {len(resp_body)}\r\n".encode()
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

    def __init__(self, *, tests: Tests) -> None:
        self.port = tests.env.port + 3
        super().__init__(('127.0.0.2', self.port), HttpBackendHandler)

        self.listen_thread = threading.Thread(
            target=self.serve_forever,
            name=f"HttpBackend-{self.port}",
        )
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


class Test(ModuleTest):
    def prepare_test(self) -> None:
        self.http_backend = HttpBackend(tests=self.tests)
        self.plain_config = f"""
setup {{ module_load "mod_proxy"; }}

self_proxy = {{
    proxy "127.0.0.2:{self.tests.env.port}";
}};
backend_proxy = {{
    proxy "127.0.0.2:{self.http_backend.port}";
}};
"""

    def cleanup_test(self) -> None:
        self.http_backend.shutdown()
