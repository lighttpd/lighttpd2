# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestSimple(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 200
	config = """
req_header.overwrite "Host" => "basic-gets";
self_proxy;
"""
	no_docroot = True

# need vhost for next test
class TestEncodedURL(CurlRequest):
	URL = "/some%2Ffile"
	EXPECT_RESPONSE_BODY = "/dest%2Ffile"
	EXPECT_RESPONSE_CODE = 200
	no_docroot = True
	config = """
rewrite_raw "/some(%2F.*)" => "/dest$1";
respond 200 => "%{req.raw_path}";
"""

# backend gets encoded %2F and rewrites again
class TestProxiedRewrittenEncodedURL(CurlRequest):
	URL = "/foo%2Ffile"
	EXPECT_RESPONSE_BODY = "/dest%2Ffile"
	EXPECT_RESPONSE_CODE = 200
	no_docroot = True
	config = """
rewrite_raw "/foo(.*)" => "/some$1";
req_header.overwrite "Host" => "encodedurl.mod-proxy";
self_proxy;
"""

# backend gets decoded %2F and doesn't rewrite again
class TestProxiedRewrittenDecodedURL(CurlRequest):
	URL = "/foo%2Ffile"
	EXPECT_RESPONSE_BODY = "/some/file"
	EXPECT_RESPONSE_CODE = 200
	no_docroot = True
	config = """
rewrite "/foo(.*)" => "/some$1";
req_header.overwrite "Host" => "encodedurl.mod-proxy";
self_proxy;
"""

class Test(GroupTest):
	group = [
		TestSimple,
		TestEncodedURL,
		TestProxiedRewrittenEncodedURL,
		TestProxiedRewrittenDecodedURL,
	]

	def Prepare(self):
		self.plain_config = """
setup {{ module_load "mod_proxy"; }}

self_proxy = {{
	proxy "127.0.0.2:{self_port}";
}};
""".format(self_port = Env.port)
