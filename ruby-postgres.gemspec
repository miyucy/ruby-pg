require 'rubygems'
require 'date'

SPEC = Gem::Specification.new do |s|
  s.name              = 'ruby-postgres'
  s.rubyforge_project = 'ruby-postgres'
  s.version           = "0.7.1.#{Date.today}".tr('-', '.')
  s.summary           = 'Ruby extension library providing an API to PostgreSQL'
  s.authors           = ['Yukihiro Matsumoto', 'Eiji Matsumoto', 'Noboru Saitou', 'Dave Lee']
  s.email             = 'davelee.com@gmail.com'
  s.homepage          = 'http://ruby.scripting.ca/postgres/'
  s.requirements      = 'PostgreSQL libpq library and headers'
  s.has_rdoc          = true
  s.require_path      = '.'
  s.autorequire       = 'postgres'

  if File.exists? 'postgres.so' and PLATFORM =~ /mingw|mswin/
    s.platform        = Gem::Platform::WIN32
  else
    s.platform        = Gem::Platform::RUBY
    s.extensions      = 'extconf.rb'
  end

  if File.exists? '_darcs'
    s.files           = Dir.chdir('_darcs/current') { Dir['**/*'] }
  else
    s.files           = Dir['**/*']
  end
  s.files.reject { |file| file =~ /\.gem$/ }

end

if $0 == __FILE__
  Gem::manage_gems
  Gem::Builder.new(SPEC).build
end
