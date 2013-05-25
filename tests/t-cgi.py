# -*- coding: utf-8 -*-

from base import *
from requests import *
from service import FastCGI
import hashlib

def generate_body(seed, size):
	i = 0
	body = ''
	while len(body) < size:
		body += hashlib.sha1(seed + str(i)).digest()
		i += 1
	return body[:size]

class CGI(FastCGI):
	name = "fcgi_cgi"
	binary = [ Env.fcgi_cgi ]


SCRIPT_ENVCHECK="""#!/bin/sh

printf 'Status: 200\\r\\nContent-Type: text/plain\\r\\n\\r\\n'

envvar=${QUERY_STRING}
eval val='$'${envvar}

printf '%s' "${val}"

"""

SCRIPT_UPLOADCHECK="""#!/bin/sh

printf 'Status: 200\\r\\nContent-Type: text/plain\\r\\n\\r\\n'

csum=`sha1sum | cut -d' ' -f1`
printf '%s' "${csum}"

"""

class TestPathInfo1(CurlRequest):
	URL = "/envcheck.cgi/abc/xyz?PATH_INFO"
	EXPECT_RESPONSE_BODY = "/abc/xyz"
	EXPECT_RESPONSE_CODE = 200

class TestRequestUri1(CurlRequest):
	URL = "/envcheck.cgi/abc/xyz?REQUEST_URI"
	EXPECT_RESPONSE_BODY = "/envcheck.cgi/abc/xyz?REQUEST_URI"
	EXPECT_RESPONSE_CODE = 200

BODY = generate_body('hello world', 2*1024*1024)
BODY_SHA1 = hashlib.sha1(BODY).hexdigest()

class TestUploadLarge1(CurlRequest):
	URL = "/uploadcheck.cgi"
	POST = BODY
	EXPECT_RESPONSE_BODY = BODY_SHA1
	EXPECT_RESPONSE_CODE = 200

class ChunkedBodyReader:
	def __init__(self, body, chunksize = 32*1024):
		self.body = body
		self.chunksize = chunksize
		self.pos = 0

	def read(self, size):
		current = self.pos
		rem = len(self.body) - current
		size = min(rem, self.chunksize, size)
		self.pos += size
		return self.body[current:current+size]

class TestUploadLargeChunked1(CurlRequest):
	URL = "/uploadcheck.cgi"
	EXPECT_RESPONSE_BODY = BODY_SHA1
	EXPECT_RESPONSE_CODE = 200
	REQUEST_HEADERS = ["Transfer-Encoding: chunked"]

	def PrepareRequest(self, reqheaders):
		c = self.curl
		c.setopt(c.UPLOAD, 1)
		c.setopt(pycurl.READFUNCTION, ChunkedBodyReader(BODY).read)

class Test(GroupTest):
	group = [
		TestPathInfo1,
		TestRequestUri1,
		TestUploadLarge1,
		TestUploadLargeChunked1
	]

	config = """
pathinfo;
if phys.exists and phys.path =$ ".cgi" {
	cgi;
} else {
	cgi;
}

"""

	def FeatureCheck(self):
		if None == Env.fcgi_cgi:
			return self.MissingFeature('fcgi-cgi')
		cgi = CGI()
		self.plain_config = """
setup {{ module_load "mod_fastcgi"; }}

cgi = {{
	fastcgi "unix:{socket}";
}};
""".format(socket = cgi.sockfile)

		self.tests.add_service(cgi)
		return True

	def Prepare(self):
		self.PrepareVHostFile("envcheck.cgi", SCRIPT_ENVCHECK, mode = 0755)
		self.PrepareVHostFile("uploadcheck.cgi", SCRIPT_UPLOADCHECK, mode = 0755)
