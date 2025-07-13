# -*- coding: utf-8 -*-

import ipaddress
import struct
from pylt.requests import CurlRequest, TEST_TXT


class TestSimpleRequest(CurlRequest):
    PORT_OFFSET = 1
    SCHEME = "https"
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
    vhost = "test1.ssl.test"


class TestSNI(CurlRequest):
    PORT_OFFSET = 1
    SCHEME = "https"
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
    vhost = "test2.ssl.test"


class TestProxyProtV1(CurlRequest):
    PORT_OFFSET = 1
    SCHEME = "https"
    URL = "/"
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_BODY = "198.51.100.1"
    CURLOPT_HAPROXY_CLIENT_IP = "198.51.100.1"
    config = """
respond "%{req.remoteip}";
"""


class TestProxyProtV2(CurlRequest):
    PORT_OFFSET = 1
    SCHEME = "https"
    URL = "/"
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_BODY = "198.51.100.1"
    CONNECTION_SEND_INITIAL = struct.pack(
        "!12s2sH4s4sHH",
        b"\x0D\x0A\x0D\x0A\x00\x0D\x0AQUIT\x0A",
        b"\x21\x11",  # version/command/...
        4 + 4 + 2 + 2,  # address length
        ipaddress.IPv4Address("198.51.100.1").packed,
        ipaddress.IPv4Address("127.0.0.2").packed,
        65535, # client port
        443, # server port
    )
    config = """
respond "%{req.remoteip}";
"""
