
local filename, args = ...

local uri_prefix = args["prefix"]
local doc_root = args["document-root"]
local secret = args["secret"]
local timeout = tonumber(args["timeout"])

local function deny_access(vr, status)
	vr:handle_direct()
	vr.resp.status = status
end

local function handle_secdownload(vr)
	local path = vr.req.uri.path
	local prefix_len = uri_prefix:len()
	if path:sub(1, prefix_len) == uri_prefix and not vr.is_handled then
		local md5str = path:sub(prefix_len + 1, prefix_len + 32)

		if md5str:len() ~= 32 then return deny_access(vr, 403) end

		if path:sub(prefix_len + 33, prefix_len + 33) ~= "/" then return deny_access(vr, 403) end

		local slash = path:find("/", prefix_len + 34)
		if slash == nil then return deny_access(vr, 403) end

		local ts_string = path:sub(prefix_len + 34, slash - 1)
		local timestamp = tonumber(ts_string, 16)
		if timestamp == nil then return deny_access(vr, 403) end

		path = path:sub(slash)

		local ts = os.time()
		if (timestamp < ts - timeout) or (timestamp > ts + timeout) then
			-- Gone, not Timeout (don't retry later)
			return  deny_access(vr, 410)
		end

		-- modify md5content as you wish :)
		local md5content = secret .. path .. ts_string
		-- vr:debug("checking md5: '" .. md5content .. "', md5: " .. md5str)
		if md5str ~= lighty.md5(secret .. path .. ts_string) then
			return deny_access(vr, 403)
		end

		-- rewrite physical paths
		vr.phys.doc_root = doc_root
		vr.phys.path = doc_root .. path
	end
end

actions = handle_secdownload
