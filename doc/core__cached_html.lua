local function try_cached_html(vr)
	local p = vr.phys.path .. '.html'
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
