#include <ruby.h> 
#include <lua5.1/lua.h>
#include <lua5.1/lauxlib.h>

VALUE mLua, cLuaState, cLuaMultret, cLuaFunction, cLuaTable;

static VALUE rlua_makeref(lua_State* state)
{
  VALUE ref;
                                                     // stack: |objt|...
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");    //        |refs|objt|...
  lua_pushvalue(state, -2);                          //        |objt|refs|objt|...
  ref = INT2FIX(luaL_ref(state, -2));                //        |refs|objt|...
  lua_pop(state, 1);                                 //        |objt|...

  return ref;
}

static VALUE rlua_finalize_ref(VALUE id, VALUE rbState)
{
  lua_State* state;
  Data_Get_Struct(rbState, lua_State, state);
  
  int ref = FIX2INT(rb_hash_aref(rb_iv_get(rbState, "@refs"), id));
  
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");
  luaL_unref(state, -1, ref);
  lua_pop(state, 1);
  
  rb_hash_delete(rb_iv_get(rbState, "@refs"), id);
  
  return Qnil;
}

static void rlua_add_ref_finalizer(VALUE state, VALUE ref, VALUE object)
{
  rb_hash_aset(rb_iv_get(state, "@refs"), rb_obj_id(object), ref);

  VALUE mObjectSpace = rb_const_get(rb_cObject, rb_intern("ObjectSpace"));
  VALUE proc = rb_proc_new(rlua_finalize_ref, state);
  rb_funcall(mObjectSpace, rb_intern("define_finalizer"), 2, object, proc);
}

static VALUE rlua_get_var(lua_State *state)
{
  VALUE rbLuaState;
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua_state");
  rbLuaState = (VALUE) lua_touserdata(state, -1);
  lua_pop(state, 1);

  switch(lua_type(state, -1)) {
    case LUA_TNONE:
    case LUA_TNIL:
      return Qnil;

    case LUA_TTHREAD:
      rb_warn("cannot pop LUA_TTHREAD");
      return Qnil;

    case LUA_TUSERDATA:
      rb_warn("cannot pop LUA_TUSERDATA");
      return Qnil;

    case LUA_TLIGHTUSERDATA:
      rb_warn("cannot pop LUA_TLIGHTUSERDATA");
      return Qnil;
    
    case LUA_TBOOLEAN:
      return lua_toboolean(state, -1) ? Qtrue : Qfalse;

    case LUA_TNUMBER:
      return rb_float_new(lua_tonumber(state, -1));

    case LUA_TSTRING: {
      size_t length;
      const char* string;
      string = lua_tolstring(state, -1, &length);
      return rb_str_new(string, length);
    }

    case LUA_TTABLE:
      return rb_funcall(cLuaTable, rb_intern("new"), 2, rbLuaState, rlua_makeref(state));

    case LUA_TFUNCTION:
      return rb_funcall(cLuaFunction, rb_intern("new"), 2, rbLuaState, rlua_makeref(state));

    default:
      rb_bug("rlua_get_var: unknown type %s", lua_typename(state, lua_type(state, -1)));
  }
}

static void rlua_push_var(lua_State *state, VALUE value)
{
  switch (TYPE(value)) {
    case T_NIL:
      lua_pushnil(state);
      break;

    case T_STRING:
      lua_pushlstring(state, RSTRING_PTR(value), RSTRING_LEN(value)); 
      break;

    case T_FIXNUM:
      lua_pushnumber(state, FIX2INT(value)); 
      break;

    case T_BIGNUM:
    case T_FLOAT:
       lua_pushnumber(state, NUM2DBL(value)); 
       break;

    case T_ARRAY: {
      int table, i;

      lua_newtable(state);
      table = lua_gettop(state);
       for(i = 0; i < RARRAY_LEN(value); i++) {
         rlua_push_var(state, RARRAY_PTR(value)[i]);
         lua_rawseti(state, table, i+1);
      }
      break;
    }

    case T_HASH: {
      int i;
      VALUE keys;

      lua_newtable(state);
        keys = rb_funcall(value, rb_intern("keys"), 0);
        for(i = 0; i < RARRAY_LEN(keys); i++) {
          VALUE key = RARRAY_PTR(keys)[i];
        lua_pushlstring(state, RSTRING_PTR(key), RSTRING_LEN(key)); 
        rlua_push_var(state, rb_hash_aref(value, key));
        lua_settable(state, -3);
      }
      break;
    }

    default:
      if(value == Qtrue || value == Qfalse) {
        lua_pushboolean(state, value == Qtrue);
      } else if(rb_obj_class(value) == cLuaTable || rb_obj_class(value) == cLuaFunction) {
        lua_getfield(state, LUA_REGISTRYINDEX, "rlua");              // stack: |refs|...
        lua_rawgeti(state, -1, FIX2INT(rb_iv_get(value, "@ref")));   //        |objt|refs|...
        lua_remove(state, -2);                                       //        |objt|...
      } else if(rb_respond_to(value, rb_intern("call"))) {
        VALUE rbLuaState;
        lua_getfield(state, LUA_REGISTRYINDEX, "rlua_state");
        rbLuaState = (VALUE) lua_touserdata(state, -1);
        lua_pop(state, 1);

        rlua_push_var(state, rb_funcall(cLuaFunction, rb_intern("new"), 2, rbLuaState, value));
      } else {
        rb_raise(rb_eTypeError, "wrong argument type %s", rb_obj_classname(value));
      }
  }
}

static void rlua_load_string(lua_State* state, VALUE code)
{
  Check_Type(code, T_STRING);
  
  int retval = luaL_loadstring(state, RSTRING_PTR(code));
  if(retval == LUA_ERRMEM)
    rb_raise(rb_eNoMemError, "cannot load Lua code", RSTRING_PTR(code));
  else if(retval == LUA_ERRSYNTAX)
    rb_raise(rb_eSyntaxError, "%s", lua_tostring(state, -1));
}

static VALUE rlua_pcall(lua_State* state, int argc)
{
  // stack: |argN-arg1|func|...
  //         <N pts.>  <1>
  int base = lua_gettop(state) - 1 - argc;
  
  int retval = lua_pcall(state, argc, LUA_MULTRET, 0);
  if(retval != 0) {
    const char* error = lua_tostring(state, -1);
    lua_pop(state, 1);
    
    if(retval == LUA_ERRRUN)
      rb_raise(rb_eRuntimeError, "%s", error);
    else if(retval == LUA_ERRMEM)
      rb_raise(rb_eNoMemError, "%s", error);
    else
      rb_fatal("unknown lua_pcall return value");
  } else {
    VALUE retval;
    int n = lua_gettop(state) - base;
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
    } else {
      rb_bug("base > top!");
    }
    return retval;
  }
}

static VALUE rbLuaTable_initialize(int argc, VALUE* argv, VALUE self)
{
  VALUE rbLuaState, ref;
  rb_scan_args(argc, argv, "11", &rbLuaState, &ref);
  
  if(rb_obj_class(rbLuaState) != cLuaState)
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::State)", rb_obj_classname(rbLuaState));
  
  VALUE rbState = rb_iv_get(rbLuaState, "@state");
  rb_iv_set(self, "@state", rbState);
  
  if(ref == Qnil) {
    lua_State* state;
    Data_Get_Struct(rbState, lua_State, state);
      
    lua_newtable(state);
    ref = rlua_makeref(state);
    lua_pop(state, 1);
  } else if(TYPE(ref) != T_FIXNUM) {
    rb_raise(rb_eTypeError, "wrong argument type %s (expected nil)", rb_obj_classname(ref));
  }
  
  rlua_add_ref_finalizer(rbState, ref, self);

  rb_iv_set(self, "@ref", ref);
  
  return self;
}

static VALUE rbLuaTable_next(VALUE self, VALUE table, VALUE index)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(table, "@state"), lua_State, state);
  
  VALUE retval;
  
  int ref = FIX2INT(rb_iv_get(table, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  rlua_push_var(state, index);                     //        |indx|this|refs|...
  if(lua_next(state, -2) != 0) {                   //        |valu|key |this|refs|...
    VALUE value, key;
    value = rlua_get_var(state);                   //        |valu|key |this|refs|...
    lua_pop(state, 1);                             //        |key |this|refs|...
    key = rlua_get_var(state);                     //        |key |this|refs|...
    lua_pop(state, 3);                             //        ...
    
    retval = rb_ary_new();
    rb_ary_push(retval, key);
    rb_ary_push(retval, value);
  } else {                                         //        |this|refs|...
    retval = Qnil;
    lua_pop(state, 2);                             //        ...
  }
  
  return retval;
}

static VALUE rbLuaTable_get(VALUE self, VALUE index)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE value;
  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  rlua_push_var(state, index);                     //        |indx|this|refs|...
  lua_gettable(state, -2);                         //        |valu|this|refs|...
  value = rlua_get_var(state);                     //        |valu|this|refs|...
  lua_pop(state, 2);                               //        ...
  
  return value;
}

static VALUE rbLuaTable_set(VALUE self, VALUE index, VALUE value)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  rlua_push_var(state, index);                     //        |indx|this|refs|...
  rlua_push_var(state, value);                     //        |valu|indx|this|refs|...
  lua_settable(state, -3);                         //        |this|refs|...
  lua_pop(state, 2);                               //        ...

  return value;
}

static VALUE rbLuaTable_rawget(VALUE self, VALUE index)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE value;
  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  rlua_push_var(state, index);                     //        |indx|this|refs|...
  lua_rawget(state, -2);                           //        |valu|this|refs|...
  value = rlua_get_var(state);                     //        |valu|this|refs|...
  lua_pop(state, 2);                               //        ...
  
  return value;
}

static VALUE rbLuaTable_rawset(VALUE self, VALUE index, VALUE value)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  rlua_push_var(state, index);                     //        |indx|this|refs|...
  rlua_push_var(state, value);                     //        |valu|indx|this|refs|...
  lua_rawset(state, -3);                           //        |this|refs|...
  lua_pop(state, 2);                               //        ...

  return value;
}

static VALUE rbLuaTable_get_metatable(VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE value;
  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  if(lua_getmetatable(state, -1)) {                //        |meta|this|refs|...
    value = rlua_get_var(state);                   //        |meta|this|refs|...
    lua_pop(state, 3);                             //        ...
  } else {                                         //        |this|refs|...
    value = Qnil;
    lua_pop(state, 2);                             //        ...
  }
  
  return value;
}

static VALUE rbLuaTable_set_metatable(VALUE self, VALUE metatable)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  if(!rb_obj_is_instance_of(metatable, cLuaTable))
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::Table)", rb_obj_classname(metatable));
  
  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  rlua_push_var(state, metatable);                 //        |meta|this|refs|...
  lua_setmetatable(state, -2);                     //        |this|refs|...
  lua_pop(state, 2);                               //        ...
  
  return metatable;
}

static VALUE rbLuaTable_length(VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE length;
  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  length = INT2FIX(lua_objlen(state, -1));
  lua_pop(state, 2);                               //        ...
  
  return length;
}

static VALUE rbLuaFunction_call(VALUE self, VALUE args);

static VALUE rbLuaTable_method_missing(int argc, VALUE* argv, VALUE self)
{
  VALUE id, args;
  rb_scan_args(argc, argv, "1*", &id, &args);

  VALUE name = rb_str_new2(rb_id2name(rb_to_id(id)));
  
  int is_method = 0;
  int is_assign = 0;
  if(RSTRING_PTR(name)[RSTRING_LEN(name) - 1] == '!')
    is_method = 1;
  if(RSTRING_PTR(name)[RSTRING_LEN(name) - 1] == '=')
    is_assign = 1;
  
  if(is_method || is_assign)
    rb_str_resize(name, RSTRING_LEN(name) - 1);
  
  if(is_assign) {
    VALUE value;
    rb_scan_args(argc, argv, "11", &id, &value);
    return rbLuaTable_set(self, name, value);
  } else {
    VALUE value = rbLuaTable_get(self, name);
    if(rb_obj_class(value) != cLuaFunction) {
      if(is_method)
        rb_raise(rb_eTypeError, "%s is not a Lua::Function", RSTRING_PTR(name));
      return value;
    } else {
      if(is_method)
        rb_ary_unshift(args, self);
      return rbLuaFunction_call(value, args);
    }
  }
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
  
  VALUE retval = rb_apply(proc, rb_intern("call"), args);
  
  if(rb_obj_class(retval) == cLuaMultret) {
    VALUE array = rb_iv_get(retval, "@args");
    int i;
    
    for(i = 0; i < RARRAY_LEN(array); i++)
      rlua_push_var(state, RARRAY_PTR(array)[i]);
    
    return RARRAY_LEN(array);
  } else {
    rlua_push_var(state, retval);
    return 1;
  }
}

static VALUE rbLuaFunction_initialize(int argc, VALUE* argv, VALUE self)
{
  VALUE rbLuaState, ref = Qnil, func;
  rb_scan_args(argc, argv, "11", &rbLuaState, &func);
  
  if(rb_obj_class(rbLuaState) != cLuaState)
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::State)", rb_obj_classname(rbLuaState));
  
  VALUE rbState = rb_iv_get(rbLuaState, "@state");
  rb_iv_set(self, "@state", rbState);
  
  lua_State* state;
  Data_Get_Struct(rbState, lua_State, state);
  
  VALUE proc = Qnil;
  
  if(func == Qnil)
    proc = rb_block_proc();
  else if(TYPE(func) == T_FIXNUM)
    ref = func;
  else if(rb_respond_to(func, rb_intern("call")))
    proc = func;
  else
    rb_raise(rb_eTypeError, "wrong argument type %s (expected nil or Proc)", rb_obj_classname(func));

  if(ref == Qnil) {
    lua_pushlightuserdata(state, (void*) proc);
    lua_pushcclosure(state, call_ruby_proc, 1);
    ref = rlua_makeref(state);
    lua_pop(state, 1);
  }
  
  rlua_add_ref_finalizer(rbState, self, ref);

  rb_iv_set(self, "@ref", ref);
  
  return self;
}

static VALUE rbLuaFunction_call(VALUE self, VALUE args)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  int i;
  VALUE retval;

  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  for(i = 0; i < RARRAY_LEN(args); i++)
    rlua_push_var(state, RARRAY_PTR(args)[i]);
                                                   //        |argN-arg1|this|refs|...
  retval = rlua_pcall(state, RARRAY_LEN(args));    //        |refs|...
  lua_pop(state, 1);                               //        ...

  return retval;
}

static VALUE rbLuaFunction_get_env(VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  VALUE env;

  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  lua_getfenv(state, -1);                          //        |envi|this|refs|...
  env = rlua_get_var(state);                       //        |envi|this|refs|...
  lua_pop(state, 3);                               //        ...

  return env;
}

static VALUE rbLuaFunction_set_env(VALUE self, VALUE env)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  if(rb_obj_class(env) != cLuaTable)
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::Table)", rb_obj_classname(env));

  int ref = FIX2INT(rb_iv_get(self, "@ref"));
  lua_getfield(state, LUA_REGISTRYINDEX, "rlua");  // stack: |refs|...
  lua_rawgeti(state, -1, ref);                     //        |this|refs|...
  rlua_push_var(state, env);                       //        |envi|this|refs|...
  lua_setfenv(state, -2);                          //        |this|refs|...
  lua_pop(state, 2);                               //        ...

  return env;
}

static VALUE rbLua_initialize(VALUE self)
{
  lua_State* state = luaL_newstate();
  
  VALUE rbState = Data_Wrap_Struct(rb_cObject, 0, lua_close, state);
  rb_iv_set(self, "@state", rbState);
  
  lua_pushlightuserdata(state, (void*) self);
  lua_setfield(state, LUA_REGISTRYINDEX, "rlua_state");
  
  lua_newtable(state);
  lua_setfield(state, LUA_REGISTRYINDEX, "rlua");
  
  rb_iv_set(rbState, "@refs", rb_hash_new());
  rb_iv_set(rbState, "@procs", rb_ary_new());
  
  return self;
}

static VALUE rbLua_eval(VALUE self, VALUE code)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  rlua_load_string(state, code);
  return rlua_pcall(state, 0);
}

static VALUE rbLua_globals(VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE ref;
  lua_pushvalue(state, LUA_GLOBALSINDEX);
  ref = rlua_makeref(state);
  lua_pop(state, 1);

  return rb_funcall(cLuaTable, rb_intern("new"), 2, self, ref);
}

static VALUE rbLua_getglobal(VALUE self, VALUE index)
{
  VALUE globals = rbLua_globals(self);
  return rbLuaTable_get(globals, index);
}

static VALUE rbLua_setglobal(VALUE self, VALUE index, VALUE value)
{
  return rbLuaTable_set(rbLua_globals(self), index, value);
}

static VALUE rbLua_method_missing(int argc, VALUE* argv, VALUE self)
{
  return rbLuaTable_method_missing(argc, argv, rbLua_globals(self));
}

static VALUE rbLua_new_table(VALUE self)
{
  return rb_funcall(cLuaTable, rb_intern("new"), 1, self);
}

static VALUE rbLuaMultret_initialize(VALUE self, VALUE args)
{
  rb_iv_set(self, "@args", args);
  return self;
}

static VALUE rbLua_multret(VALUE self, VALUE args)
{
  return rb_funcall(cLuaMultret, rb_intern("new"), 1, args);
}

// float typed indexes
// syntax errors
// error handling
// userdata popping
// rawget/set

void Init_rlua()
{
  mLua = rb_define_module("Lua");
  
  cLuaState = rb_define_class_under(mLua, "State", rb_cObject);
  rb_define_method(cLuaState, "initialize", rbLua_initialize, 0);
  rb_define_method(cLuaState, "eval", rbLua_eval, 1);
  rb_define_method(cLuaState, "__globals", rbLua_globals, 0);
  rb_define_method(cLuaState, "[]", rbLua_getglobal, 1);
  rb_define_method(cLuaState, "[]=", rbLua_setglobal, 2);
  rb_define_method(cLuaState, "method_missing", rbLua_method_missing, -1);
  rb_define_method(cLuaState, "new_table", rbLua_new_table, 0);
  
  cLuaMultret = rb_define_class_under(mLua, "Multret", rb_cObject);
  rb_define_method(cLuaMultret, "initialize", rbLuaMultret_initialize, 1);
  rb_define_singleton_method(mLua, "multret", rbLua_multret, -2);
  
  cLuaFunction = rb_define_class_under(mLua, "Function", rb_cObject);
  rb_define_method(cLuaFunction, "initialize", rbLuaFunction_initialize, -1);
  rb_define_method(cLuaFunction, "call", rbLuaFunction_call, -2);
  rb_define_method(cLuaFunction, "env", rbLuaFunction_get_env, 0);
  rb_define_method(cLuaFunction, "env=", rbLuaFunction_set_env, 1);

  cLuaTable = rb_define_class_under(mLua, "Table", rb_cObject);
  rb_define_singleton_method(cLuaTable, "next", rbLuaTable_next, 2);
  rb_define_method(cLuaTable, "initialize", rbLuaTable_initialize, -1);
  rb_define_method(cLuaTable, "__metatable", rbLuaTable_get_metatable, 0);
  rb_define_method(cLuaTable, "__metatable=", rbLuaTable_set_metatable, 1);
  rb_define_method(cLuaTable, "__length", rbLuaTable_length, 0);
  rb_define_method(cLuaTable, "__get", rbLuaTable_rawget, 1);
  rb_define_method(cLuaTable, "__set", rbLuaTable_rawset, 2);
  //rb_define_method(cLuaTable, "each", rbLuaTable_each, 0);
  //rb_define_method(cLuaTable, "to_a", rbLuaTable_to_array, 0);
  //rb_define_method(cLuaTable, "to_hash", rbLuaTable_to_hash, 0);
  rb_define_method(cLuaTable, "[]", rbLuaTable_get, 1);
  rb_define_method(cLuaTable, "[]=", rbLuaTable_set, 2);
  rb_define_method(cLuaTable, "method_missing", rbLuaTable_method_missing, -1);
}
