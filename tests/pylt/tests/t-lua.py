# -*- coding: utf-8 -*-

from pylt.base import ModuleTest, TestBase
from pylt.requests import CurlRequest


LUA_STATE_ENV_INFO = """
-- globals should be reset after loading
info = "global info"

local function extract_info(vr)
    -- simple globals should be "per handler" (and request)
    info = (info or "") .. "handler global"
    -- special `REQ` allows request-global state across handlers
    REQ.info = (REQ.info or "") .. "request global:" .. vr.env["INFO"]
end

local function show_info(vr)
    if vr:handle_direct() then
        vr.resp.status = 200
        vr.resp.headers["Content-Type"] = "text/plain"
        vr.out:add((info or "") .. (REQ.info or ""))
    end
end

actions = action.list({extract_info, show_info})
"""


class TestLuaStateInfo1(CurlRequest):
    URL = "/?a_simple_query"
    EXPECT_RESPONSE_BODY = "request global:a_simple_query"
    EXPECT_RESPONSE_CODE = 200

    config = """
env.set "INFO" => "%{req.query}";
lua_state_env_info;
"""

class TestLuaStateInfo2(CurlRequest):
    URL = "/?b_simple_query"
    EXPECT_RESPONSE_BODY = "request global:b_simple_query"
    EXPECT_RESPONSE_CODE = 200

    config = """
env.set "INFO" => "%{req.query}";
lua_state_env_info;
"""

class TestLuaWorkerStateInfo1(CurlRequest):
    URL = "/?a_simple_query"
    EXPECT_RESPONSE_BODY = "request global:a_simple_query"
    EXPECT_RESPONSE_CODE = 200

    config = """
env.set "INFO" => "%{req.query}";
worker_lua_state_env_info;
"""

class TestLuaWorkerStateInfo2(CurlRequest):
    URL = "/?b_simple_query"
    EXPECT_RESPONSE_BODY = "request global:b_simple_query"
    EXPECT_RESPONSE_CODE = 200

    config = """
env.set "INFO" => "%{req.query}";
worker_lua_state_env_info;
"""

class Test(ModuleTest):
    def prepare_test(self) -> None:
        show_env_info_lua = self.prepare_file("lua/lua_state_env_info.lua", LUA_STATE_ENV_INFO)
        self.plain_config = f"""
lua_state_env_info = {{
    include_lua "{show_env_info_lua}";
}};
worker_lua_state_env_info = {{
    lua.handler "{show_env_info_lua}";
}};
"""
