# -*- coding: utf-8 -*-

import bz2
import dataclasses
import io
import os
import socket
import typing
import zlib

import pycurl

from pylt.base import log, TestBase, Tests


TEST_TXT = """Hi!
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
"""


class CurlRequestException(Exception):
    pass


@dataclasses.dataclass
class Response:
    _debug_requests: bool = dataclasses.field(repr=False, default=False)
    first_line: str = ''
    code: int = 0
    header_list: list[tuple[str, str]] = dataclasses.field(default_factory=list)
    headers: dict[str, str] = dataclasses.field(default_factory=dict)  # lowercased keys
    headers_done: bool = False
    body_raw: io.BytesIO = dataclasses.field(default_factory=io.BytesIO)
    body_decoded: typing.Optional[str] = None
    body_complete: bool = False

    def recv_header_line(self, /, line_raw: bytes) -> None:
        if self.headers_done:
            raise Exception('Unexpected header line - headers already finished')
        line = line_raw.decode().rstrip()
        if not self.first_line:
            if not line:
                raise Exception('First line must not be empty')
            self.first_line = line
            parts = line.split(maxsplit=2)
            if len(parts) != 3:
                raise Exception(f'Invalid first line {line!r}')
            _version, code, _reason = parts
            self.code = int(code)
        elif not line:
            if self.code == 100:
                if self._debug_requests:
                    log(f"Handling 100 Continue: {self.first_line!r}")
                # wait for "new" first line
                self.first_line = ''
            else:
                self.headers_done = True
        else:
            parts = line.split(":", maxsplit=1)
            if len(parts) != 2:
                raise Exception(f'Invalid header line {line!r}')
            (key, value) = (parts[0].strip(), parts[1].strip())
            self.header_list.append((key, value))
            key = key.lower()
            if key in self.headers:
                self.headers[key] += f", {value}"
            else:
                self.headers[key] = value

    def get_all_headers(self, key: str) -> list[str]:
        key = key.lower()
        return [
            v
            for k, v in self.header_list
            if k.lower() == key
        ]

    @property
    def body(self) -> str:
        if not self.body_decoded is None:
            return self.body_decoded
        if not self.body_complete:
            raise Exception('Response not complete')
        data = self.body_raw.getvalue()
        cenc = self.headers.get("content-encoding", None)
        if not cenc is None:
            data = self._decode(cenc, data)
        self.body_decoded = data.decode()
        return self.body_decoded

    @staticmethod
    def _decode(method: str, data: bytes) -> bytes:
        if 'x-gzip' == method or 'gzip' == method:
            header = data[:10]
            if b"\x1f\x8b\x08\x00\x00\x00\x00\x00" != header[:8]:
                raise CurlRequestException("Unsupported content-encoding gzip header")
            return zlib.decompress(data[10:], -15)
        elif 'deflate' == method:
            return zlib.decompress(data, -15)
        elif 'compress' == method:
            raise CurlRequestException(f"Unsupported content-encoding {method}")
        elif 'x-bzip2' == method or 'bzip2' == method:
            return bz2.decompress(data)
        else:
            raise CurlRequestException(f"Unsupported content-encoding {method}")

    def dump(self) -> None:
        log(f"Response code: {self.code}")
        log("Response headers:")
        for (k, v) in self.header_list:
            log(f"  {k}: {v}")
        if self.body_complete:
            try:
                body = self.body
            except Exception:
                log("Failed decoding response body, raw:")
                log(repr(self.body_raw.getbuffer()))
            else:
                log("Response body:")
                log(body)
        else:
            log("Response body incomplete, raw:")
            log(repr(self.body_raw.getbuffer()))


class _BaseRequest(TestBase):
    _NO_REGISTER = True  # only for metaclass

    URL: str
    SCHEME: str = "http"
    PORT_OFFSET: int = 0  # offset to Env.port
    AUTH: typing.Optional[str] = None
    POST: typing.Optional[bytes] = None
    REQUEST_HEADERS: typing.Sequence[str] = ()
    ACCEPT_ENCODING: typing.Optional[str] = "deflate, gzip"

    EXPECT_RESPONSE_BODY: typing.Optional[str] = None
    EXPECT_RESPONSE_CODE: typing.Optional[int] = None
    # list of expected headers
    # (hdr, None) means header must not be present
    EXPECT_RESPONSE_HEADERS: typing.Sequence[tuple[str, typing.Optional[str]]] = ()

    def __init__(self, *, tests: Tests, parent: TestBase) -> None:
        super().__init__(tests=tests, parent=parent)
        self.port = self.tests.env.port + self.PORT_OFFSET
        self.response = Response(_debug_requests=self.tests.env.debugRequests)

    def build_headers(self) -> list[str]:
        headers = [f"Host: {self.vhost}"]
        headers.extend(self.REQUEST_HEADERS)
        if self.ACCEPT_ENCODING:
            headers.append(f"Accept-Encoding: {self.ACCEPT_ENCODING}")
        return headers

    def _checkResponse(self) -> bool:
        if self.tests.env.debugRequests:
            self.dump()

        if not self.CheckResponse():
            return False

        if not self.EXPECT_RESPONSE_CODE is None:
            if self.response.code != self.EXPECT_RESPONSE_CODE:
                raise CurlRequestException(
                    f"Unexpected response code {self.response.code} (wanted {self.EXPECT_RESPONSE_CODE})"
                )

        if not self.EXPECT_RESPONSE_BODY is None:
            if self.response.body != self.EXPECT_RESPONSE_BODY:
                raise CurlRequestException("Unexpected response body")

        for (k, v) in self.EXPECT_RESPONSE_HEADERS:
            resp_hdr_v = self.response.headers.get(k.lower(), None)
            if v is None:
                if not resp_hdr_v is None:
                    raise CurlRequestException(
                        f"Got unwanted response header {k!r} = {resp_hdr_v!r}"
                    )
            else:
                if resp_hdr_v is None:
                    raise CurlRequestException(f"Didn't get wanted response header {k!r}")
                if resp_hdr_v != v:
                    raise CurlRequestException(
                        f"Unexpected response header {k!r} = {resp_hdr_v!r} (wanted {v!r})"
                    )

        return True

    def CheckResponse(self) -> bool:
        return True

    def dump(self) -> None:
        raise NotImplementedError()


class CurlRequest(_BaseRequest):
    _NO_REGISTER = True  # only for metaclass

    CURLOPT_HAPROXY_CLIENT_IP: str | None = None
    CONNECTION_SEND_INITIAL: bytes = b""

    curl_url: str

    def prepare_curl_request(self, curl: pycurl.Curl) -> None:
        pass

    def run_test(self) -> bool:
        reqheaders = self.build_headers()
        c = pycurl.Curl()
        c.setopt(pycurl.CAINFO, os.path.join(self.tests.env.sourcedir, "tests", "ca", "ca.crt"))
        if hasattr(pycurl, 'RESOLVE') and self.vhost:
            url_host = self.vhost
            c.setopt(pycurl.RESOLVE, [f'{self.vhost}:{self.port}:127.0.0.2'])
        else:
            url_host = '127.0.0.2'
            c.setopt(pycurl.SSL_VERIFYHOST, 0)
        self.curl_url = f"{self.SCHEME}://{url_host}:{self.port}{self.URL}"
        c.setopt(pycurl.URL, self.curl_url)
        c.setopt(pycurl.HTTPHEADER, reqheaders)
        c.setopt(pycurl.NOSIGNAL, 1)
        # ssl connections sometimes have timeout issues. could be entropy related..
        # use 10 second timeout instead of 2 for ssl - only 3 requests, shouldn't hurt
        c.setopt(pycurl.TIMEOUT, ("http" == self.SCHEME and 2) or 10)
        c.setopt(pycurl.WRITEFUNCTION, self.response.body_raw.write)
        c.setopt(pycurl.HEADERFUNCTION, self.response.recv_header_line)
        if not self.POST is None:
            c.setopt(pycurl.POSTFIELDS, self.POST)

        if not self.AUTH is None:
            c.setopt(pycurl.USERPWD, self.AUTH)
            c.setopt(pycurl.FOLLOWLOCATION, 1)
            c.setopt(pycurl.MAXREDIRS, 5)

        hook_connect = False
        send_raw_haproxy = False

        if self.CURLOPT_HAPROXY_CLIENT_IP:
            if hasattr(pycurl, 'HAPROXY_CLIENT_IP'):
                c.setopt(pycurl.HAPROXYPROTOCOL, 1)
                c.setopt(pycurl.HAPROXY_CLIENT_IP, self.CURLOPT_HAPROXY_CLIENT_IP)
            else:
                hook_connect = True
                send_raw_haproxy = True

        if self.CONNECTION_SEND_INITIAL:
            hook_connect = True

        if hook_connect:
            def opensocket(purpose, address):
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, 0)
                sock.connect(('127.0.0.2', self.port))
                if self.CONNECTION_SEND_INITIAL:
                    sock.sendall(self.CONNECTION_SEND_INITIAL)
                if send_raw_haproxy:
                    local_ip, local_port = sock.getsockname()
                    sock.sendall(f"PROXY TCP4 {self.CURLOPT_HAPROXY_CLIENT_IP} 127.0.0.2 {local_port} {self.port}\r\n".encode())
                return sock

            def setopt(curlfd, purpose):
                return pycurl.SOCKOPT_ALREADY_CONNECTED

            c.setopt(pycurl.OPENSOCKETFUNCTION, opensocket)
            c.setopt(pycurl.SOCKOPTFUNCTION, setopt)

        self.prepare_curl_request(c)

        c.perform()

        self.response.body_complete = True

        try:
            curl_resp_code = c.getinfo(pycurl.RESPONSE_CODE)
            if curl_resp_code != self.response.code:
                raise CurlRequestException(
                    f"curl returned respose code {curl_resp_code}, but headers show {self.response.code}"
                )

            if not self._checkResponse():
                raise CurlRequestException("Response check failed")
        except Exception:
            if not self.tests.env.debugRequests:
                self.dump()
            raise
        finally:
            c.close()

        return True

    def dump(self) -> None:
        self.tests.env.log.flush()
        log(f"Dumping request for test {self.name!r}")
        log(f"Curl request: URL = {self.curl_url}")
        self.response.dump()
        self.tests.env.log.flush()


class RawRequest(_BaseRequest):
    _NO_REGISTER = True  # only for metaclass

    def __init__(self, *, tests: Tests, parent: TestBase):
        super().__init__(tests=tests, parent=parent)
        self.request = b''

    def run_test(self) -> bool:
        if not self.URL:
            raise Exception("You have to set URL in your CurlRequest instance")
        reqheaders = self.build_headers()
        if self.SCHEME != 'http':
            raise Exception(f"Unsupported scheme {self.SCHEME!r}")
        if not self.POST is None:
            raise Exception("POST not supported")
        if not self.AUTH is None:
            raise Exception("AUTH not supported")

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as tcp_con:
            tcp_con.settimeout(2)
            tcp_con.connect(('127.0.0.2', self.port))

            req = f'GET {self.URL} HTTP/1.1\r\n'.encode()
            for hdr in reqheaders:
                req += hdr.encode() + b'\r\n'
            # not implementing content-length handling / keep-alive / ...
            req += b'Connection: close\r\n'
            req += b'\r\n'
            self.request = req

            tcp_con.sendall(req)
            response = b''
            while True:
                chunk = tcp_con.recv(4069)
                if not chunk:
                    break
                response += chunk

        while True:
            if not b'\n' in response:
                raise Exception("Response missing header end")
            line, response = response.split(b'\n', maxsplit=1)
            if not line or line == b'\r':
                break
            self.response.recv_header_line(line)

        self.response.body_raw.write(response)
        self.response.body_raw.seek(0, io.SEEK_SET)
        self.response.body_complete = True

        try:
            if not self._checkResponse():
                raise CurlRequestException("Response check failed")
        except Exception:
            if not self.tests.env.debugRequests:
                self.dump()
            raise

        return True

    def dump(self):
        self.tests.env.log.flush()
        log(f"Dumping request for test {self.name!r}")
        log("Request:")
        for line in self.request.splitlines():
            log(f"  {line.decode()}")
        self.response.dump()
        self.tests.env.log.flush()
