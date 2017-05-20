#!/usr/bin/ruby

require 'rubygems'
require 'nokogiri'

require 'redcloth'

require 'cgi'

# find all options, actions, setups in the c modules - can be used to check list for completeness (although it doesn't scan the lua modules):
# awk '/static const liPluginOption/, /;/ {print; }' src/*/*.c | grep '"' | cut -d'"' -f2 | perl -e 'print sort <>;'
# awk '/static const liPluginAction/, /;/ {print; }' src/*/*.c | grep '"' | cut -d'"' -f2 | perl -e 'print sort <>;'
# awk '/static const liPluginSetup/, /;/ {print; }' src/*/*.c | grep '"' | cut -d'"' -f2 | perl -e 'print sort <>;'

HTML_TEMPLATE='''
<!DOCTYPE html>
<html>
<head>
	<meta http-equiv="Content-Type" content="text/html;charset=utf-8">
	<title>Title</title>
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<link rel="stylesheet" href="bootstrap.min.css">
	<link rel="stylesheet" href="bootstrap-theme.min.css">
	<link rel="stylesheet" href="style.css">
	<script src="jquery-1.10.1.min.js"></script>
	<script src="bootstrap.min.js"></script>
</head>
<body data-spy="scroll" data-target=".bs-sidebar" data-offset="30"><div class="container"><div class="row">

	<!-- TOC -->
	<div class="col-md-3" role="complementary"><div class="bs-sidebar hidden-print toc" id="sidebar" data-spy="affix" data-offset-top="0" data-offset-bottom="30"></div></div>

	<!-- MAIN -->
	<div class="col-md-9" role="main" id="main"></div></div>

</div></div></body>
</html>
'''

class Documentation
	XPATH_NAMESPACES = { 'd' => 'urn:lighttpd.net:lighttpd2/doc1' }
	TAB_WIDTH = 4

	def initialize(basename)
		@html_doc = Nokogiri::HTML::Document.parse(HTML_TEMPLATE)
		@html_main = @html_doc.xpath('//div[@role="main"]')[0]
		@html_toc = @html_doc.css('#sidebar')[0]
		@title = nil

		@depth = 0
		@uniqueid = 0
		@current_toc = @toc = []

		@basename = basename

		@actions = []
		@setups = []
		@options = []

		@sub_pages = []
	end

	def render_main
		Nokogiri::XML::Builder.with(@html_main) do |html|
			@html = html
			yield
			@html = nil
		end
	end

	def title
		@title
	end

	def title=(value)
		@title = value
		@html_doc.xpath('/html/head/title')[0].content = value ? 'lighttpd2 - ' + value : 'lighttpd2'
	end

	def to_html_fragment
		@html_main.inner_html
	end

	def to_html
		@html_doc.to_html
	end

	def toc
		@toc
	end

	def actions
		@actions
	end

	def setups
		@setups
	end

	def options
		@options
	end

	def sub_pages
		@sub_pages
	end

	def _store_toc(html, toc, rootToc = false)
		return unless toc.length > 0
		html.ul(:class => rootToc ? "nav bs-sidenav" : "nav" ) {
			if rootToc and @basename != 'index' and @basename != 'all'
				html.li(:class => 'index') {
					html.a({:href => 'index.html'}, 'Index')
				}
			end
			toc.each do |anchor, title, subtoc, cls|
				html.li(:class => cls || '') {
					html.a({:href => '#' + anchor}, title)
					_store_toc(html, subtoc)
				}
			end
		}
	end

	def store_toc
		Nokogiri::HTML::Builder.with(@html_toc) do |html|
			_store_toc(html, @toc, true)
		end
	end

	def _unique_anchor(title)
		id = @uniqueid
		@uniqueid += 1
		("%02x-" % id) + title.downcase.gsub(/[^a-z0-9]+/, '_')
	end

	# although '.' and ':' are allowed in anchors (IDs), they
	# don't work in CSS selectors like: #anchor:foo and #anchor.bar
	def escape_anchor(anchor)
		anchor.gsub(/[.:]/, '-')
	end

	def nest(title, anchor = nil, cls = nil)
		attribs = {}
		have_toc = nil != @current_toc
		anchor = (@depth < 3 and have_toc) ? _unique_anchor(title) : nil if anchor == '#'
		if anchor
			anchor = (anchor == '') ? @basename : @basename + '__' + anchor
			anchor = escape_anchor(anchor)
			attribs[:id] = anchor
		end

		use_toc = have_toc && anchor
		old_toc = @current_toc
		@current_toc = use_toc ? [] : nil

		@depth += 1
		if cls
			@html.div(:class => cls) {
				@html.send("h#{@depth}", attribs, title)
				yield
			}
		else
			@html.send("h#{@depth}", attribs, title)
			yield
		end
		@depth -= 1

		old_toc << [ anchor, title, @current_toc, cls ] if use_toc
		@current_toc = old_toc
		return anchor
	end

	## Code formatting
	def _count_indent(line)
		/^[ \t]*/.match(line)[0].each_char.reduce(0) do |i, c|
			c == ' ' ? i + 1 : (i + TAB_WIDTH) - (i % TAB_WIDTH)
		end
	end
	def _remove_indent(indent, line)
		p = 0
		l = line.length
		i = 0
		while i < indent && p < l
			case line[p]
			when ' '
				i += 1
			when "\t"
				i = (i + TAB_WIDTH) - (i % TAB_WIDTH)
			else
				return line[p..-1]
			end
			p += 1
		end
		return line[p..-1]
	end
	def _get_cdata(xml)
		if xml.cdata?
			return xml.content
		else
			c = xml.children
			if 1 == c.length and c[0].cdata?
				return c[0].content
			else
				return CGI::unescapeHTML(xml.inner_html)
			end
		end
	end
	def _format_code(xml)
		code = _get_cdata(xml)
		lines = code.rstrip.lines
		real_lines = lines.grep(/\S/)
		return '' if real_lines.length == 0

		indent = real_lines.map { |l| _count_indent l }.min

		code = lines.map { |line| _remove_indent(indent, line).rstrip }.join("\n") + "\n"
		code.gsub(/\A\n+/, "").gsub(/\n\n+/, "\n\n").gsub(/\n+\Z/, "\n")
	end

	def _parse_code(xml)
		@html.pre { @html.code { @html << _format_code(xml) } }
	end

	def _parse_textile(xml)
		tx = _format_code(xml)
		@html << RedCloth.new(tx).to_html
	end

	def _parse_html(xml)
		@html << xml.inner_html
	end

	def _parse_description(xmlParent)
		return unless xmlParent
		xml = xmlParent.xpath('d:description[1]', XPATH_NAMESPACES)[0]
		return unless xml

		xml.children.each do |child|
			if child.text?
				@html.p child.content.strip
			elsif ['html','textile'].include? child.name
				self.send('_parse_' + child.name, child)
			else
				raise 'invalid description element ' + child.name
			end
		end
	end

	def <=>(other)
		ordername <=> other.ordername
	end

	def ordername=(value)
		@ordername = value
	end

	def ordername
		@ordername || @basename
	end

	def basename
		@basename
	end

	def filename
		basename + '.html'
	end

	def write_disk(output_directory)
		puts "Writing #{output_directory}: #{filename}"
		File.open(File.join(output_directory, self.filename), "w") { |f| f.write self.to_html }
	end
end

class GenericModuleDocumentation < Documentation
	def initialize(filename, xml)
		super(File.basename(filename, '.xml'))
		self.title = basename unless self.title

		render_main { _parse_module(xml.root) }

		store_toc
	end

	def link(html_builder)
		html_builder.a({:href => self.filename + '#' + escape_anchor(self.basename)}, self.title)
	end

	def short
		@short
	end

	def _parse_short(xmlParent, makeDiv = false)
		return unless xmlParent
		xml = xmlParent.xpath('d:short[1]', XPATH_NAMESPACES)[0]
		return unless xml
		text = xml.content.strip
		return unless text

		if makeDiv
			@html.p.short text
		else
			@html.text text
		end
		text
	end

	def _parse_parameters(xml)
		@html.dl {
			xml.xpath('d:parameter', XPATH_NAMESPACES).each do |param|
				@html.dt param['name']
				child = param.element_children[0]
				if child.name == 'short'
					@html.dd { _parse_short param }
				elsif child.name == 'table'
					@html.dd {
						@html.text "A key-value table with the following entries:"
						@html.dl {
							child.element_children.each do |entry|
								@html.dt entry['name']
								@html.dd { _parse_short entry }
							end
						}
					}
				end
			end
		}
	end

	def _parse_default(xml)
		@html.div(:class => 'default') {
			@html.text "Default value: "

			child = xml.element_children[0]
			@html.span({:class => child.name}, child.content.strip)
		}
	end

	def _parse_example(xml)
		nest(xml['title'] || 'Example', xml['anchor'], 'example') {
			_parse_description(xml)

			config = xml.xpath('d:config[1]', XPATH_NAMESPACES)
			_parse_code(config[0])
		}
	end

	def _parse_item(xml, type, anchor_prefix = nil, title_suffix = nil)
		name = xml['name']
		raise "#{type} requires a name" unless name
		parameter_names = xml.xpath('d:parameter', XPATH_NAMESPACES).map { |p| p['name'] }
		parameter_names = ['value'] if parameter_names.length == 0 and type == 'option'

		anchor_prefix ||= "#{type}_"
		title_suffix ||= " (#{type})"
		title = "#{name}#{title_suffix}"
		anchor = "#{anchor_prefix}#{name}"
		cls = "aso #{type}"
		short = nil

		anchor = nest(title, anchor, cls) {
			short = _parse_short(xml, true)

			@html.pre(:class => "template") {
				@html.span.key name
				if parameter_names.length == 1
					@html.text ' '
					@html.span.param parameter_names[0]
					@html.text ';'
				elsif parameter_names.length > 0
					@html.text ' ('
					first = true
					parameter_names.each do |pname|
						@html.text ', ' unless first
						first = false
						@html.span.param pname
					end
					@html.text ');'
				else
					@html.text ';'
				end
			}

			if type == 'option'
				_parse_default(xml.xpath('d:default', XPATH_NAMESPACES)[0])
			else
				_parse_parameters(xml) if parameter_names.length > 0
			end

			_parse_description(xml)
			xml.xpath('d:example', XPATH_NAMESPACES).each do |child|
				_parse_example(child)
			end
		}

		[name, filename + '#' + anchor, short, self]
	end

end


class ModuleDocumentation < GenericModuleDocumentation
	def initialize(filename, xml)
		super(filename, xml)
	end

	def _parse_action(xml)
		@actions << _parse_item(xml, 'action')
	end

	def _parse_setup(xml)
		@setups << _parse_item(xml, 'setup')
	end 

	def _parse_option(xml)
		@options << _parse_item(xml, 'option')
	end

	def _parse_section(xml)
		title = xml['title']
		raise 'section requires a title' unless title

		nest(title, xml['anchor'] || '#', 'section') {
			xml.children.each do |child|
				if child.text?
					text = child.content.strip
					@html.p text if text.length > 0
				elsif ['action','setup','option','html','textile','example','section'].include? child.name
					self.send('_parse_' + child.name, child)
				else
					raise 'invalid section element ' + child.name
				end
			end
		}
	end

	def _parse_module(xml)
		raise 'unexpected root node' if xml.name != 'module'

		self.title = xml['title'] || self.title
		self.ordername = xml['order']

		nest(title, '', 'module') {
			@html.p {
				@html.text (basename + ' ')
				@short = _parse_short(xml, false)
			}
			_parse_description(xml)

			xml.element_children.each do |child|
				if ['action','setup','option','example','section'].include? child.name
					self.send('_parse_' + child.name, child)
				elsif ['short', 'description'].include? child.name
					nil # skip
				else
					raise 'invalid module element ' + child.name
				end
			end
		}
	end
end

class AngelModuleDocumentation < GenericModuleDocumentation
	def initialize(filename, xml)
		@items = []
		super(filename, xml)
	end

	def _parse_item(xml)
		@items << super(xml, 'item', '', '')
	end

	def _parse_section(xml)
		title = xml['title']
		raise 'section requires a title' unless title

		nest(title, xml['anchor'] || '#', 'section') {
			xml.children.each do |child|
				if child.text?
					text = child.content.strip
					@html.p text if text.length > 0
				elsif ['item','html','textile','example','section'].include? child.name
					self.send('_parse_' + child.name, child)
				else
					raise 'invalid section element ' + child.name
				end
			end
		}
	end

	def _parse_module(xml)
		raise 'unexpected root node' if xml.name != 'angel-module'

		self.title = xml['title'] || self.title
		self.ordername = xml['order']

		nest(title, '', 'angel-module') {
			if 'core_config_angel' != basename then
				@html.p {
					@html.text (basename + ' ')
					@short = _parse_short(xml, false)
				}
			end
			_parse_description(xml)

			xml.element_children.each do |child|
				if ['item','example','section'].include? child.name
					self.send('_parse_' + child.name, child)
				elsif ['short', 'description'].include? child.name
					nil # skip
				else
					raise 'invalid module element ' + child.name
				end
			end
		}
	end
end

class ChapterDocumentation < Documentation
	def initialize(filename, xml)
		super(File.basename(filename, '.xml'))

		render_main { _parse_chapter(xml.root) }

		store_toc
	end

	def _parse_example(xml)
		nest(xml['title'] || 'Example', xml['anchor'], 'example') {
			_parse_description(xml)

			config = xml.xpath('d:config[1]', XPATH_NAMESPACES)
			_parse_code(config[0])
		}
	end

	def _parse_section(xml)
		title = xml['title']
		raise 'section requires a title' unless title

		nest(title, xml['anchor'] || '#', 'section') {
			xml.children.each do |child|
				if child.text?
					text = child.content.strip
					@html.p text if text.length > 0
				elsif ['html','textile','example','section'].include? child.name
					self.send('_parse_' + child.name, child)
				else
					raise 'invalid section element ' + child.name
				end
			end
		}
	end

	def _parse_chapter(xml)
		raise 'unexpected root node' if xml.name != 'chapter'

		self.title = xml['title']
		raise 'chapter requires a title' unless self.title
		self.ordername = xml['order']

		nest(self.title, '', 'chapter') {
			_parse_description(xml)

			xml.element_children.each do |child|
				if ['example','section'].include? child.name
					self.send('_parse_' + child.name, child)
				elsif ['description'].include? child.name
					nil # skip
				else
					raise 'invalid chapter element ' + child.name
				end
			end
		}
	end
end

class ModuleIndex < Documentation
	def modules_table(modules)
		nest('Modules', 'modules') {
			@html.table(:class => 'table table-striped') {
				@html.tr {
					@html.th "name"
					@html.th "description"
				}
				modules.each do |mod|
					next unless mod.is_a? ModuleDocumentation
					@html.tr {
						@html.td {
							mod.link(@html)
						}
						@html.td mod.short
					}
				end
			}

		}
	end

	def aso_html_table(name, list)
		nest(name.capitalize + 's', name + 's') {
			@html.table(:class => 'table table-striped aso') {
				@html.tr {
					@html.th "name"
					@html.th "module"
					@html.th "description"
				}
				list.each do |id, href, short, mod|
					@html.tr {
						@html.td {
							@html.a({:href => href}, id)
						}
						@html.td {
							mod.link(@html)
						}
						@html.td short
					}
				end
			}
			@html.text "none" unless list.length > 0
		}
	end

	def initialize(modules)
		super('index_modules')

		@sub_pages = modules

		actions = []
		setups = []
		options = []
		modules.each do |mod|
			actions += mod.actions
			setups += mod.setups
			options += mod.options
		end

		render_main do
			nest('Module index', '', 'index_modules') {
				modules_table(modules)
				aso_html_table('action', actions.sort)
				aso_html_table('setup', setups.sort)
				aso_html_table('option', options.sort)
			}
		end

		self.title = "Module index"
		store_toc
	end
end

class IndexPage < Documentation
	def list(pages)
		return if pages.empty?
		@html.ul do
			pages.each do |page|
				@html.li do
					@html.a({:href => page.filename}, page.title)
					list(page.sub_pages)
				end
			end
		end
	end


	def initialize(pages)
		super('index')

		self.title = "Index"

		render_main do
			nest(self.title, '', 'index') do
				@html.p do
					@html << "The documentation is also available as a "
					@html.a({:href => "all.html"}, "single HTML page")
					@html << "."
				end
				list(pages)
			end
		end

		store_toc
	end
end

class AllPage < Documentation
	def fix_link(a)
		href = a['href']
		return unless href
		m = /^([^#]*)(#.*)$/.match(href)
		a['href'] = m[2] if m && @href_map[m[1]]
	end

	def append(pages)
		pages.each do |page|
			@href_map[page.filename] = true
			@html << page.to_html_fragment
			@toc += page.toc
			append(page.sub_pages)
		end
	end

	def initialize(pages)
		super('all')

		@href_map = {}

		render_main do
			append(pages)
		end

		@html_doc.xpath('//a').each do |a|
			fix_link(a)
		end

		self.title = "all in one"
		store_toc
	end
end


def loadXML(filename)
	xml = Nokogiri::XML(File.read filename) do |config|
		config.strict.nonet
	end

	if xml.root.name == 'module'
		ModuleDocumentation.new(filename, xml)
	elsif xml.root.name == 'angel-module'
		AngelModuleDocumentation.new(filename, xml)
	elsif xml.root.name == 'chapter'
		ChapterDocumentation.new(filename, xml)
	end
end


if __FILE__ == $0
	output_directory = ARGV[0] || '.'

	if not system("xmllint --noout --schema doc_schema.xsd *.xml 2>&1")
		STDERR.puts "Couldn't validate XML files"
		exit 1
	end

	pages = []

	Dir["*.xml"].each do |file|
		puts "Compiling #{file}"
		pages << loadXML(file)
	end

	pages.sort!

	normal_pages = []
	module_pages = []

	pages.each do |page|
		if page.is_a? ModuleDocumentation
			module_pages << page
		else
			normal_pages << page
		end
	end

	module_index_page = ModuleIndex.new(module_pages)

	pages << module_index_page
	normal_pages << module_index_page

	pages.sort!
	all_page = AllPage.new(normal_pages)
	index_page = IndexPage.new(normal_pages)

	pages << all_page << index_page

	pages.sort!
	pages.each { |page| page.write_disk(output_directory) }
end
