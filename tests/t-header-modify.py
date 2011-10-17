# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestHeaderAdd(CurlRequest):
	# use capture from previous regex conditional
	config = """
header.add "Test-Header" => "%{req.query}";
header.add "Test-Header" => "%{req.path}";
respond;
"""
	URL = "/path?simple_query"

	def CheckResponse(self):
		h = map(lambda x: x[1], filter(lambda x: x[0] == "Test-Header", self.resp_header_list))
		if len(h) != 2 or h[0] != "simple_query" or h[1] != "/path":
			print >>Env.log, repr(h)
			raise BaseException("Unexpected headers 'Test-Header'")
		return True

class TestHeaderAppend(CurlRequest):
	# use capture from previous regex conditional
	config = """
header.append "Test-Header" => "%{req.query}";
header.append "Test-Header" => "%{req.path}";
respond;
"""
	URL = "/path?simple_query"

	def CheckResponse(self):
		h = map(lambda x: x[1], filter(lambda x: x[0] == "Test-Header", self.resp_header_list))
		if len(h) != 1 or h[0] != "simple_query, /path":
			print >>Env.log, repr(h)
			raise BaseException("Unexpected headers 'Test-Header'")
		return True

class TestHeaderOverwrite(CurlRequest):
	# use capture from previous regex conditional
	config = """
header.overwrite "Test-Header" => "%{req.query}";
header.overwrite "Test-Header" => "%{req.path}";
respond;
"""
	URL = "/path?simple_query"

	def CheckResponse(self):
		h = map(lambda x: x[1], filter(lambda x: x[0] == "Test-Header", self.resp_header_list))
		if len(h) != 1 or h[0] != "/path":
			print >>Env.log, repr(h)
			raise BaseException("Unexpected headers 'Test-Header'")
		return True

class TestHeaderRemove(CurlRequest):
	# use capture from previous regex conditional
	config = """
header.add "Test-Header" => "%{req.query}";
header.remove "Test-Header";
respond;
"""
	URL = "/path?simple_query"

	def CheckResponse(self):
		h = map(lambda x: x[1], filter(lambda x: x[0] == "Test-Header", self.resp_header_list))
		if len(h) != 0:
			print >>Env.log, repr(h)
			raise BaseException("Unexpected headers 'Test-Header'")
		return True

class Test(GroupTest):
	group = [
		TestHeaderAdd,
		TestHeaderAppend,
		TestHeaderOverwrite,
		TestHeaderRemove,
	]
