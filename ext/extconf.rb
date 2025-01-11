require "mkmf"

$CFLAGS += " -std=c99 -Wno-declaration-after-statement"

dir_config('lua5.4')

unless have_library('lua5.3', 'luaL_newstate') or have_library('lua.5.3', 'luaL_newstate')
  puts ' extconf failure: need liblua5.3'
  exit 1
end

create_makefile("rlua")
