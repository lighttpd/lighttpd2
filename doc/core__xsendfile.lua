
local filename, args = ...

local docroot = args

-- create new class XFilterDrop
local XFilterDrop = { }
XFilterDrop.__index = XFilterDrop

-- "classmethod" to create new instance
function XFilterDrop:new(vr)
--	vr:debug("New XSendfile instance")
	local o = { }
	setmetatable(o, self)
	return o
end

-- normal method to handle content
function XFilterDrop:handle(vr, outq, inq)
	-- drop further input (we closed it already)
	inq:skip_all()
	return lighty.HANDLER_GO_ON
end

-- create a new filter which drops all input and adds it to the vrequest
-- returns the filter object so you can insert your own content in f.out (it is already closed)
local function add_drop_filter(vr)
	local f = vr:add_filter_out(XFilterDrop:new())
	f['in'].is_closed = true
	f['in']:skip_all()
	f.out.is_closed = true
	return f
end

local function handle_x_sendfile(vr)
-- 	vr:debug("handle x-sendfile")
	-- wait for response
	if not vr.has_response then
		if vr.is_handled then
-- 			vr:debug("x-sendfile: waiting for response headers")
			return lighty.HANDLER_WAIT_FOR_EVENT
		else
-- 			vr:debug("No response handler yet, cannot handle X-Sendfile")
			return lighty.HANDLER_GO_ON
		end
	end
-- 	vr:debug("handle x-sendfile: headers available")
	-- add filter if x-sendfile header is not empty
	local xs = vr.resp.headers["X-Sendfile"]
	if xs and xs ~= "" then
		xs = lighty.path_simplify(xs)
		if docroot and xs:sub(docroot.len()) =~ docroot then
			vr:error("x-sendfile: File '".. xs .. "'not in required docroot '" .. docroot .. "'")
			return lighty.HANDLER_GO_ON
		end

		-- make sure to drop all other content from the backend
		local f = add_drop_filter(vr)

		vr.resp.headers["X-Sendfile"] = nil -- remove header from response

		-- Add checks for the pathname here

		vr:debug("XSendfile:handle: pushing file '" .. xs .. "' as content")
		f.out:add({ filename = xs })
	end
end

actions = handle_x_sendfile
