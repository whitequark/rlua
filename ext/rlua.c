#include <ruby.h> 
#include <lua5.1/lua.h>

static VALUE rlua_get_var(lua_State *state)
{
  VALUE hash;
  int index;
  const char* string;
  size_t length;
  
  switch(lua_type(state, -1)) {
  	case LUA_TNONE:
  	case LUA_TNIL:
	    return Qnil;
	  
  	case LUA_TFUNCTION:
	    rb_warn("cannot pop LUA_TFUNCTION");
	    return Qnil;
	  
  	case LUA_TTHREAD:
	    rb_warn("cannot pop LUA_TTHREAD");
	    return Qnil;
	  
  	case LUA_TUSERDATA:
	    rb_warn("cannot pop LUA_TUSERDATA");
	    return Qnil;
	  
	  case LUA_TBOOLEAN:
	    return lua_toboolean(state, -1) ? Qtrue : Qfalse;

  	case LUA_TNUMBER:
	    return rb_float_new(lua_tonumber(state, -1));

  	case LUA_TSTRING:
  	  string = lua_tolstring(state, -1, &length);
	    return rb_str_new(string, length);
    
  	case LUA_TTABLE:
	    hash = rb_hash_new();
	    
	    index = lua_gettop(state);
	    lua_pushnil(state);
	    
	    while(lua_next(state, index) != 0) {
    		VALUE value = rlua_get_var(state);
		    lua_pushvalue(state, -2);
    		rb_hash_aset(hash, rb_str_new2(lua_tostring(state, -1)), value);
		    lua_pop(state, 2);
	    }
	    
	    lua_settop(state, index);
	    return hash;

  	case LUA_TLIGHTUSERDATA:
      return (VALUE) lua_touserdata(state, -1);
    
    default:
      rb_bug("rlua_get_var: unknown type %s", lua_typename(state, -1));
  }
}

static void rlua_push_var(lua_State *state, VALUE value)
{
  int table, i;
  VALUE keys;
  
  switch (TYPE(value)) {
    case T_NIL:
      lua_pushnil(state);
      break;

    case T_STRING:
	    lua_pushlstring(state, RSTRING(value)->ptr, RSTRING(value)->len); 
	    break;
  	
  	case T_FIXNUM:
	    lua_pushnumber(state, FIX2INT(value)); 
	    break;
  	
  	case T_BIGNUM:
  	case T_FLOAT:
 	    lua_pushnumber(state, NUM2DBL(value)); 
	    break;
  	
  	case T_ARRAY:
	    lua_newtable(state);
	    table = lua_gettop(state);
 	    for(i = 0; i < RARRAY(value)->len; i++) {
    		rlua_push_var(state, RARRAY(value)->ptr[i]);
		    lua_rawseti(state, table, i);
      }
	    break;
    
    case T_HASH:
	    lua_newtable(state);
	    keys = rb_funcall(value, rb_intern("keys"), 0);
	    for(i = 0; i <= RARRAY(keys)->len - 1; i++) {
	      VALUE key = RARRAY(keys)->ptr[i];
        lua_pushlstring(state, RSTRING(key)->ptr, RSTRING(key)->len); 
        rlua_push_var(state, rb_hash_aref(value, key));
        lua_settable(state, -3);
      }
	    break;

	  default:
	    lua_pushlightuserdata(state, (void*) value);
    }
}

static void rlua_load_string(lua_State* state, VALUE code)
{
  Check_Type(code, T_STRING);
  int retval = luaL_loadstring(state, RSTRING(code)->ptr);
  if(retval == LUA_ERRMEM) {
    rb_raise(rb_eNoMemError, "cannot load Lua code", RSTRING(code)->ptr);
  } else if(retval == LUA_ERRSYNTAX) {
    rb_raise(rb_eSyntaxError, "cannot load Lua code", RSTRING(code)->ptr);
  }
}

static VALUE rlua_pcall(lua_State* state, int argc)
{
  int retval = lua_pcall(state, argc, LUA_MULTRET, 0);
  if(retval != 0) {
    const char* error = lua_tostring(state, -1);
    lua_pop(state, 1);
    if(retval == LUA_ERRRUN)
      rb_raise(rb_eRuntimeError, "Lua error: %s", error);
    else if(retval == LUA_ERRMEM)
      rb_raise(rb_eNoMemError, "Lua error: %s", error);
    else
      rb_fatal("unknown lua_pcall return value");
  } else {
    VALUE retval;
    int n = lua_gettop(state);
    if(n == 0) {
      return Qnil;
    } else if(n == 1) {
      retval = rlua_get_var(state);
      lua_pop(state, 1);
      return retval;
    } else if(n > 1) {
      retval = rb_ary_new();
      while(n--) {
        rb_ary_unshift(retval, rlua_get_var(state));
        lua_pop(state, 1);
      }
    }
    return retval;
  }
}

static VALUE rbLua_initialize(VALUE self)
{
  lua_State* state = luaL_newstate();
  VALUE rbState = Data_Wrap_Struct(rb_cObject, 0, lua_close, state);
  rb_iv_set(self, "@state", rbState);
  return self;
}

static VALUE rbLua_eval(VALUE self, VALUE code)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  rlua_load_string(state, code);
  return rlua_pcall(state, 0);
}

static VALUE rbLua_call(int argc, VALUE* argv, VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE func, args;
  int i;
  rb_scan_args(argc, argv, "1*", &func, &args);
  Check_Type(func, T_STRING);
  lua_getglobal(state, RSTRING(func)->ptr);
  for(i = 0; i < RARRAY(args)->len; i++)
    rlua_push_var(state, RARRAY(args)->ptr[i]);
  return rlua_pcall(state, RARRAY(args)->len);
}

static VALUE rbLua_get(VALUE self, VALUE name)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  Check_Type(name, T_STRING);
  
  lua_getglobal(state, RSTRING(name)->ptr);
  VALUE retval = rlua_get_var(state);
  lua_pop(state, 1);
  return retval;
}

static VALUE rbLua_set(VALUE self, VALUE name, VALUE value)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  Check_Type(name, T_STRING);
  
  rlua_push_var(state, value);
  lua_setglobal(state, RSTRING(name)->ptr);
  return self;
}

static int call_ruby_proc(lua_State* state)
{
  int i;
  int argc = lua_gettop(state);
  VALUE proc, args;
  
  proc = (VALUE) lua_touserdata(state, lua_upvalueindex(1));
  args = rb_ary_new();
  
  for(i = 0; i < argc; i++) {
    rb_ary_unshift(args, rlua_get_var(state));
    lua_pop(state, 1);
  }
  
  rb_apply(proc, rb_intern("call"), args);
}

static VALUE rbLua_attach(VALUE self, VALUE name, VALUE proc)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  Check_Type(name, T_STRING);
  
  lua_pushlightuserdata(state, (void*) proc);
  lua_pushcclosure(state, call_ruby_proc, 1);
  lua_setglobal(state, RSTRING(name)->ptr);
  
  return Qnil;
}

void Init_rlua()
{
  VALUE cLua;
  cLua = rb_define_class("Lua", rb_cObject);
  rb_define_method(cLua, "initialize", rbLua_initialize, 0);
  rb_define_method(cLua, "eval", rbLua_eval, 1);
  rb_define_method(cLua, "call", rbLua_call, -1);
  rb_define_method(cLua, "get", rbLua_get, 1);
  rb_define_method(cLua, "set", rbLua_set, 2);
  rb_define_method(cLua, "attach", rbLua_attach, 2);
}
