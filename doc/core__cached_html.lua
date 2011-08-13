local function try_cached_html(vr)
	local p = vr.phys.path
	if p:sub(-1) == '/' then
		p = p:sub(0, -2) .. '.html'
	else
		p = p .. '.html'
	end
	st, res = vr:stat(p)
	if st and st.is_file then
		-- found the file
		vr.phys.path = p
	elseif res == lighty.HANDLER_WAIT_FOR_EVENT then
		return lighty.HANDLER_WAIT_FOR_EVENT
	end
	-- ignore other errors
end

actions = try_cached_html
