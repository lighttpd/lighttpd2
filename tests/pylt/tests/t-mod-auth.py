# -*- coding: utf-8 -*-

from pylt.base import GroupTest
from pylt.requests import CurlRequest


#userI:passI for I in [1..4] with [apr-md5, crypt, plain and apr-sha]
PASSWORDS="""user1:$apr1$mhpONdUp$xSRcAbK2F6hLFUzW59tzW/
user2:JTMoqfZHCS0aI
user3:pass3
user4:{SHA}LbTBgR9CRYKpD41+53mVzwGNlEM=
"""

# user5:pass5 in realm 'Realm1'
DIGESTPASSWORDS="""user5:Realm1:b0590e8c95605dd708226b552fc86a22
"""


class TestAprMd5Fail(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 401
	AUTH = "user1:test1"


class TestAprMd5Success(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 200
	AUTH = "user1:pass1"


class TestCryptFail(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 401
	AUTH = "user2:test2"


class TestCryptSuccess(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 200
	AUTH = "user2:pass2"


class TestPlainFail(CurlRequest):
	URL = "/test.txt?plain"
	EXPECT_RESPONSE_CODE = 401
	AUTH = "user3:test3"


class TestPlainSuccess(CurlRequest):
	URL = "/test.txt?plain"
	EXPECT_RESPONSE_CODE = 200
	AUTH = "user3:pass3"


class TestAprSha1Fail(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 401
	AUTH = "user4:test4"

class TestAprSha1Success(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 200
	AUTH = "user4:pass4"


class TestDigestFail(CurlRequest):
	URL = "/test.txt?digest"
	EXPECT_RESPONSE_CODE = 401
	AUTH = "user5:test5"


class TestDigestSuccess(CurlRequest):
	URL = "/test.txt?digest"
	EXPECT_RESPONSE_CODE = 200
	AUTH = "user5:pass5"


class TestRequireUserDeny(CurlRequest):
	URL = "/test.txt?require1"
	EXPECT_RESPONSE_CODE = 403
	AUTH = "user4:pass4"


class TestRequireUserSuccess(CurlRequest):
	URL = "/test.txt?require1"
	EXPECT_RESPONSE_CODE = 200
	AUTH = "user1:pass1"


class TestDeny(CurlRequest):
	URL = "/test.txt?deny"
	EXPECT_RESPONSE_CODE = 403


class Test(GroupTest):
	group = [
		TestAprMd5Fail, TestAprMd5Success,
		TestCryptFail, TestCryptSuccess,
		TestPlainFail, TestPlainSuccess,
		TestAprSha1Fail, TestAprSha1Success,
		TestDigestFail, TestDigestSuccess,
		TestRequireUserDeny, TestRequireUserSuccess,
		TestDeny,
	]

	def Prepare(self):
		passwdfile = self.PrepareFile("conf/mod-auth.htpasswd", PASSWORDS)
		digestfile = self.PrepareFile("conf/mod-auth.htdigest", DIGESTPASSWORDS)

		self.config = """
			setup {{ module_load ( "mod_auth" ); }}

			auth.debug true;
			if req.query == "plain" {{
				auth.plain ["method" => "basic", "realm" => "Basic Auth Realm", "file" => "{passwdfile}", "ttl" => 10];
			}} else if req.query == "digest" {{
				auth.htdigest ["method" => "basic", "realm" => "Realm1", "file" => "{digestfile}", "ttl" => 10];
			}} else if req.query == "deny" {{
				auth.deny;
			}} else {{
				auth.htpasswd ["method" => "basic", "realm" => "Basic Auth Realm", "file" => "{passwdfile}", "ttl" => 10];
			}}
			if req.query == "require1" {{
				auth.require_user ("user1");
			}}

			defaultaction;
		""".format(passwdfile = passwdfile, digestfile = digestfile)
