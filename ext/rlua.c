#include <ruby.h>
#include <lua5.1/lua.h>
#include <lua5.1/lauxlib.h>
#include <lua5.1/lualib.h>
#include <ctype.h>
#include <ruby/encoding.h>

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
      return rb_enc_str_new(string, length, rb_default_external_encoding());
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

    case T_STRING: {
      const char* string;
      string = rb_str_export_to_enc(value, rb_default_external_encoding());

      lua_pushlstring(state, RSTRING_PTR(string), RSTRING_LEN(string));
      break;
    }
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
        rlua_push_var(state, key);
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

static const char* rlua_reader(lua_State* state, void *data, size_t *size)
{
  VALUE code = (VALUE) data;

  if(rb_iv_get(code, "@read") != Qnil) {
    *size = 0;
    return NULL;
  } else {
    *size = RSTRING_LEN(code);
    rb_iv_set(code, "@read", Qtrue);
    return RSTRING_PTR(code);
  }
}

static void rlua_load_string(lua_State* state, VALUE code, VALUE chunkname)
{
  Check_Type(code, T_STRING);
  Check_Type(chunkname, T_STRING);

  // do not interfere with users' string
  VALUE interm_code = rb_str_new3(code);

  int retval = lua_load(state, rlua_reader, (void*) interm_code, RSTRING_PTR(chunkname));
  if(retval != 0) {
    size_t errlen;
    const char* errstr = lua_tolstring(state, -1, &errlen);
    VALUE error = rb_str_new(errstr, errlen);
    lua_pop(state, 1);
    if(retval == LUA_ERRMEM)
      rb_exc_raise(rb_exc_new3(rb_eNoMemError, error));
    else if(retval == LUA_ERRSYNTAX)
      rb_exc_raise(rb_exc_new3(rb_eSyntaxError, error));
  }
}

static VALUE rlua_pcall(lua_State* state, int argc)
{
  // stack: |argN-arg1|func|...
  //         <N pts.>  <1>
  int base = lua_gettop(state) - 1 - argc;

  int retval = lua_pcall(state, argc, LUA_MULTRET, 0);
  if(retval != 0) {
    size_t errlen;
    const char* errstr = lua_tolstring(state, -1, &errlen);
    VALUE error = rb_str_new(errstr, errlen);
    lua_pop(state, 1);

    if(retval == LUA_ERRRUN)
      rb_exc_raise(rb_exc_new3(rb_eRuntimeError, error));
    else if(retval == LUA_ERRSYNTAX)
      rb_exc_raise(rb_exc_new3(rb_eSyntaxError, error));
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

/* :nodoc: */
static VALUE rbLuaTable_initialize(int argc, VALUE* argv, VALUE self)
{
  VALUE rbLuaState, ref;
  rb_scan_args(argc, argv, "11", &rbLuaState, &ref);

  VALUE stateSource = rb_obj_class(rbLuaState);
  if(stateSource != cLuaState && stateSource != cLuaTable && stateSource != cLuaFunction)
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::State, Lua::Table or Lua::Function)",
           rb_obj_classname(rbLuaState));

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

/*
 * call-seq: Lua::Table.next(table, key) -> [ key, value ] or nil
 *
 * Iterates over a Lua table referenced by +table+.
 * This function is analogue for Lua next function, but can be used
 * even if Lua one is not defined.
 *
 * You can use this method yourself, through it is easier to invoke
 * to_a or to_hash.
 */
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

/*
 * call-seq: table[key] -> value
 *
 * Returns value associated with +key+ in Lua table. May invoke Lua
 * +__index+ metamethod.
 */
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

/*
 * call-seq: table[key] = value -> value
 *
 * Sets value associated with +key+ to +value+ in Lua table. May invoke Lua
 * +__newindex+ metamethod.
 */
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

/*
 * call-seq: table.__get(key) -> value
 *
 * Returns value associated with +key+ in Lua table without invoking
 * any Lua metamethod (similar to +rawget+ Lua function).
 */
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

/*
 * call-seq: table.__set(key, value) -> value
 *
 * Sets value associated with +key+ to +value+ in Lua table without invoking
 * any Lua metamethod (similar to +rawset+ Lua function).
 */
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

/*
 * call-seq: table.__length -> int
 *
 * Returns table length as with Lua length # operator. Will not call
 * any metamethod because __len metamethod has no effect when
 * defined on table (and string).
 */
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

/*
 * call-seq: table.method_missing(method, *args) -> *values
 *
 * This method allows accessing Lua tables much as if they were Ruby
 * objects.
 *
 * <b>Setting values</b> can be done with a standard <i>setter method</i>:
 * <tt>table.key = value</tt> is equivalent to <tt>table["key"] = value</tt>.
 *
 * <b>Getting values</b> is as easy as setting: if the value is not a function,
 * <tt>table.key</tt> is equivalent to <tt>table["key"]</tt>.
 *
 * If some table has a function as value, you can <b>invoke methods</b> on it.
 * <tt>table.method(arg1, ..., argN)</tt> is equivalent to
 * <tt>table["method"].call(arg1, ..., argN)</tt>, and
 * <tt>table.method!(arg1, ..., argN)</tt> is equivalent to
 * <tt>table["method"].call(table, arg1, ..., argN)</tt>.
 * To get a reference to function you should use the <tt>table["method"]</tt>
 * notation.
 *
 * If the value is not present in table (which is equivalent to +nil+ value
 * in Lua) MethodNotFound exception will be raised; if you will attempt
 * to call something which is not a function as a method, TypeError exception
 * will be raised.
 */
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
    if(value == Qnil) {
      return rb_call_super(argc, argv);
    } else if(rb_obj_class(value) != cLuaFunction) {
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

/*
 * call-seq: Lua::Function.new(state, proc)
 *
 * Converts a Ruby closure +proc+ to a Lua function in Lua::State represented
 * by +state+. Note that you generally do not need to call this function
 * explicitly: any +proc+ or +lambda+ passed as a value for table assignment
 * will be automagically converted to Lua closure.
 */
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

  if(TYPE(func) == T_FIXNUM)
    ref = func;
  else if(rb_respond_to(func, rb_intern("call")))
    proc = func;
  else
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Proc)", rb_obj_classname(func));

  if(ref == Qnil) {
    lua_pushlightuserdata(state, (void*) proc);
    lua_pushcclosure(state, call_ruby_proc, 1);
    ref = rlua_makeref(state);
    lua_pop(state, 1);

    // don't allow GC to collect proc
    rb_iv_set(self, "@proc", proc);
  }

  rlua_add_ref_finalizer(rbState, ref, self);

  rb_iv_set(self, "@ref", ref);

  return self;
}

/*
 * call-seq: func.call(*args) -> *values
 *
 * Invokes a Lua function in protected environment (like a Lua +xpcall+).
 *
 * One value returned in Lua is returned as one value in Ruby; multiple values
 * returned in Lua are returned as an array of them in Ruby. This convention
 * allows usage of identical code for calling methods with multiple return
 * values in both languages.
 *
 * While you can easily call Lua procedures and use their return values,
 * returning multiple values from Ruby is not that easy. RLua cannot
 * guess whether array returned by Ruby function is a 'real' array or
 * just several return values, so there is a Lua.multret proxy method for
 * that purpose. Ruby return values constructed with +multret+ method
 * are automatically unpacked to multiple return values. Example:
 *
 *   state = Lua::State.new
 *   state.p = lambda{ |*args| p *args }
 *   state.f1 = Lua::Function.new(state, lambda{ [ 1, 2, 3 ] })
 *   state.f2 = Lua::Function.new(state, lambda{ Lua.multret( 1, 2, 3 ) })
 *   state.__eval("p(f1())") # results in { 1.0, 2.0, 3.0 } Lua table
 *                           # shown as array
 *   state.__eval("p(f2())") # results in 1.0, 2.0, 3.0 Lua multiple return values
 *                           # shown as three separate values
 *
 * Any Lua error that has appeared during the call will be raised as Ruby
 * exception with message equal to Lua error message.
 * *Lua error messages are explicitly converted to strings with +lua_tostring+
 * function.*
 * Lua errors are mapped as follows:
 * LUA_ERRMEM:: NoMemError is raised.
 * LUA_ERRRUN:: RuntimeError is raised.
 *
 * Note that if any uncatched exception is raised in Ruby code inside
 * Lua::Function it will be propagated as Lua error. *In this case the
 * backtrace is lost!* Catch your exceptions in Ruby code if you want to
 * keep their backtraces.
 *
 * This 'drawback' is intentional: while it is technically possible to
 * re-raise the same Exception object, the backtrace will lead to re-raise
 * point anyway.
 */
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

/*
 * call-seq: Lua::State.new
 *
 * Creates a new Lua state.
 */
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

/*
 * call-seq: state.__eval(code[, chunkname='=&lt;eval&gt;']) -> *values
 *
 * Runs +code+ in Lua interpreter. Optional argument +chunkname+
 * specifies a string that will be used in error messages and other
 * debug information as a file name.
 *
 * Start +chunkname+ with a @ to make Lua think the following is filename
 * (e.g. @test.lua); start it with a = to indicate a non-filename stream
 * (e.g. =stdin). Anything other is interpreted as a plaintext Lua code and
 * a few starting characters will be shown.
 */
static VALUE rbLua_eval(int argc, VALUE* argv, VALUE self)
{
  VALUE code, chunkname;
  rb_scan_args(argc, argv, "11", &code, &chunkname);

  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  if(chunkname == Qnil)
    chunkname = rb_str_new2("=<eval>");

  rlua_load_string(state, code, chunkname);

  return rlua_pcall(state, 0);
}

/*
 * call-seq: object.__env() -> Lua::Table
 *
 * Returns environment table of Lua::State or Lua::Function.
 */
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

/*
 * call-seq: object.__env=(table) -> table
 *
 * Sets environment table for Lua::State or Lua::Function. +table+ may be
 * a Lua::Table or a Hash.
 */
static VALUE rbLua_set_env(VALUE self, VALUE env)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  if(rb_obj_class(env) != cLuaTable && TYPE(env) != T_HASH)
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::Table or Hash)", rb_obj_classname(env));

  rlua_push_var(state, self);                      // stack: |this|...
  rlua_push_var(state, env);                       //        |envi|this|...
  lua_setfenv(state, -2);                          //        |this|...
  lua_pop(state, 1);                               //        ...

  return env;
}

/*
 * call-seq: state.__get_metatable(object) -> Lua::Table or nil
 *
 * Returns metatable of any valid Lua object or nil if it is not present.
 * If you want to get metatables of non-table objects (e.g. numbers)
 * just pass their Ruby equivalent.
 */
static VALUE rbLua_get_metatable(VALUE self, VALUE object)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  rlua_push_var(state, object);                 // stack: |objt|...
  if(lua_getmetatable(state, -1)) {             //        |meta|objt|...
    VALUE ref = rlua_makeref(state);            //        |meta|objt|...
    lua_pop(state, 2);                          //        ...

    return rb_funcall(cLuaTable, rb_intern("new"), 2, self, ref);
  } else {                                      //        |objt|...
    lua_pop(state, 1);                          //        ...

    return Qnil;
  }
}

/*
 * call-seq: state.__set_metatable=(object, metatable) -> metatable
 *
 * Sets metatable for any valid Lua object. +metatable+ can be Lua::Table or
 * Hash. If you want to set metatables for non-table objects (e.g. numbers)
 * just pass their Ruby equivalent.
 *
 *   # Implement concatenation operator for Lua strnigs.
 *   state = Lua::State.new
 *   state.__set_metatable("", { '__add' => lambda{ |a, b| a + b } })
 *   p state.__eval('return "hello," + " world"') # => "hello, world"
 */
static VALUE rbLua_set_metatable(VALUE self, VALUE object, VALUE metatable)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  if(rb_obj_class(metatable) != cLuaTable && TYPE(metatable) != T_HASH)
    rb_raise(rb_eTypeError, "wrong argument type %s (expected Lua::Table or Hash)", rb_obj_classname(metatable));

  rlua_push_var(state, object);                    // stack: |objt|...
  rlua_push_var(state, metatable);                 //        |meta|objt|...
  lua_setmetatable(state, -2);                     //        |objt|...
  lua_pop(state, 1);                               //        ...

  return metatable;
}

/*
 * call-seq: table.__metatable() -> Lua::Table or nil
 *
 * Returns metatable of this table or nil if it is not present.
 */
static VALUE rbLuaTable_get_metatable(VALUE self)
{
  return rbLua_get_metatable(self, self);
}

/*
 * call-seq: table.__metatable=(metatable) -> metatable
 *
 * Sets metatable for this table. +metatable+ can be Lua::Table or Hash.
 */
static VALUE rbLuaTable_set_metatable(VALUE self, VALUE metatable)
{
  return rbLua_set_metatable(self, self, metatable);
}

/*
 * call-seq: state[key] -> value
 *
 * Returns value of a global variable. Equivalent to __env[key].
 */
static VALUE rbLua_get_global(VALUE self, VALUE index)
{
  VALUE globals = rbLua_get_env(self);
  return rbLuaTable_get(globals, index);
}

/*
 * call-seq: state[key] = value -> value
 *
 * Sets value for a global variable. Equivalent to __env[key] = value.
 */
static VALUE rbLua_set_global(VALUE self, VALUE index, VALUE value)
{
  return rbLuaTable_set(rbLua_get_env(self), index, value);
}

/*
 * call-seq: this == other -> true or false
 *
 * Compares +this+ with +other+. May call +__eq+ metamethod.
 */
static VALUE rbLua_equal(VALUE self, VALUE other)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  int equal;
  rlua_push_var(state, self);           // stack: |this|...
  rlua_push_var(state, other);          //        |othr|this|...
  equal = lua_equal(state, -1, -2);     //        |othr|this|...
  lua_pop(state, 2);                    //        ...

  return equal ? Qtrue : Qfalse;
}

/*
 * call-seq: this.__equal(other) -> true or false
 *
 * Compares +this+ with +other+ without calling any metamethods.
 */
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

/*
 * call-seq: state.method_missing(method, *args) -> *values
 *
 * See Lua::Table#method_missing. Equivalent to __env.method_missing(method, *args).
 */
static VALUE rbLua_method_missing(int argc, VALUE* argv, VALUE self)
{
  return rbLuaTable_method_missing(argc, argv, rbLua_get_env(self));
}

/*
 * call-seq: Lua::Multret.new(*values)
 *
 * Creates a new Multret object with +values+ inside. Example:
 *
 *   Lua::Multret.new(1, 2, 3) # three return values
 */
static VALUE rbLuaMultret_initialize(VALUE self, VALUE args)
{
  rb_iv_set(self, "@args", args);
  return self;
}

/*
 * call-seq: Lua.multret(*values)
 *
 * Shorthand for Lua::Multret.new.
 */
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

/*
 * call-seq: state.__bootstrap() -> true
 *
 * Deploys an absolute minimum of functions required to write minimally
 * useful Lua programs. This is really a subset of Lua _base_ library
 * (copied from Lua 5.1 sources) that may be handy if you don't like standard
 * function layout. All of these functions can be implemented in pure Ruby,
 * but that will slow down Lua code incredibly.
 *
 * <b>If you want to get familiar layout described in Lua documentation, check
 * #__load_stdlib function.</b>
 *
 * Exact list of functions defined: type, next, tonumber, tostring, unpack,
 * select, error, assert, pcall, xpcall.
 */
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

static void rlua_openlib(lua_State* state, lua_CFunction func)
{
  lua_pushcfunction(state, func);
  lua_call(state, 0, 0);
}

/*
 * call-seq: state.__load_stdlib(*libs) -> true
 *
 * Loads Lua standard libraries. There are two ways of calling this function:
 *
 * If you will call it as __load_stdlib(:all), it is equivalent to calling
 * C +luaL_openlibs+ function.
 *
 * If you will pass it symbolized names of separate libraries (like :base),
 * it is equivalent to calling corresponding +luaopen_*+ functions.
 *
 * Examples:
 *
 *   # Load all standard libraries
 *   state = Lua::State.new
 *   state.__load_stdlib :all
 *
 *   # Load only math, string and table libraries
 *   state = Lua::State.new
 *   state.__load_stdlib :math, :string, :table
 *
 * Exact list of libraries recognized: <tt>:base</tt>, <tt>:table</tt>,
 * <tt>:math</tt>, <tt>:string</tt>, <tt>:debug</tt>, <tt>:io</tt>,
 * <tt>:os</tt>, <tt>:package</tt>.
 * <b>Anything not included in this list will be silently ignored.</b>
 */
static VALUE rbLua_load_stdlib(VALUE self, VALUE args)
{
  lua_State* state;
  Data_Get_Struct(rb_iv_get(self, "@state"), lua_State, state);

  if(rb_ary_includes(args, ID2SYM(rb_intern("all")))) {
    luaL_openlibs(state);
  } else {
    if(rb_ary_includes(args, ID2SYM(rb_intern("base"))))
      rlua_openlib(state, luaopen_base);
    if(rb_ary_includes(args, ID2SYM(rb_intern(LUA_TABLIBNAME))))
      rlua_openlib(state, luaopen_table);
    if(rb_ary_includes(args, ID2SYM(rb_intern(LUA_MATHLIBNAME))))
      rlua_openlib(state, luaopen_math);
    if(rb_ary_includes(args, ID2SYM(rb_intern(LUA_STRLIBNAME))))
      rlua_openlib(state, luaopen_string);
    if(rb_ary_includes(args, ID2SYM(rb_intern(LUA_DBLIBNAME))))
      rlua_openlib(state, luaopen_debug);
    if(rb_ary_includes(args, ID2SYM(rb_intern(LUA_IOLIBNAME))))
      rlua_openlib(state, luaopen_io);
    if(rb_ary_includes(args, ID2SYM(rb_intern(LUA_OSLIBNAME))))
      rlua_openlib(state, luaopen_os);
    if(rb_ary_includes(args, ID2SYM(rb_intern(LUA_LOADLIBNAME))))
      rlua_openlib(state, luaopen_package);
  }

  return Qtrue;
}

void Init_rlua()
{
  /*
   * Main module that encapsulates all RLua classes and methods.
   */
  mLua = rb_define_module("Lua");

  /*
   * Lua::State represents Lua interpreter state which is one thread of
   * execution.
   */
  cLuaState = rb_define_class_under(mLua, "State", rb_cObject);
  rb_define_method(cLuaState, "initialize", rbLua_initialize, 0);
  rb_define_method(cLuaState, "__eval", rbLua_eval, -1);
  rb_define_method(cLuaState, "__bootstrap", rbLua_bootstrap, 0);
  rb_define_method(cLuaState, "__load_stdlib", rbLua_load_stdlib, -2);
  rb_define_method(cLuaState, "__env", rbLua_get_env, 0);
  rb_define_method(cLuaState, "__env=", rbLua_set_env, 1);
  rb_define_method(cLuaState, "__get_metatable", rbLua_get_metatable, 1);
  rb_define_method(cLuaState, "__set_metatable", rbLua_set_metatable, 2);
  rb_define_method(cLuaState, "[]", rbLua_get_global, 1);
  rb_define_method(cLuaState, "[]=", rbLua_set_global, 2);
  rb_define_method(cLuaState, "method_missing", rbLua_method_missing, -1);

  /*
   * An intermediate object assisting return of multiple values from Ruby.
   * See description of Lua::Function#call.
   */
  cLuaMultret = rb_define_class_under(mLua, "Multret", rb_cObject);
  rb_define_method(cLuaMultret, "initialize", rbLuaMultret_initialize, 1);
  rb_define_singleton_method(mLua, "multret", rbLua_multret, -2);

  /*
   * Lua::Function represents a Lua function, may it be a native (i.e.
   * defined in Lua code) function or Ruby closure.
   *
   * Lua::Function is duck-typed like a +proc+ (implements +call+ method)
   * and thus can be used as a replacement, through it cannot be converted
   * to block. You can use
   *
   *   some_method(parameters) { |*args| lua_func.call(*args) }
   *
   * instead.
   *
   * See also important notes about exceptions and return values in
   * #call function.
   */
  cLuaFunction = rb_define_class_under(mLua, "Function", rb_cObject);
  rb_define_method(cLuaFunction, "initialize", rbLuaFunction_initialize, -1);
  rb_define_method(cLuaFunction, "call", rbLuaFunction_call, -2);
  rb_define_method(cLuaFunction, "__env", rbLua_get_env, 0);
  rb_define_method(cLuaFunction, "__env=", rbLua_set_env, 1);
  rb_define_method(cLuaFunction, "__equal", rbLua_rawequal, 1);
  rb_define_method(cLuaFunction, "==", rbLua_equal, 1);

  /*
   * A Ruby Lua::Table object represents a *reference* to a Lua table.
   * As it is a reference, any changes made to table in Lua are visible in
   * Ruby and vice versa.
   *
   * See also #method_missing function for a convenient way to access tables.
   */
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
  rb_define_method(cLuaTable, "==", rbLua_equal, 1);
  rb_define_method(cLuaTable, "method_missing", rbLuaTable_method_missing, -1);
}
