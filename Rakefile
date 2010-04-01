require 'rubygems'
require 'rake/gempackagetask'
require 'rake/rdoctask'

RLUA_VERSION = "1.0rc1"
RDOC_OPTIONS = ['-S', '-N', '--main=README.rdoc', '--title=RLua Documentation']
RDOC_FILES = FileList['*.rdoc', 'lib/*', 'ext/rlua.c'].to_a

spec = Gem::Specification.new do |s|
  s.name = "rlua"
  s.version = RLUA_VERSION
  s.author = "Peter Zotov"
  s.email = "whitequark@whitequark.ru"
  s.platform = Gem::Platform::RUBY
  s.summary = "Ruby to Lua bindings."
  s.description = <<-EOD
  Fully functional, almost complete Ruby to Lua binding library that
  features seamless translation of most Lua and Ruby objects and calling
  code of each language from other one.
  EOD
  s.files = FileList["ext/*.c", "lib/*"].to_a
  s.extensions = 'ext/extconf.rb'
  s.rubyforge_project = 'rlua'
  s.homepage = 'http://rlua.rubyforge.org'
  s.extra_rdoc_files = RDOC_FILES
  s.rdoc_options = RDOC_OPTIONS
end

Rake::GemPackageTask.new(spec) do |pkg|
  pkg.need_tar_bz2 = true
end

Rake::RDocTask.new do |rd|
  rd.rdoc_files = RDOC_FILES
  rd.rdoc_dir = 'doc'
  rd.options += RDOC_OPTIONS
end
