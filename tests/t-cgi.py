# -*- coding: utf-8 -*-

from base import *
from requests import *
from service import FastCGI

class CGI(FastCGI):
	name = "fcgi_cgi"
	binary = [ Env.fcgi_cgi ]


SCRIPT_PATHINFO="""#!/bin/sh

echo -en 'Status: 200\\r\\nContent-Type: text/plain\\r\\n\\r\\n'

echo -n ${PATH_INFO}

"""

class TestPathInfo1(CurlRequest):
	URL = "/pathinfo.cgi/abc/xyz"
	EXPECT_RESPONSE_BODY = "/abc/xyz"
	EXPECT_RESPONSE_CODE = 200

class Test(GroupTest):
	group = [
		TestPathInfo1,
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
		self.PrepareVHostFile("pathinfo.cgi", SCRIPT_PATHINFO, mode = 0755)
