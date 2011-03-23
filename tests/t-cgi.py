# -*- coding: utf-8 -*-

from base import *
from requests import *
from service import FastCGI

class CGI(FastCGI):
	name = "fcgi_cgi"
	binary = [ Env.fcgi_cgi ]


SCRIPT_ENVCHECK="""#!/bin/sh

printf 'Status: 200\\r\\nContent-Type: text/plain\\r\\n\\r\\n'

envvar=${QUERY_STRING}
eval val='$'${envvar}

printf '%s' "${val}"

"""

class TestPathInfo1(CurlRequest):
	URL = "/envcheck.cgi/abc/xyz?PATH_INFO"
	EXPECT_RESPONSE_BODY = "/abc/xyz"
	EXPECT_RESPONSE_CODE = 200

class TestRequestUri1(CurlRequest):
	URL = "/envcheck.cgi/abc/xyz?REQUEST_URI"
	EXPECT_RESPONSE_BODY = "/envcheck.cgi/abc/xyz?REQUEST_URI"
	EXPECT_RESPONSE_CODE = 200

class Test(GroupTest):
	group = [
		TestPathInfo1,
		TestRequestUri1,
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

cgi {{
	fastcgi "unix:{socket}";
}}
""".format(socket = cgi.sockfile)

		self.tests.add_service(cgi)
		return True

	def Prepare(self):
		self.PrepareVHostFile("envcheck.cgi", SCRIPT_ENVCHECK, mode = 0755)
