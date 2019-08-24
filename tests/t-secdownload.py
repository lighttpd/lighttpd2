# -*- coding: utf-8 -*-

from base import *
from requests import *
import time
from hashlib import md5

def securl(prefix, path, secret, tstamp = None):
	if tstamp == None: tstamp = time.time()
	tstamp = '%x' % int(tstamp)
	md5content = secret + path + tstamp
	if prefix[-1] != '/': prefix += '/'
	return prefix + md5(md5content.encode('utf-8')).hexdigest() + '/' + tstamp + path

class SecdownloadFail(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 403

class SecdownloadSuccess(CurlRequest):
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200

	def Run(self):
		self.URL = securl('/', '/test.txt', 'abc')
		return super(SecdownloadSuccess, self).Run()

class SecdownloadGone(CurlRequest):
	EXPECT_RESPONSE_CODE = 410

	def Run(self):
		self.URL = securl('/', '/test.txt', 'abc', time.time() - 800)
		return super(SecdownloadGone, self).Run()

class Test(GroupTest):
	group = [SecdownloadFail, SecdownloadSuccess, SecdownloadGone]
	config = """
secdownload ( "prefix" => "/", "document-root" => var.default_docroot, "secret" => "abc", "timeout" => 600 );
"""
	no_docroot = True
