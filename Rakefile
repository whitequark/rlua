require 'rubygems'
require 'rake/gempackagetask'
require 'rake/rdoctask'

RLUA_VERSION = "0.1rc1"

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
  of each language code from others.
  EOD
  s.files = FileList["ext/*.c", "lib/*"].to_a
  s.extensions = 'ext/extconf.rb'
end

Rake::GemPackageTask.new(spec) do |pkg|
end

Rake::RDocTask.new do |rd|
  rd.rdoc_files.include("README.rdoc", "lib/*.rb", "ext/rlua.c")
  rd.options << '--inline-source'
  rd.main = 'README.rdoc'
end
