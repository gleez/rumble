/*$I0 */
<< << << < HEAD << << << < HEAD == == == =

/*$T rumble_lua.c GC 1.140 02/17/11 19:05:38 */
>> >> >> > 7 c6078b307d012f3ab1c0cc605edd7fa50d50252 == == == =

/*$T rumble_lua.c GC 1.140 02/17/11 19:05:38 */
>> >> >> > 43 a381c615c91573f80c48bfd2769fa03b2c5644

/*$6
 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 */

#include "rumble.h"
#include "rumble_lua.h"
#ifdef RUMBLE_LUA
#   define FOO "Rumble"
typedef struct Rumble
{
    int x;
    int y;
    masterHandle * m;
}
rumble_lua_userdata;

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static rumble_lua_userdata *toFoo(lua_State *L, int index) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_lua_userdata *bar = (rumble_lua_userdata *) lua_touserdata(L, index);
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    if (bar == NULL) luaL_typerror(L, index, FOO);
    return (bar);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static rumble_lua_userdata *checkFoo(lua_State *L, int index) {

    /*~~~~~~~~~~~~~~~~~~~~~*/
    rumble_lua_userdata *bar;
    /*~~~~~~~~~~~~~~~~~~~~~*/

    luaL_checktype(L, index, LUA_TUSERDATA);
    bar = (rumble_lua_userdata *) luaL_checkudata(L, index, FOO);
    if (bar == NULL) luaL_typerror(L, index, FOO);
    return (bar);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static rumble_lua_userdata *pushFoo(lua_State *L) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_lua_userdata *bar = (rumble_lua_userdata *) lua_newuserdata(L, sizeof(rumble_lua_userdata));
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    luaL_getmetatable(L, FOO);
    lua_setmetatable(L, -2);
    return (bar);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static int Foo_new(lua_State *L) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    int                 x = luaL_optint(L, 1, 0);
    int                 y = luaL_optint(L, 2, 0);
    rumble_lua_userdata *bar = pushFoo(L);
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    bar->x = x;
    bar->y = y;
    return (1);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static int Foo_yourCfunction(lua_State *L) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_lua_userdata *bar = checkFoo(L, 1);
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    printf("this is yourCfunction\t");
    lua_pushnumber(L, bar->x);
    lua_pushnumber(L, bar->y);
    return (2);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static int Foo_setx(lua_State *L) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_lua_userdata *bar = checkFoo(L, 1);
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    bar->x = luaL_checkint(L, 2);
    lua_settop(L, 1);
    return (1);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static int Foo_sety(lua_State *L) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_lua_userdata *bar = checkFoo(L, 1);
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    bar->y = luaL_checkint(L, 2);
    lua_settop(L, 1);
    return (1);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static int Foo_add(lua_State *L) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_lua_userdata *bar1 = checkFoo(L, 1);
    rumble_lua_userdata *bar2 = checkFoo(L, 2);
    rumble_lua_userdata *sum = pushFoo(L);
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    sum->x = bar1->x + bar2->x;
    sum->y = bar1->y + bar2->y;
    return (1);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static int Foo_dot(lua_State *L) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_lua_userdata *bar1 = checkFoo(L, 1);
    rumble_lua_userdata *bar2 = checkFoo(L, 2);
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    lua_pushnumber(L, bar1->x * bar2->x + bar1->y * bar2->y);
    return (1);
}

static const luaL_reg   Foo_methods[] =
{
    { "new", Foo_new },
    { "yourCfunction", Foo_yourCfunction },
    { "setx", Foo_setx },
    { "sety", Foo_sety },
    { "add", Foo_add },
    { "dot", Foo_dot },
    { 0, 0 }
};

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static int Foo_gc(lua_State *L) {
    printf("bye, bye, bar = %p\n", toFoo(L, 1));
    return (0);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
static int Foo_tostring(lua_State *L) {

    /*~~~~~~~~~~~~~*/
    char    buff[32];
    /*~~~~~~~~~~~~~*/

    sprintf(buff, "%p", toFoo(L, 1));
    lua_pushfstring(L, "Foo (%s)", buff);
    return (1);
}

static const luaL_reg   Foo_meta[] = { { "__gc", Foo_gc }, { "__tostring", Foo_tostring }, { "__add", Foo_add }, { 0, 0 } };

/*
 =======================================================================================================================
 =======================================================================================================================
 */
int Foo_register(lua_State *L) {
    luaL_openlib(L, FOO, Foo_methods, 0);   /* create methods table, add it to the globals */
    luaL_newmetatable(L, FOO);          /* create metatable for Foo, and add it to the Lua registry */
    luaL_openlib(L, 0, Foo_meta, 0);    /* fill metatable */
    lua_pushliteral(L, "__index");
    lua_pushvalue(L, -3);   /* dup methods table */
    lua_rawset(L, -3);      /* metatable.__index = methods */
    lua_pushliteral(L, "__metatable");
    lua_pushvalue(L, -3);   /* dup methods table */
    lua_rawset(L, -3);      /* hide metatable: metatable.__metatable = methods */
    lua_pop(L, 1);          /* drop metatable */
    return (1); /* return methods on the stack */
}
#endif
