require 'bundler/gem_tasks'
require 'rdoc/task'

Rake::RDocTask.new do |rd|
  rd.main        = 'README.rdoc'
  rd.title       = 'RLua Documentation'
  rd.rdoc_files  = Dir['*.rdoc', 'lib/*.rb', 'ext/*.c'].to_a
  rd.rdoc_dir    = 'doc'
  rd.options    += ['--format=hanna']
end