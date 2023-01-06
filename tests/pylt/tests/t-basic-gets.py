# -*- coding: utf-8 -*-

from pylt.base import ModuleTest, TestBase
from pylt.requests import CurlRequest, RawRequest, TEST_TXT


LUA_SHOW_ENV_INFO = """
function show_env_info(vr)
    if vr:handle_direct() then
        vr.resp.status = 200
        vr.resp.headers["Content-Type"] = "text/plain"
        vr.out:add(vr.env["INFO"])
    end
end

actions = show_env_info
"""


class TestSimpleRequest(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]


class TestSimpleRequestStatus(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 403
    config = """
defaultaction;
static_no_fail;
set_status 403;
"""


class TestSimpleRespond(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = "hello"
    EXPECT_RESPONSE_CODE = 200
    config = 'respond "hello";'


class TestIndex1(CurlRequest):
    URL = "/"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200
    config = """
defaultaction;
index "test.txt";
"""


class TestIndex2(CurlRequest):
    URL = "/"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200
    config = """
defaultaction;
index "index.html", "test.txt";
"""


class TestIndexNotExisting1(CurlRequest):
    URL = "/not-existing"
    EXPECT_RESPONSE_CODE = 404
    config = """
defaultaction;
index "index.html", "test.txt";
"""


class TestIndex3(CurlRequest):
    URL = "/not-existing"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200
    config = """
defaultaction;
index "/index.html", "/test.txt";
"""


class TestSimpleInfo(CurlRequest):
    URL = "/?a_simple_query"
    EXPECT_RESPONSE_BODY = "a_simple_query"
    EXPECT_RESPONSE_CODE = 200

    config = """
env.set "INFO" => "%{req.query}";
show_env_info;
"""


class TestBadRequest1(RawRequest):
    # unencoded query
    URL = "/?complicated?query= $"
    EXPECT_RESPONSE_CODE = 400


class TestStaticExcludeExtensions1(CurlRequest):
    URL = "/test.php"
    EXPECT_RESPONSE_CODE = 403
    config = """
defaultaction;
static.exclude_extensions ".php";
"""


class TestStaticExcludeExtensions2(CurlRequest):
    URL = "/test.php"
    EXPECT_RESPONSE_CODE = 403
    config = """
defaultaction;
static.exclude_extensions (".php", ".py");
"""


class TestServerTag(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("Server", "apache - no really!")]
    config = """
defaultaction;
server.tag "apache - no really!";
"""


class TestConditionalHeader1(CurlRequest):
    URL = "/"
    EXPECT_RESPONSE_BODY = "a"
    REQUEST_HEADERS = ["X-Select: a"]
    config = """
if req.header["X-Select"] == "a" {
    respond "a";
} else {
    respond "b";
}
"""


class TestConditionalHeader2(CurlRequest):
    URL = "/"
    EXPECT_RESPONSE_BODY = "b"
    config = """
if req.header["X-Select"] == "a" {
    respond "a";
} else {
    respond "b";
}
"""


class TestSimplePattern1(CurlRequest):
    URL = "/"
    EXPECT_RESPONSE_CODE = 403
    EXPECT_RESPONSE_BODY = "hello"
    REQUEST_HEADERS = ["X-Select: hello"]
    config = """
respond 403 => "%{req.header[X-Select]}";
"""


class ProvideStatus(TestBase):
    runnable = False
    vhost = "status"
    config = """
setup { module_load "mod_status"; }
status.info;
"""


class Test(ModuleTest):
    def prepare_test(self) -> None:
        self.prepare_file("www/default/test.txt", TEST_TXT)
        self.prepare_file("www/default/test.php", "")
        show_env_info_lua = self.prepare_file("lua/show_env_info.lua", LUA_SHOW_ENV_INFO)
        self.plain_config = f"""
show_env_info = {{
    lua.handler "{show_env_info_lua}";
}};
"""
