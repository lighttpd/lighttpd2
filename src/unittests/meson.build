unittests = {
  'Chunk-UnitTest': {
    'binary': 'test-chunk',
    'sources': ['test-chunk.c'],
  },
  'HttpRequestParser-UnitTest': {
    'binary': 'test-http-request-parser',
    'sources': ['test-http-request-parser.c'],
  },
  'IpParser-UnitTest': {
    'binary': 'test-ip-parser',
    'sources': ['test-ip-parser.c'],
  },
  'Radix-UnitTest': {
    'binary': 'test-radix',
    'sources': ['test-radix.c'],
  },
  'RangeParser-UnitTest': {
    'binary': 'test-range-parser',
    'sources': ['test-range-parser.c'],
  },
  'Utils-UnitTest': {
    'binary': 'test-utils',
    'sources': ['test-utils.c'],
  },
}

foreach name, def: unittests
  test_bin = executable(
    def['binary'],
    def['sources'],
    include_directories: inc_dir,
    dependencies: main_deps + def.get('dependencies', []),
    link_with: [
      lib_shared,
      lib_common,
    ],
    build_by_default: false,
  )
  test(
    name,
    test_bin,
    protocol: 'tap',
  )
endforeach
