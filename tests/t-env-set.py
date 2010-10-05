# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestPatternCapture(CurlRequest):
	# use capture from previous regex conditional
	config = """
if req.path =~ "(.*)" {
	env.set "INFO" => "%1";
	show_env_info;
}
"""
	URL = "/path/?a_simple_query"
	EXPECT_RESPONSE_BODY = "/path/"
	EXPECT_RESPONSE_CODE = 200

class TestPatternCaptureRange(CurlRequest):
	# use capture from previous regex conditional
	config = """
if req.path =~ "/([^/]*)/(.*)" {
	env.set "INFO" => "%[1-2]";
	show_env_info;
}
"""
	URL = "/path/xyz"
	EXPECT_RESPONSE_BODY = "pathxyz"
	EXPECT_RESPONSE_CODE = 200

class TestPatternCaptureRevRange(CurlRequest):
	# use capture from previous regex conditional
	config = """
if req.path =~ "/([^/]*)/(.*)" {
	env.set "INFO" => "%[2-1]";
	show_env_info;
}
"""
	URL = "/path/xyz"
	EXPECT_RESPONSE_BODY = "xyzpath"
	EXPECT_RESPONSE_CODE = 200

class TestPatternEncodingPath(CurlRequest):
	# encoding path
	config = """
env.set "INFO" => "%{enc:req.path}";
show_env_info;
"""
	URL = "/complicated%3fpath%3d%20%24"
	EXPECT_RESPONSE_BODY = "/complicated%3fpath%3d%20%24"
	EXPECT_RESPONSE_CODE = 200

class TestPatternCombine(CurlRequest):
	# combine several pieces
	config = """
env.set "INFO" => "Abc:%{enc:req.path}:%{req.query}:%{req.host}";
show_env_info;
"""
	URL = "/complicated%3fpath%3d%20%24?a_simple_query"
	EXPECT_RESPONSE_CODE = 200

	def Prepare(self):
		self.EXPECT_RESPONSE_BODY = "Abc:/complicated%3fpath%3d%20%24:a_simple_query:" + self.vhost

class TestPatternEscape(CurlRequest):
	config = """
env.set "INFO" => "\\\\%\\\\?\\\\$\\\\%{req.path}";
show_env_info;
"""
	URL = "/abc"
	EXPECT_RESPONSE_BODY = "%?$%{req.path}"
	EXPECT_RESPONSE_CODE = 200


class Test(GroupTest):
	group = [
		TestPatternCapture,
		TestPatternCaptureRange,
		TestPatternCaptureRevRange,
		TestPatternEncodingPath,
		TestPatternCombine,
		TestPatternEscape,
	]
