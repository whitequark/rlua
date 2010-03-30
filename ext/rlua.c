#include <ruby.h> 
#include <lua5.1/lua.h>
#include <lua5.1/lauxlib.h>
#include <ctype.h>

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
      } else if(rb_obj_class(value) == cLuaState) {
        lua_State* state;
        Data_Get_Struct(rb_iv_get(value, "@state"), lua_State, state);
        lua_pushthread(state);
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
    rb_raise(rb_eNoMemError, "cannot load Lua code");
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
  
  rlua_push_var(state, table);                     // stack: |this|...
  rlua_push_var(state, index);                     //        |indx|this|...
  if(lua_next(state, -2) != 0) {                   //        |valu|key |this|...
    VALUE value, key;
    value = rlua_get_var(state);                   //        |valu|key |this|...
    lua_pop(state, 1);                             //        |key |this|...
    key = rlua_get_var(state);                     //        |key |this|...
    lua_pop(state, 2);                             //        ...
    
    retval = rb_ary_new();
    rb_ary_push(retval, key);
    rb_ary_push(retval, value);
  } else {                                         //        |this|...
    retval = Qnil;
    lua_pop(state, 1);                             //        ...
  }
  
  return retval;
}

static VALUE rbLuaTable_get(VALUE self, VALUE index)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE value;
  rlua_push_var(state, self);                      // stack: |this|...
  rlua_push_var(state, index);                     //        |indx|this|...
  lua_gettable(state, -2);                         //        |valu|this|...
  value = rlua_get_var(state);                     //        |valu|this|...
  lua_pop(state, 2);                               //        ...
  
  return value;
}

static VALUE rbLuaTable_set(VALUE self, VALUE index, VALUE value)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  rlua_push_var(state, self);                      // stack: |this|...
  rlua_push_var(state, index);                     //        |indx|this|...
  rlua_push_var(state, value);                     //        |valu|indx|this|...
  lua_settable(state, -3);                         //        |this|...
  lua_pop(state, 1);                               //        ...

  return value;
}

static VALUE rbLuaTable_equal(VALUE self, VALUE table)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  int equal;
  rlua_push_var(state, self);           // stack: |this|...
  rlua_push_var(state, table);          //        |tble|this|...
  equal = lua_equal(state, -1, -2);     //        |tble|this|...
  lua_pop(state, 2);                    //        ...

  return equal ? Qtrue : Qfalse;
}

static VALUE rbLuaTable_rawget(VALUE self, VALUE index)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE value;
  rlua_push_var(state, self);                      // stack: |this|...
  rlua_push_var(state, index);                     //        |indx|this|...
  lua_rawget(state, -2);                           //        |valu|this|...
  value = rlua_get_var(state);                     //        |valu|this|...
  lua_pop(state, 2);                               //        ...
  
  return value;
}

static VALUE rbLuaTable_rawset(VALUE self, VALUE index, VALUE value)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  rlua_push_var(state, self);                      // stack: |this|...
  rlua_push_var(state, index);                     //        |indx|this|...
  rlua_push_var(state, value);                     //        |valu|indx|this|...
  lua_rawset(state, -3);                           //        |this|...
  lua_pop(state, 1);                               //        ...

  return value;
}

static VALUE rbLuaTable_get_metatable(VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE value;
  rlua_push_var(state, self);                      // stack: |this|...
  if(lua_getmetatable(state, -1)) {                //        |meta|this|...
    value = rlua_get_var(state);                   //        |meta|this|...
    lua_pop(state, 2);                             //        ...
  } else {                                         //        |this|...
    value = Qnil;
    lua_pop(state, 1);                             //        ...
  }
  
  return value;
}

static VALUE rbLuaTable_set_metatable(VALUE self, VALUE metatable)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  if(!rb_obj_is_instance_of(metatable, cLuaTable))
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::Table)", rb_obj_classname(metatable));
  
  rlua_push_var(state, self);                      // stack: |refs|...
  rlua_push_var(state, metatable);                 //        |meta|this|...
  lua_setmetatable(state, -2);                     //        |this|...
  lua_pop(state, 1);                               //        ...
  
  return metatable;
}

static VALUE rbLuaTable_length(VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE length;
  rlua_push_var(state, self);                      // stack: |this|...
  length = INT2FIX(lua_objlen(state, -1));
  lua_pop(state, 1);                               //        ...
  
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
  
  rlua_add_ref_finalizer(rbState, ref, self);

  rb_iv_set(self, "@ref", ref);
  
  return self;
}

static VALUE rbLuaFunction_call(VALUE self, VALUE args)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  int i;
  VALUE retval;

  rlua_push_var(state, self);                      // stack: |this|...
  for(i = 0; i < RARRAY_LEN(args); i++)
    rlua_push_var(state, RARRAY_PTR(args)[i]);
                                                   //        |argN-arg1|this|...
  retval = rlua_pcall(state, RARRAY_LEN(args));    //        ...

  return retval;
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

static VALUE rbLua_get_env(VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  VALUE ref;
  rlua_push_var(state, self);                   // stack: |this|...
  lua_getfenv(state, -1);                       //        |envi|this|...
  ref = rlua_makeref(state);                    //        |envi|this|...
  lua_pop(state, 2);                            //        ...

  return rb_funcall(cLuaTable, rb_intern("new"), 2, self, ref);
}

static VALUE rbLua_set_env(VALUE self, VALUE env)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  if(rb_obj_class(env) != cLuaTable)
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::Table)", rb_obj_classname(env));

  rlua_push_var(state, self);                      // stack: |this|...
  rlua_push_var(state, env);                       //        |envi|this|...
  lua_setfenv(state, -2);                          //        |this|...
  lua_pop(state, 2);                               //        ...
  
  return env;
}

static VALUE rbLua_get_global(VALUE self, VALUE index)
{
  VALUE globals = rbLua_get_env(self);
  return rbLuaTable_get(globals, index);
}

static VALUE rbLua_set_global(VALUE self, VALUE index, VALUE value)
{
  return rbLuaTable_set(rbLua_get_env(self), index, value);
}

static VALUE rbLua_rawequal(VALUE self, VALUE other)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  int equal;
  rlua_push_var(state, self);           // stack: |this|...
  rlua_push_var(state, other);          //        |othr|this|...
  equal = lua_rawequal(state, -1, -2);  //        |othr|this|...
  lua_pop(state, 2);                    //        ...

  return equal ? Qtrue : Qfalse;
}

static VALUE rbLua_method_missing(int argc, VALUE* argv, VALUE self)
{
  return rbLuaTable_method_missing(argc, argv, rbLua_get_env(self));
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

// bootstrap* are from Lua5.1 source

static int bootstrap_tonumber (lua_State *L) {
  int base = luaL_optint(L, 2, 10);
  if (base == 10) {  /* standard conversion */
    luaL_checkany(L, 1);
    if (lua_isnumber(L, 1)) {
      lua_pushnumber(L, lua_tonumber(L, 1));
      return 1;
    }
  }
  else {
    const char *s1 = luaL_checkstring(L, 1);
    char *s2;
    unsigned long n;
    luaL_argcheck(L, 2 <= base && base <= 36, 2, "base out of range");
    n = strtoul(s1, &s2, base);
    if (s1 != s2) {  /* at least one valid digit? */
      while (isspace((unsigned char)(*s2))) s2++;  /* skip trailing spaces */
      if (*s2 == '\0') {  /* no invalid trailing characters? */
        lua_pushnumber(L, (lua_Number)n);
        return 1;
      }
    }
  }
  lua_pushnil(L);  /* else not a number */
  return 1;
}

static int bootstrap_tostring (lua_State *L) {
  luaL_checkany(L, 1);
  if (luaL_callmeta(L, 1, "__tostring"))  /* is there a metafield? */
    return 1;  /* use its value */
  switch (lua_type(L, 1)) {
    case LUA_TNUMBER:
      lua_pushstring(L, lua_tostring(L, 1));
      break;
    case LUA_TSTRING:
      lua_pushvalue(L, 1);
      break;
    case LUA_TBOOLEAN:
      lua_pushstring(L, (lua_toboolean(L, 1) ? "true" : "false"));
      break;
    case LUA_TNIL:
      lua_pushliteral(L, "nil");
      break;
    default:
      lua_pushfstring(L, "%s: %p", luaL_typename(L, 1), lua_topointer(L, 1));
      break;
  }
  return 1;
}

static int bootstrap_error (lua_State *L) {
  int level = luaL_optint(L, 2, 1);
  lua_settop(L, 1);
  if (lua_isstring(L, 1) && level > 0) {  /* add extra information? */
    luaL_where(L, level);
    lua_pushvalue(L, 1);
    lua_concat(L, 2);
  }
  return lua_error(L);
}

static int bootstrap_type (lua_State *L) {
  luaL_checkany(L, 1);
  lua_pushstring(L, luaL_typename(L, 1));
  return 1;
}


static int bootstrap_next (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 2);  /* create a 2nd argument if there isn't one */
  if (lua_next(L, 1))
    return 2;
  else {
    lua_pushnil(L);
    return 1;
  }
}


static int bootstrap_pairs (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushvalue(L, lua_upvalueindex(1));  /* return generator, */
  lua_pushvalue(L, 1);  /* state, */
  lua_pushnil(L);  /* and initial value */
  return 3;
}


static int bootstrap_inext (lua_State *L) {
  int i = luaL_checkint(L, 2);
  luaL_checktype(L, 1, LUA_TTABLE);
  i++;  /* next value */
  lua_pushinteger(L, i);
  lua_rawgeti(L, 1, i);
  return (lua_isnil(L, -1)) ? 0 : 2;
}


static int bootstrap_ipairs (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushvalue(L, lua_upvalueindex(1));  /* return generator, */
  lua_pushvalue(L, 1);  /* state, */
  lua_pushinteger(L, 0);  /* and initial value */
  return 3;
}

static int bootstrap_unpack (lua_State *L) {
  int i, e, n;
  luaL_checktype(L, 1, LUA_TTABLE);
  i = luaL_optint(L, 2, 1);
  e = luaL_opt(L, luaL_checkint, 3, luaL_getn(L, 1));
  n = e - i + 1;  /* number of elements */
  if (n <= 0) return 0;  /* empty range */
  luaL_checkstack(L, n, "table too big to unpack");
  for (; i<=e; i++)  /* push arg[i...e] */
    lua_rawgeti(L, 1, i);
  return n;
}


static int bootstrap_select (lua_State *L) {
  int n = lua_gettop(L);
  if (lua_type(L, 1) == LUA_TSTRING && *lua_tostring(L, 1) == '#') {
    lua_pushinteger(L, n-1);
    return 1;
  }
  else {
    int i = luaL_checkint(L, 1);
    if (i < 0) i = n + i;
    else if (i > n) i = n;
    luaL_argcheck(L, 1 <= i, 1, "index out of range");
    return n - i;
  }
}

static int bootstrap_assert (lua_State *L) {
  luaL_checkany(L, 1);
  if (!lua_toboolean(L, 1))
    return luaL_error(L, "%s", luaL_optstring(L, 2, "assertion failed!"));
  return lua_gettop(L);
}

static int bootstrap_pcall (lua_State *L) {
  int status;
  luaL_checkany(L, 1);
  status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
  lua_pushboolean(L, (status == 0));
  lua_insert(L, 1);
  return lua_gettop(L);  /* return status + all results */
}

static int bootstrap_xpcall (lua_State *L) {
  int status;
  luaL_checkany(L, 2);
  lua_settop(L, 2);
  lua_insert(L, 1);  /* put error function under function to be called */
  status = lua_pcall(L, 0, LUA_MULTRET, 1);
  lua_pushboolean(L, (status == 0));
  lua_replace(L, 1);
  return lua_gettop(L);  /* return status + all results */
}

static const
  struct { char* name; lua_CFunction func; }
  stdlib[] = {
    { "type",     bootstrap_type     },
    { "next",     bootstrap_next     },
    { "tonumber", bootstrap_tonumber },
    { "tostring", bootstrap_tostring },
    { "unpack",   bootstrap_unpack   },
    { "select",   bootstrap_select   },
    { "error",    bootstrap_error    },
    { "assert",   bootstrap_assert   },
    { "pcall",    bootstrap_pcall    },
    { "xpcall",   bootstrap_xpcall   },
  };

// deploy an absolute minimum of functions required to write Lua programs
// most of them can be implemented with Ruby, but this is slow.

static VALUE rbLua_bootstrap(VALUE self)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);
  
  int nf;
  for(nf = 0; nf < sizeof(stdlib) / sizeof(stdlib[0]); nf++) {
    lua_pushcclosure(state, stdlib[nf].func, 0);
    lua_setglobal(state, stdlib[nf].name);
  }
  
  lua_pushcfunction(state, bootstrap_next);
  lua_pushcclosure(state, bootstrap_pairs, 1);
  lua_setglobal(state, "pairs");
  
  lua_pushcfunction(state, bootstrap_inext);
  lua_pushcclosure(state, bootstrap_ipairs, 1);
  lua_setglobal(state, "ipairs");
  
  return Qtrue;
}

// float typed indexes
// syntax errors
// error handling
// userdata popping

void Init_rlua()
{
  mLua = rb_define_module("Lua");
  
  cLuaState = rb_define_class_under(mLua, "State", rb_cObject);
  rb_define_method(cLuaState, "initialize", rbLua_initialize, 0);
  rb_define_method(cLuaState, "__eval", rbLua_eval, 1);
  rb_define_method(cLuaState, "__bootstrap", rbLua_bootstrap, 0);
  rb_define_method(cLuaState, "__env", rbLua_get_env, 0);
  rb_define_method(cLuaState, "__env=", rbLua_set_env, 1);
  rb_define_method(cLuaState, "[]", rbLua_get_global, 1);
  rb_define_method(cLuaState, "[]=", rbLua_set_global, 2);
  rb_define_method(cLuaState, "method_missing", rbLua_method_missing, -1);
  
  cLuaMultret = rb_define_class_under(mLua, "Multret", rb_cObject);
  rb_define_method(cLuaMultret, "initialize", rbLuaMultret_initialize, 1);
  rb_define_singleton_method(mLua, "multret", rbLua_multret, -2);
  
  cLuaFunction = rb_define_class_under(mLua, "Function", rb_cObject);
  rb_define_method(cLuaFunction, "initialize", rbLuaFunction_initialize, -1);
  rb_define_method(cLuaFunction, "call", rbLuaFunction_call, -2);
  rb_define_method(cLuaFunction, "__env", rbLua_get_env, 0);
  rb_define_method(cLuaFunction, "__env=", rbLua_set_env, 1);

  cLuaTable = rb_define_class_under(mLua, "Table", rb_cObject);
  rb_define_singleton_method(cLuaTable, "next", rbLuaTable_next, 2);
  rb_define_method(cLuaTable, "initialize", rbLuaTable_initialize, -1);
  rb_define_method(cLuaTable, "__metatable", rbLuaTable_get_metatable, 0);
  rb_define_method(cLuaTable, "__metatable=", rbLuaTable_set_metatable, 1);
  rb_define_method(cLuaTable, "__length", rbLuaTable_length, 0);
  rb_define_method(cLuaTable, "__get", rbLuaTable_rawget, 1);
  rb_define_method(cLuaTable, "__set", rbLuaTable_rawset, 2);
  rb_define_method(cLuaTable, "__equal", rbLua_rawequal, 1);
  rb_define_method(cLuaTable, "[]", rbLuaTable_get, 1);
  rb_define_method(cLuaTable, "[]=", rbLuaTable_set, 2);
  rb_define_method(cLuaTable, "==", rbLuaTable_equal, 1);
  rb_define_method(cLuaTable, "method_missing", rbLuaTable_method_missing, -1);
}
