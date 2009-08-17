require 'rubygems'
require 'rake/gempackagetask'

spec = Gem::Specification.new do |s| 
  s.name = "rlua"
  s.version = "0.9"
  s.author = "Peter Zotov"
  s.email = "whitequark@whitequark.ru"
  s.platform = Gem::Platform::RUBY
  s.summary = "A simple Ruby-to-Lua glue library"
  s.files = FileList["ext/*"].to_a
  s.extensions = 'ext/extconf.rb'
end
 
Rake::GemPackageTask.new(spec) do |pkg| 
end
 
