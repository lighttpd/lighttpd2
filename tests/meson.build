runtest_file = files('runtests.py')

test(
  'http',
  runtest_file,
  args: [
    '--angel', bin_angel.full_path(),
    '--worker', bin_worker.full_path(),
    '--plugindir', modules_build_dir,
  ],
  depends: [
    bin_angel,
    bin_worker,
    enabled_modules,
  ],
)
