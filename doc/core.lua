-- contrib.lua

-- usage:
-- load mod_lua and this plugin:
--   setup {
--       module_load "mod_lua";
--       lua.plugin "/path/contrib.lua";
--   }

local filename, args = ...

-- basepath for loading sub handlers with lua.handler
-- this allows us to have lua actions that don't use the global lock
local basepath = filename:gsub("(.*/)(.*)", "%1")

-- core.wsgi:
--   WSGI applications expect the url to be split into SCRIPT_NAME and
--   PATH_INFO; SCRIPT_NAME is their "application root", and PATH_INFO the requsted
--   resource in the application.
--   By default, lighttpd uses an empty PATH_INFO (unless you used the "pathinfo;" action,
--   but this doesn't help here as we're not dealing with static files here)
--   Important: WSGI is an "extension" of CGI; it doesn't specify a transport protocol,
--   you can use it with plain CGI, FastCGI or SCGI (or anything else that supports
--   the basic CGI protocol)
--
--   Usage:
--     core.wsgi ( <url-prefix>, ${ <backend-actions>; } );
--
--   Example: Trac in "/trac", listening via FastCGI on unix:/tmp/trac.socket
--     You oviously have to load mod_fastcgi for this.
--
--     core.wsgi ( "/trac", ${ fastcgi "unix:/tmp/trac.socket"; } );

local function wsgi(uri_prefix, act)
	if type(uri_prefix) ~= "string" then
		lighty.error("wsgi expects a string (uri-prefix) as first parameter")
		return nil
	end

	local uri_len = uri_prefix:len()

	local function wsgi_rewrite(vr)
		vr.phys.pathinfo = vr.req.uri.path:sub(uri_len)
		vr.req.uri.path = uri_prefix
	end

	return action.when(request.path:prefix(uri_prefix),
		action.list(wsgi_rewrite, act)
	)
end


-- try to find a file for the current url with ".html" prefix,
-- if url doesn't already belong to a file and has not already ".html" prefix
-- example:
--   core.cached_html;
local function cached_html()
	local try_cached_html = action.lua.handler(basepath .. 'core__cached_html.lua')

	return action.when(physical.is_file:isnot(), action.when(physical.path:notsuffix('.html'), try_cached_html))
end

-- Usage
--   auth.require_user (userlist);
-- Require a specific authenticated user; put it after an auth action.
-- Be careful: the empty username matches unauthenticated users.
-- Example:
--   auth.plain [ "method": "basic", "realm": "test", "file": "test.plain" ];
--   auth.require_user ("foo1", "foo2");
local function auth_require_user(...)
	local users = {...}
--	lighty.debug("auth_require_user")
	if #users == 0 then
		lighty.error("Empty userlist in auth.require_user")
		return nil
	else
		local escapedusers = {}
		for i, u in ipairs(users) do
			escapedusers[i] = u:gsub("[]|+*[\\^$-]", "\\%0")
		end
		local regex = "^(" .. table.concat(escapedusers, "|") .. ")$"
-- 		lighty.debug("auth_require_user REMOTE_USER regex: " .. regex)
		return action.when(request.environment["REMOTE_USER"]:nomatch(regex), action.auth.deny())
	end
end

-- Usage:
--   core.xsendfile docroot;
-- The parameter is the doc-root the files has to be in (can be omitted)
-- Example:
--   core.xsendfile "/srv/";
local function xsendfile(docroot)
	if docroot and type(docroot) =~ "string" then
		lighty.error("xsendfile: parameter has to be a string")
		return nil
	end

	local handle_x_sendfile = action.lua.handler(basepath .. 'core__xsendfile.lua', nil, docroot)

	return handle_x_sendfile
end


actions = {
	["core.wsgi"] = wsgi,
	["core.cached_html"] = cached_html,
	["auth.require_user"] = auth_require_user,
	["core.xsendfile"] = xsendfile
}
