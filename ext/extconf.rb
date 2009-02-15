#!/usr/bin/ruby
require "mkmf"

dir_config('lua')

unless have_library('lua', 'luaL_newstate')
  puts 'need liblua5.1'
  exit 1
end

create_header()
create_makefile("rlua")
