# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestSimple(CurlRequest):
	vhost = "xyz.abc.basic-docroot"
	URL = "/?simple"
	EXPECT_RESPONSE_BODY = "/var/www/xyz.abc.basic-docroot/htdocs"
	EXPECT_RESPONSE_CODE = 200

class TestSubdir(CurlRequest):
	vhost = "xyz.abc.basic-docroot"
	URL = "/?subdir"
	EXPECT_RESPONSE_BODY = "/var/www/basic-docroot/xyz.abc.basic-docroot/htdocs"
	EXPECT_RESPONSE_CODE = 200

class TestSubdirOpenRange(CurlRequest):
	vhost = "test.xyz.abc.basic-docroot"
	URL = "/?subdir-open-range"
	EXPECT_RESPONSE_BODY = "/var/www/basic-docroot/test.xyz.abc/htdocs"
	EXPECT_RESPONSE_CODE = 200

class TestSubdirFixedRange(CurlRequest):
	vhost = "test.xyz.abc.basic-docroot"
	URL = "/?subdir-fixed-range"
	EXPECT_RESPONSE_BODY = "/var/www/basic-docroot/xyz.abc/htdocs"
	EXPECT_RESPONSE_CODE = 200

class TestCascade(CurlRequest):
	URL = "/?cascade"
	EXPECT_RESPONSE_BODY = "/"
	EXPECT_RESPONSE_CODE = 200

class TestCascadeFallback(CurlRequest):
	URL = "/?cascade-fallback"
	EXPECT_RESPONSE_BODY = "/var/www/fallback/htdocs"
	EXPECT_RESPONSE_CODE = 200

class Test(GroupTest):
	group = [
		TestSimple,
		TestSubdir,
		TestSubdirOpenRange,
		TestSubdirFixedRange,
		TestCascade,
		TestCascadeFallback
	]

	vhost = "basic-docroot"
	subdomains = True
	config = """

if req.query == "simple" {
	docroot "/var/www/$0/htdocs";
} else if req.query == "subdir" {
	docroot "/var/www/$[1]/$[0]/htdocs";
} else if req.query == "subdir-open-range" {
	docroot "/var/www/$1/$[2-]/htdocs";
} else if req.query == "subdir-fixed-range" {
	docroot "/var/www/$1/$[2-3]/htdocs";
} else if req.query == "cascade" {
	docroot ("/","/var/www/fallback/htdocs");
} else if req.query == "cascade-fallback" {
	docroot ("_","/var/www/fallback/htdocs");
}

env.set "INFO" => "%{phys.docroot}";
show_env_info;

"""
