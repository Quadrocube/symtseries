/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Lua sax functions @file */

#include <lua.h>
#include <lauxlib.h>
#include <math.h>
#include <symtseries.h>
#include <string.h>

#ifdef LUA_SANDBOX
#include "luasandbox_output.h"
#include "luasandbox_serialize.h"
#endif

static const char* mozsvc_sax_table = "sax";
static const char* mozsvc_sax_window = "mozsvc.sax.window";
static const char* mozsvc_sax_word = "mozsvc.sax.word";
static const char* mozsvc_sax_win_suffix = "window";
static const char* mozsvc_sax_word_suffix = "word";

static void check_nwc(lua_State* lua, int n, int w, int c, int offset)
{
  luaL_argcheck(lua, n > 1 && n <= 4096,
                offset, "n is out of range");
  luaL_argcheck(lua, w > 1 && w <= 2048, offset, "w is out of range");
  luaL_argcheck(lua, n % w == 0, offset,
                "n must be evenly divisible by w");
  luaL_argcheck(lua, 1 < c && c <= STS_MAX_CARDINALITY, offset,
                "cardinality is out of range");
}

static sts_word check_sax_word(lua_State* lua, int ind)
{
  // word was previously successfully constructed -> No need to check for NULLs
  sts_word* ud = luaL_checkudata(lua, ind, mozsvc_sax_word);
  return *ud;
}

typedef enum {SAX_WORD, SAX_WINDOW} sax_type;

static sax_type sax_gettype(lua_State* lua, int ind)
{
  void* ud = lua_touserdata(lua, ind);
  if (ud) {
    if (lua_getmetatable(lua, ind)) {
      lua_getfield(lua, LUA_REGISTRYINDEX, mozsvc_sax_word);
      if (lua_rawequal(lua, -1, -2)) {
        lua_pop(lua, 2);  /* remove both metatables */
        return SAX_WORD;
      } else {
        lua_pop(lua, 1);  /* remove word metatable */
        lua_getfield(lua, LUA_REGISTRYINDEX, mozsvc_sax_window);
        if (lua_rawequal(lua, -1, -2)) {
          lua_pop(lua, 2);  /* remove both metatables */
          return SAX_WINDOW;
        }
      }
    }
  }
  luaL_typerror(lua, ind, "sax.window or sax.word expected");
  return SAX_WORD; // to silence the warning; unreachable due to longjmp
}

static const struct sts_word* check_word_or_window(lua_State* lua, int ind)
{
  sax_type type = sax_gettype(lua, ind);
  void* ud = lua_touserdata(lua, ind);
  if (type == SAX_WORD) {
    return *((sts_word*)ud);
  } else {
    sts_window window = *((struct sts_window**)ud);
    return &window->current_word;
  }
}

static sts_window check_sax_window(lua_State* lua, int ind)
{
  /* same as with check_sax_word - no need to check for NULLs */
  sts_window* ud = luaL_checkudata(lua, ind, mozsvc_sax_window);
  return *ud;
}

static void push_window(lua_State* lua, sts_window win)
{
  sts_window* ud = lua_newuserdata(lua, sizeof*ud);
  if (!ud) {
    luaL_error(lua, "memory allocation failed");
    // never reached since error long jumps but aids static analysis
    return;
  }

  *ud = win;
  luaL_getmetatable(lua, mozsvc_sax_window);
  lua_setmetatable(lua, -2);
}

static int sax_new_window(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 3, 0, "incorrect number of args");
  int n = luaL_checkint(lua, 1);
  int w = luaL_checkint(lua, 2);
  int c = luaL_checkint(lua, 3);
  check_nwc(lua, n, w, c, 1);

  sts_window win = sts_new_window(n, w, c);
  if (!win) {
    return luaL_error(lua, "memory allocation failed");
  }

  push_window(lua, win);
  return 1;
}

static void push_word(lua_State* lua, const struct sts_word* a)
{
  const struct sts_word** ud = lua_newuserdata(lua, sizeof*ud);
  if (!ud) {
    luaL_error(lua, "memory allocation failed");
    // never reached since error long jumps but aids static analysis
    return;
  }

  *ud = a;
  luaL_getmetatable(lua, mozsvc_sax_word);
  lua_setmetatable(lua, -2);
}

static double* check_array(lua_State* lua, int ind, size_t size)
{
  double* buf = malloc(size * sizeof*buf);
  if (!buf) {
    luaL_error(lua, "memory allocation failed");
    // never reached since error long jumps but aids static analysis
    return NULL;
  }
  for (size_t i = 1; i <= size; ++i) {
    lua_rawgeti(lua, ind, (int)i);
    if (!lua_isnumber(lua, -1)) {
      free(buf);
      luaL_argerror(lua, 1, "expected array of numbers as input");
      // never reached since argerror long jumps but aids static analysis
      return NULL;
    }
    buf[i - 1] = lua_tonumber(lua, -1);
    lua_pop(lua, 1);
  }
  return buf;
}

static int sax_add(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 2, 0, "incorrect number of args");
  sts_window win = check_sax_window(lua, 1);
  if (lua_isnumber(lua, 2)) {
    double d = lua_tonumber(lua, 2);
    sts_append_value(win, d);
  } else {
    if (!lua_istable(lua, 2)) {
      return luaL_argerror(lua, 2, "number or array-like table expected");
    }
    size_t size = lua_objlen(lua, 2);
    if (size) {
      double* vals = check_array(lua, 2, size);
      sts_append_array(win, vals, size);
      free(vals);
    }
  }
  return 0;
}

static int sax_mindist(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 2, 0, "incorrect number of args");
  const struct sts_word* a = check_word_or_window(lua, 1);
  const struct sts_word* b = check_word_or_window(lua, 2);

  double above, below;
  double d = sts_mindist_ab(a, b, &above, &below);
  if (isnan(d)) {
    lua_pushnil(lua);
    return 1;
  } else {
    lua_pushnumber(lua, d);
    lua_pushnumber(lua, above);
    lua_pushnumber(lua, below);
  }
  return 3;
}

static int sax_to_string(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 1, 0, "incorrect number of args");
  const struct sts_word* a = check_word_or_window(lua, 1);
  char* str = sts_word_to_sax_string(a);
  if (!str) {
    return luaL_argerror(lua, 1, "unprocessable symbols for cardinality "
                         "detected");
  }
  lua_pushstring(lua, str);
  free(str);
  return 1;
}

static int sax_equal(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 2, 0, "incorrect number of args");
  const struct sts_word* a = check_word_or_window(lua, 1);
  const struct sts_word* b = check_word_or_window(lua, 2);
  lua_pushboolean(lua, sts_words_equal(a, b));
  return 1;
}

static int sax_window_get_word(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 1, 0, "incorrect number of args");
  sts_window window = check_sax_window(lua, 1);
  push_word(lua, sts_dup_word(&window->current_word));
  return 1;
}

static int sax_from_double_array(lua_State* lua)
{
  int w = luaL_checkint(lua, 2);
  int c = luaL_checkint(lua, 3);
  if (!lua_istable(lua, 1)) {
    return luaL_argerror(lua, 1, "array-like table expected");
  }

  size_t size = lua_objlen(lua, 1);
  check_nwc(lua, (int)size, w, c, 2);

  double* buf = check_array(lua, 1, size);
  sts_word a = sts_from_double_array(buf, size, w, c);
  free(buf);
  if (!a) {
    return luaL_error(lua, "memory allocation failed");
  }
  push_word(lua, a);
  return 1;
}

static int sax_from_string(lua_State* lua)
{
  size_t len;
  const char* s = luaL_checklstring(lua, 1, &len);
  luaL_argcheck(lua, len > 1, 1, "length of SAX string should be > 1");
  int c = luaL_checkint(lua, 2);
  sts_word a = sts_from_sax_string(s, c);
  if (!a) {
    return luaL_argerror(lua, 1, "illegal symbols for given cardinality "
                         "or bad cardinality itself");
  }
  push_word(lua, a);
  return 1;
}

static int sax_new_word(lua_State* lua)
{
  int argc = lua_gettop(lua);
  switch (argc) {
  case 2:
    return sax_from_string(lua);
  case 3:
    return sax_from_double_array(lua);
  default:
    return luaL_argerror(lua, 0, "incorrect number of arguments");
  }
}

static int sax_clear(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 1, 0, "incorrect number of arguments");
  sts_window win = check_sax_window(lua, 1);
  sts_reset_window(win);
  return 0;
}

#ifdef LUA_SANDBOX

static bool all_nans(double* array, size_t size)
{
  for (size_t i = 0; i < size; ++i) {
    if (!isnan(array[i])) return false;
  }
  return true;
}

static int serialize_sax(lua_State* lua)
{
  lsb_output_data* output = lua_touserdata(lua, -1);
  const char* key = lua_touserdata(lua, -2);
  sax_type type = sax_gettype(lua, -3);
  if (!key || !output) return 1;
  switch (type) {
  case SAX_WINDOW:
    {
      const struct sts_window* win = check_sax_window(lua, -3);
      size_t n = win->current_word.n_values;
      size_t w = win->current_word.w;
      size_t c = win->current_word.c;
      if (lsb_appendf(output,
                      "if %s == nil then %s = sax.window.new(%" PRIuSIZE
                      ", %" PRIuSIZE ", %" PRIuSIZE ") end\n",
                      key, key, n, w, c)) return 1;
      if (!all_nans(win->values->buffer, win->current_word.n_values + 1)) {
        if (lsb_appendf(output, "%s:clear()\n%s:add({", key, key)) return 1;
        double* val = win->values->head;
        size_t n_values = 0;
        while (n_values < n) {
          if (n_values++ != 0 && lsb_appends(output, ",", 1)) return 1;
          if (lsb_serialize_double(output, *val)) return 1;
          if (++val == win->values->buffer_end) val = win->values->buffer;
        }
        if (lsb_appends(output, "})\n", 3)) return 1;
      }
      return 0;
    }
  case SAX_WORD:
    {
      const struct sts_word* a = check_sax_word(lua, -3);
      char* sax = sts_word_to_sax_string(a);
      if (!sax) {
        return luaL_error(lua, "memory allocation failed");
      }
      if (lsb_appendf(output,
                      "if %s == nil then %s = sax.word.new(\"%s\", %" PRIuSIZE
                      ") end\n",
                      key, key, sax, a->c)) {
        free(sax);
        return 1;
      }
      free(sax);
      return 0;
    }
  }
  return 1;
}

static int output_sax(lua_State* lua)
{
  lsb_output_data* output = lua_touserdata(lua, -1);
  if (!output) {
    return 1;
  }
  const struct sts_word* a = check_word_or_window(lua, -2);
  char* sax = sts_word_to_sax_string(a);
  if (!sax) {
    return luaL_error(lua, "unprocessable symbols for cardinality detected");
  }
  if (lsb_appends(output, sax, strlen(sax))) {
    free(sax);
    return 1;
  }
  free(sax);
  return 0;
}

#endif // LUA_SANDBOX

static int sax_gc_window(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 1, 0, "incorrect number of arguments");
  sts_window win = check_sax_window(lua, 1);
  sts_free_window(win);
  return 0;
}

static int sax_gc_word(lua_State* lua)
{
  luaL_argcheck(lua, lua_gettop(lua) == 1, 0, "incorrect number of arguments");
  sts_word a = check_sax_word(lua, 1);
  sts_free_word(a);
  return 0;
}

static int sax_version(lua_State* lua)
{
  lua_pushstring(lua, DIST_VERSION);
  return 1;
}

static const struct luaL_Reg saxlib_f[] =
{
  { "mindist", sax_mindist }
  , { "version", sax_version }
  , { NULL, NULL }
};

static const struct luaL_Reg saxlib_word[] =
{
  { "__gc", sax_gc_word }
  , { "__tostring", sax_to_string }
  , { NULL, NULL }
};

static const struct luaL_Reg saxlib_win[] =
{
  { "add", sax_add }
  , { "clear", sax_clear }
  , { "__gc", sax_gc_window }
  , { "__tostring", sax_to_string }
  , { "get_word", sax_window_get_word }
  , { NULL, NULL }
};

static void reg_class(lua_State* lua,
                      const char* name,
                      const struct luaL_Reg* module)
{
  luaL_newmetatable(lua, name);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  luaL_register(lua, NULL, module);
  lua_pushvalue(lua, -2); // Copy sax_equal on top
  lua_setfield(lua, -2, "__eq");
  lua_pop(lua, 1); // Pop table
}

static void reg_module(lua_State* lua,
                       const char* name,
                       const lua_CFunction module)
{
  lua_newtable(lua);
  lua_pushcfunction(lua, module);
  lua_setfield(lua, -2, "new");
  lua_setfield(lua, -2, name);
}

int luaopen_sax(lua_State* lua)
{
#ifdef LUA_SANDBOX
  lua_newtable(lua);
  lsb_add_serialize_function(lua, serialize_sax);
  lsb_add_output_function(lua, output_sax);
  lua_replace(lua, LUA_ENVIRONINDEX);
#endif // LUA_SANDBOX

  /* We're registering sax_equal separately since it's the only way to make lua
   * aware that it's exactly the same function each time
   * (otherwise it doesn't get called on different object types) */
  lua_pushcfunction(lua, sax_equal);

  reg_class(lua, mozsvc_sax_window, saxlib_win);
  reg_class(lua, mozsvc_sax_word, saxlib_word);

  lua_newtable(lua);
  luaL_register(lua, NULL, saxlib_f);
  reg_module(lua, mozsvc_sax_word_suffix, sax_new_word);
  reg_module(lua, mozsvc_sax_win_suffix, sax_new_window);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, LUA_GLOBALSINDEX, mozsvc_sax_table);

  return 1;
}
