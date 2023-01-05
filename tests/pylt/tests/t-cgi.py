# -*- coding: utf-8 -*-

from io import BytesIO
import time
import hashlib
import pycurl

from pylt.base import Env, GroupTest
from pylt.requests import CurlRequest
from pylt.service import FastCGI


def generate_body(seed, size):
	i = 0
	body = BytesIO()
	while body.tell() < size:
		body.write(hashlib.sha1((seed + str(i)).encode('utf-8')).digest())
		i += 1
	return body.getvalue()[:size]


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

SHA1SUM=$(which sha1sum sha1)

if [ ! -x "${SHA1SUM}" ]; then
	echo >&2 "Couldn't find sha1sum nor sha1 in PATH='${PATH}'; can't calculate upload checksum"
	printf 'Status: 404\r\nContent-Type: text/plain\r\n\r\n'
	printf "Couldn't find sha1sum nor sha1; can't calculate upload checksum"
	exit 0
fi

printf 'Status: 200\r\nContent-Type: text/plain\r\n\r\n'

csum=`"${SHA1SUM}" | cut -d' ' -f1`
printf '%s' "${csum}"

"""

SCRIPT_CHUNKEDENCODINGCHECK="""#!/bin/sh

printf 'Status: 200\\r\\nContent-Type: text/plain\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n'

printf 'd\\r\\nHello World!\\n\\r\\n0\\r\\n\\r\\n'

"""

# we don't actually check whether this ends up in the error log
# but at least try whether writing does break something
SCRIPT_STDERRCHECK="""#!/bin/sh

echo >&2 "This is not a real error"

printf 'Status: 404\r\nContent-Type: text/plain\r\n\r\n'
printf "Not a real error"
exit 0
"""

SCRIPT_EXITERRORCHECK="""#!/bin/sh

echo >&2 "This is not a real error"

printf 'Status: 404\r\nContent-Type: text/plain\r\n\r\n'
printf "Not a real error"
exit 1
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


class DelayedChunkedBodyReader:
	def __init__(self, body, chunksize = 32*1024):
		self.body = body
		self.chunksize = chunksize
		self.pos = 0

	def read(self, size):
		time.sleep(0.1)
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

	def PrepareRequest(self, curl, reqheaders):
		curl.setopt(curl.UPLOAD, 1)
		curl.setopt(pycurl.READFUNCTION, ChunkedBodyReader(BODY).read)


class TestChunkedEncoding1(CurlRequest):
	URL = "/chunkedencodingcheck.cgi"
	EXPECT_RESPONSE_BODY = "Hello World!\n"
	EXPECT_RESPONSE_CODE = 200


class TestStderr1(CurlRequest):
	URL = "/stderr.cgi"
	EXPECT_RESPONSE_BODY = "Not a real error"
	EXPECT_RESPONSE_CODE = 404


class TestExitError1(CurlRequest):
	URL = "/exiterror.cgi"
	EXPECT_RESPONSE_BODY = "Not a real error"
	EXPECT_RESPONSE_CODE = 404


class TestExitErrorUpload1(CurlRequest):
	URL = "/exiterror.cgi"
	EXPECT_RESPONSE_BODY = "Not a real error"
	EXPECT_RESPONSE_CODE = 404
	REQUEST_HEADERS = ["Transfer-Encoding: chunked"]

	def PrepareRequest(self, curl, reqheaders):
		curl.setopt(curl.UPLOAD, 1)
		curl.setopt(pycurl.READFUNCTION, DelayedChunkedBodyReader("test").read)


class Test(GroupTest):
	group = [
		TestPathInfo1,
		TestRequestUri1,
		TestUploadLarge1,
		TestUploadLargeChunked1,
		TestChunkedEncoding1,
		TestStderr1,
		TestExitError1,
		TestExitErrorUpload1,
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
		self.PrepareVHostFile("envcheck.cgi", SCRIPT_ENVCHECK, mode = 0o755)
		self.PrepareVHostFile("uploadcheck.cgi", SCRIPT_UPLOADCHECK, mode = 0o755)
		self.PrepareVHostFile("chunkedencodingcheck.cgi", SCRIPT_CHUNKEDENCODINGCHECK, mode = 0o755)
		self.PrepareVHostFile("stderr.cgi", SCRIPT_STDERRCHECK, mode = 0o755)
		self.PrepareVHostFile("exiterror.cgi", SCRIPT_EXITERRORCHECK, mode = 0o755)
