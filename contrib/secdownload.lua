
-- secdownload.lua

-- usage:
-- a) load mod_lua and this plugin:
--   setup {
--       module_load "mod_lua";
--       lua.plugin "/path/secdownload.lua";
--   }
-- b) activate it anywhere you want:
--   secdownload [ "prefix" => "/sec/", "document-root" => "/secret/path", "secret" => "abc", "timeout" => 600 ];

local filename, args = ...

-- basepath for loading sub handlers with lua.handler
-- this allows us to have lua actions that don't use the global lock
local basepath = filename:gsub("(.*/)(.*)", "%1")

local function secdownload(options)
	if type(options) ~= "table" then
		lighty.error("secdownload expects a hashtable as parameter")
		return nil
	end

	local uri_prefix = options["prefix"]
	local doc_root = options["document-root"]
	local secret = options["secret"]
	local timeout = tonumber(options["timeout"]) or 60

	if secret == nil then
		lighty.error("secdownload: need secret in options")
		return nil
	end

	if doc_root == nil then
		lighty.error("secdownload: need doc_root in options")
		return nil
	end

	if doc_root:sub(-1) ~= "/" then
		doc_root = doc_root .. "/"
	end

	if uri_prefix == nil then
		uri_prefix = "/"
	end

	local args = { ["prefix"] = uri_prefix, ["document-root"] = doc_root, ["secret"] = secret, ["timeout"] = timeout }

	local handle_secdownload = action.lua.handler(basepath .. 'secdownload__secdownload.lua', nil, args)

	return handle_secdownload
end

actions = {
	["secdownload"] = secdownload
}
