#!/usr/bin/ruby
require "mkmf"

dir_config('lua5.1')

unless have_library('lua5.1', 'luaL_newstate')
  puts 'need liblua5.1'
  exit 1
end

create_header()
create_makefile("rlua")
