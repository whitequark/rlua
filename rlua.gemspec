Gem::Specification.new do |gem|
  gem.name          = "rlua"
  gem.version       = '1.1.rc1'
  gem.authors       = ["Peter Zotov"]
  gem.email         = ["whitequark@whitequark.org"]
  gem.description   = %q{Ruby to Lua bindings}
  gem.summary       = <<-EOD
  Fully functional, almost complete Ruby to Lua binding library that
  features seamless translation of most Lua and Ruby objects and calling
  code of each language from other one.
  EOD
  gem.homepage      = "http://whitequark.github.com/rlua/"

  gem.files         = `git ls-files`.split($/)
  gem.executables   = gem.files.grep(%r{^bin/}).map{ |f| File.basename(f) }
  gem.test_files    = gem.files.grep(%r{^(test|spec|features)/})
  gem.require_paths = ["lib"]

  gem.add_development_dependency 'rdoc'
  gem.add_development_dependency 'rake'
  gem.add_development_dependency 'hanna-nouveau'

  gem.extra_rdoc_files = Dir['*.rdoc', 'ext/*.c'].to_a
  gem.rdoc_options     = ['--main=README.rdoc', '--title=RLua Documentation']
end