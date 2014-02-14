/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"
#include "sha1.h"
#include "rand.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <ctype.h>
#include <math.h>

char *redisProtocolToLuaType_Int(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Bulk(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Status(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Error(lua_State *lua, char *reply);
char *redisProtocolToLuaType_MultiBulk(lua_State *lua, char *reply);
int redis_math_random (lua_State *L);
int redis_math_randomseed (lua_State *L);
void sha1hex(char *digest, char *script, size_t len);

/* Take a Redis reply in the Redis protocol format and convert it into a
 * Lua type. Thanks to this function, and the introduction of not connected
 * clients, it is trivial to implement the redis() lua function.
 *
 * Basically we take the arguments, execute the Redis command in the context
 * of a non connected client, then take the generated reply and convert it
 * into a suitable Lua type. With this trick the scripting feature does not
 * need the introduction of a full Redis internals API. Basically the script
 * is like a normal client that bypasses all the slow I/O paths.
 *
 * Note: in this function we do not do any sanity check as the reply is
 * generated by Redis directly. This allows us to go faster.
 * The reply string can be altered during the parsing as it is discarded
 * after the conversion is completed.
 *
 * Errors are returned as a table with a single 'err' field set to the
 * error string.
 */

char *redisProtocolToLuaType(lua_State *lua, char* reply) {
    char *p = reply;

    switch(*p) {
    case ':':
        p = redisProtocolToLuaType_Int(lua,reply);
        break;
    case '$':
        p = redisProtocolToLuaType_Bulk(lua,reply);
        break;
    case '+':
        p = redisProtocolToLuaType_Status(lua,reply);
        break;
    case '-':
        p = redisProtocolToLuaType_Error(lua,reply);
        break;
    case '*':
        p = redisProtocolToLuaType_MultiBulk(lua,reply);
        break;
    }
    return p;
}

char *redisProtocolToLuaType_Int(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long value;

    string2ll(reply+1,p-reply-1,&value);
    lua_pushnumber(lua,(lua_Number)value);
    return p+2;
}

char *redisProtocolToLuaType_Bulk(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long bulklen;

    string2ll(reply+1,p-reply-1,&bulklen);
    if (bulklen == -1) {
        lua_pushboolean(lua,0);
        return p+2;
    } else {
        lua_pushlstring(lua,p+2,bulklen);
        return p+2+bulklen+2;
    }
}

char *redisProtocolToLuaType_Status(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');

    lua_newtable(lua);
    lua_pushstring(lua,"ok");
    lua_pushlstring(lua,reply+1,p-reply-1);
    lua_settable(lua,-3);
    return p+2;
}

char *redisProtocolToLuaType_Error(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');

    lua_newtable(lua);
    lua_pushstring(lua,"err");
    lua_pushlstring(lua,reply+1,p-reply-1);
    lua_settable(lua,-3);
    return p+2;
}

char *redisProtocolToLuaType_MultiBulk(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long mbulklen;
    int j = 0;

    string2ll(reply+1,p-reply-1,&mbulklen);
    p += 2;
    if (mbulklen == -1) {
        lua_pushboolean(lua,0);
        return p;
    }
    lua_newtable(lua);
    for (j = 0; j < mbulklen; j++) {
        lua_pushnumber(lua,j+1);
        p = redisProtocolToLuaType(lua,p);
        lua_settable(lua,-3);
    }
    return p;
}

void luaPushError(lua_State *lua, char *error) {
    lua_Debug dbg;

    lua_newtable(lua);
    lua_pushstring(lua,"err");

    /* Attempt to figure out where this function was called, if possible */
    if(lua_getstack(lua, 1, &dbg) && lua_getinfo(lua, "nSl", &dbg)) {
        sds msg = sdscatprintf(sdsempty(), "%s: %d: %s",
            dbg.source, dbg.currentline, error);
        lua_pushstring(lua, msg);
        sdsfree(msg);
    } else {
        lua_pushstring(lua, error);
    }
    lua_settable(lua,-3);
}

/* Sort the array currently in the stack. We do this to make the output
 * of commands like KEYS or SMEMBERS something deterministic when called
 * from Lua (to play well with AOf/replication).
 *
 * The array is sorted using table.sort itself, and assuming all the
 * list elements are strings. */
void luaSortArray(lua_State *lua) {
    /* Initial Stack: array */
    lua_getglobal(lua,"table");
    lua_pushstring(lua,"sort");
    lua_gettable(lua,-2);       /* Stack: array, table, table.sort */
    lua_pushvalue(lua,-3);      /* Stack: array, table, table.sort, array */
    if (lua_pcall(lua,1,0,0)) {
        /* Stack: array, table, error */

        /* We are not interested in the error, we assume that the problem is
         * that there are 'false' elements inside the array, so we try
         * again with a slower function but able to handle this case, that
         * is: table.sort(table, __redis__compare_helper) */
        lua_pop(lua,1);             /* Stack: array, table */
        lua_pushstring(lua,"sort"); /* Stack: array, table, sort */
        lua_gettable(lua,-2);       /* Stack: array, table, table.sort */
        lua_pushvalue(lua,-3);      /* Stack: array, table, table.sort, array */
        lua_getglobal(lua,"__redis__compare_helper");
        /* Stack: array, table, table.sort, array, __redis__compare_helper */
        lua_call(lua,2,0);
    }
    /* Stack: array (sorted), table */
    lua_pop(lua,1);             /* Stack: array (sorted) */
}

int luaRedisGenericCommand(lua_State *lua, int raise_error) {
    int j, argc = lua_gettop(lua);
    struct redisCommand *cmd;
    robj **argv;
    redisClient *c = server.lua_client;
    sds reply;

    /* Require at least one argument */
    if (argc == 0) {
        luaPushError(lua,
            "Please specify at least one argument for redis.call()");
        return 1;
    }

    /* Build the arguments vector */
    argv = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        if (!lua_isstring(lua,j+1)) break;
        argv[j] = createStringObject((char*)lua_tostring(lua,j+1),
                                     lua_rawlen(lua,j+1));
    }

    /* Check if one of the arguments passed by the Lua script
     * is not a string or an integer (lua_isstring() return true for
     * integers as well). */
    if (j != argc) {
        j--;
        while (j >= 0) {
            decrRefCount(argv[j]);
            j--;
        }
        zfree(argv);
        luaPushError(lua,
            "Lua redis() command arguments must be strings or integers");
        return 1;
    }

    /* Setup our fake client for command execution */
    c->argv = argv;
    c->argc = argc;

    /* Command lookup */
    cmd = lookupCommand(argv[0]->ptr);
    if (!cmd || ((cmd->arity > 0 && cmd->arity != argc) ||
                   (argc < -cmd->arity)))
    {
        if (cmd)
            luaPushError(lua,
                "Wrong number of args calling Redis command From Lua script");
        else
            luaPushError(lua,"Unknown Redis command called from Lua script");
        goto cleanup;
    }

    /* There are commands that are not allowed inside scripts. */
    if (cmd->flags & REDIS_CMD_NOSCRIPT) {
        luaPushError(lua, "This Redis command is not allowed from scripts");
        goto cleanup;
    }

    /* Write commands are forbidden against read-only slaves, or if a
     * command marked as non-deterministic was already called in the context
     * of this script. */
    if (cmd->flags & REDIS_CMD_WRITE) {
        if (server.lua_random_dirty) {
            luaPushError(lua,
                "Write commands not allowed after non deterministic commands");
            goto cleanup;
        } else if (server.masterhost && server.repl_slave_ro &&
                   !server.loading &&
                   !(server.lua_caller->flags & REDIS_MASTER))
        {
            luaPushError(lua, shared.roslaveerr->ptr);
            goto cleanup;
        } else if (server.stop_writes_on_bgsave_err &&
                   server.saveparamslen > 0 &&
                   server.lastbgsave_status == REDIS_ERR)
        {
            luaPushError(lua, shared.bgsaveerr->ptr);
            goto cleanup;
        }
    }

    /* If we reached the memory limit configured via maxmemory, commands that
     * could enlarge the memory usage are not allowed, but only if this is the
     * first write in the context of this script, otherwise we can't stop
     * in the middle. */
    if (server.maxmemory && server.lua_write_dirty == 0 &&
        (cmd->flags & REDIS_CMD_DENYOOM))
    {
        if (freeMemoryIfNeeded() == REDIS_ERR) {
            luaPushError(lua, shared.oomerr->ptr);
            goto cleanup;
        }
    }

    if (cmd->flags & REDIS_CMD_RANDOM) server.lua_random_dirty = 1;
    if (cmd->flags & REDIS_CMD_WRITE) server.lua_write_dirty = 1;

    /* Run the command */
    c->cmd = cmd;
    call(c,REDIS_CALL_SLOWLOG | REDIS_CALL_STATS);

    /* Convert the result of the Redis command into a suitable Lua type.
     * The first thing we need is to create a single string from the client
     * output buffers. */
    reply = sdsempty();
    if (c->bufpos) {
        reply = sdscatlen(reply,c->buf,c->bufpos);
        c->bufpos = 0;
    }
    while(listLength(c->reply)) {
        robj *o = listNodeValue(listFirst(c->reply));

        reply = sdscatlen(reply,o->ptr,sdslen(o->ptr));
        listDelNode(c->reply,listFirst(c->reply));
    }
    if (raise_error && reply[0] != '-') raise_error = 0;
    redisProtocolToLuaType(lua,reply);
    /* Sort the output array if needed, assuming it is a non-null multi bulk
     * reply as expected. */
    if ((cmd->flags & REDIS_CMD_SORT_FOR_SCRIPT) &&
        (reply[0] == '*' && reply[1] != '-')) {
            luaSortArray(lua);
    }
    sdsfree(reply);
    c->reply_bytes = 0;

cleanup:
    /* Clean up. Command code may have changed argv/argc so we use the
     * argv/argc of the client instead of the local variables. */
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    zfree(c->argv);

    if (raise_error) {
        /* If we are here we should have an error in the stack, in the
         * form of a table with an "err" field. Extract the string to
         * return the plain error. */
        lua_pushstring(lua,"err");
        lua_gettable(lua,-2);
        return lua_error(lua);
    }
    return 1;
}

int luaRedisCallCommand(lua_State *lua) {
    return luaRedisGenericCommand(lua,1);
}

int luaRedisPCallCommand(lua_State *lua) {
    return luaRedisGenericCommand(lua,0);
}

/* This adds redis.sha1hex(string) to Lua scripts using the same hashing
 * function used for sha1ing lua scripts. */
int luaRedisSha1hexCommand(lua_State *lua) {
    int argc = lua_gettop(lua);
    char digest[41];
    size_t len;
    char *s;

    if (argc != 1) {
        luaPushError(lua, "wrong number of arguments");
        return 1;
    }

    s = (char*)lua_tolstring(lua,1,&len);
    sha1hex(digest,s,len);
    lua_pushstring(lua,digest);
    return 1;
}

/* Returns a table with a single field 'field' set to the string value
 * passed as argument. This helper function is handy when returning
 * a Redis Protocol error or status reply from Lua:
 *
 * return redis.error_reply("ERR Some Error")
 * return redis.status_reply("ERR Some Error")
 */
int luaRedisReturnSingleFieldTable(lua_State *lua, char *field) {
    if (lua_gettop(lua) != 1 || lua_type(lua,-1) != LUA_TSTRING) {
        luaPushError(lua, "wrong number or type of arguments");
        return 1;
    }

    lua_newtable(lua);
    lua_pushstring(lua, field);
    lua_pushvalue(lua, -3);
    lua_settable(lua, -3);
    return 1;
}

int luaRedisErrorReplyCommand(lua_State *lua) {
    return luaRedisReturnSingleFieldTable(lua,"err");
}

int luaRedisStatusReplyCommand(lua_State *lua) {
    return luaRedisReturnSingleFieldTable(lua,"ok");
}

int luaLogCommand(lua_State *lua) {
    int j, argc = lua_gettop(lua);
    int level;
    sds log;

    if (argc < 2) {
        luaPushError(lua, "redis.log() requires two arguments or more.");
        return 1;
    } else if (!lua_isnumber(lua,-argc)) {
        luaPushError(lua, "First argument must be a number (log level).");
        return 1;
    }
    level = lua_tonumber(lua,-argc);
    if (level < REDIS_DEBUG || level > REDIS_WARNING) {
        luaPushError(lua, "Invalid debug level.");
        return 1;
    }

    /* Glue together all the arguments */
    log = sdsempty();
    for (j = 1; j < argc; j++) {
        size_t len;
        char *s;

        s = (char*)lua_tolstring(lua,(-argc)+j,&len);
        if (s) {
            if (j != 1) log = sdscatlen(log," ",1);
            log = sdscatlen(log,s,len);
        }
    }
    redisLogRaw(level,log);
    sdsfree(log);
    return 0;
}

void luaMaskCountHook(lua_State *lua, lua_Debug *ar) {
    long long elapsed;
    REDIS_NOTUSED(ar);
    REDIS_NOTUSED(lua);

    elapsed = mstime() - server.lua_time_start;
    if (elapsed >= server.lua_time_limit && server.lua_timedout == 0) {
        redisLog(REDIS_WARNING,"Lua slow script detected: still in execution after %lld milliseconds. You can try killing the script using the SCRIPT KILL command.",elapsed);
        server.lua_timedout = 1;
        /* Once the script timeouts we reenter the event loop to permit others
         * to call SCRIPT KILL or SHUTDOWN NOSAVE if needed. For this reason
         * we need to mask the client executing the script from the event loop.
         * If we don't do that the client may disconnect and could no longer be
         * here when the EVAL command will return. */
         aeDeleteFileEvent(server.el, server.lua_caller->fd, AE_READABLE);
    }
    if (server.lua_timedout)
        aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
    if (server.lua_kill) {
        redisLog(REDIS_WARNING,"Lua script killed by user with SCRIPT KILL.");
        lua_pushstring(lua,"Script killed by user with SCRIPT KILL...");
        lua_error(lua);
    }
}

void luaLoadLib(lua_State *lua, const char *libname, lua_CFunction luafunc) {
  lua_pushcfunction(lua, luafunc);
  lua_pushstring(lua, libname);
  lua_call(lua, 1, 0);
}

LUALIB_API int (luaopen_cjson) (lua_State *L);
LUALIB_API int (luaopen_struct) (lua_State *L);
LUALIB_API int (luaopen_cmsgpack) (lua_State *L);

void luaLoadLibraries(lua_State *lua) {
    luaLoadLib(lua, "", luaopen_base);
    luaLoadLib(lua, LUA_TABLIBNAME, luaopen_table);
    luaLoadLib(lua, LUA_STRLIBNAME, luaopen_string);
    luaLoadLib(lua, LUA_MATHLIBNAME, luaopen_math);
    luaLoadLib(lua, LUA_DBLIBNAME, luaopen_debug);
    luaLoadLib(lua, "cjson", luaopen_cjson);
    luaLoadLib(lua, "struct", luaopen_struct);
    luaLoadLib(lua, "cmsgpack", luaopen_cmsgpack);

#if 0 /* Stuff that we don't load currently, for sandboxing concerns. */
    luaLoadLib(lua, LUA_LOADLIBNAME, luaopen_package);
    luaLoadLib(lua, LUA_OSLIBNAME, luaopen_os);
#endif
}

/* Remove a functions that we don't want to expose to the Redis scripting
 * environment. */
void luaRemoveUnsupportedFunctions(lua_State *lua) {
    lua_pushnil(lua);
    lua_setglobal(lua,"loadfile");
}

/* This function installs metamethods in the global table _G that prevent
 * the creation of globals accidentally.
 *
 * It should be the last to be called in the scripting engine initialization
 * sequence, because it may interact with creation of globals. */
void scriptingEnableGlobalsProtection(lua_State *lua) {
    char *s[32];
    sds code = sdsempty();
    int j = 0;

    /* strict.lua from: http://metalua.luaforge.net/src/lib/strict.lua.html.
     * Modified to be adapted to Redis. */
    s[j++]="local mt = {}\n";
    s[j++]="setmetatable(_G, mt)\n";
    s[j++]="mt.__newindex = function (t, n, v)\n";
    s[j++]="  if debug.getinfo(2) then\n";
    s[j++]="    local w = debug.getinfo(2, \"S\").what\n";
    s[j++]="    if w ~= \"main\" and w ~= \"C\" then\n";
    s[j++]="      error(\"Script attempted to create global variable '\"..tostring(n)..\"'\", 2)\n";
    s[j++]="    end\n";
    s[j++]="  end\n";
    s[j++]="  rawset(t, n, v)\n";
    s[j++]="end\n";
    s[j++]="mt.__index = function (t, n)\n";
    s[j++]="  if debug.getinfo(2) and debug.getinfo(2, \"S\").what ~= \"C\" then\n";
    s[j++]="    error(\"Script attempted to access unexisting global variable '\"..tostring(n)..\"'\", 2)\n";
    s[j++]="  end\n";
    s[j++]="  return rawget(t, n)\n";
    s[j++]="end\n";
    s[j++]=NULL;

    for (j = 0; s[j] != NULL; j++) code = sdscatlen(code,s[j],strlen(s[j]));
    luaL_loadbuffer(lua,code,sdslen(code),"@enable_strict_lua");
    lua_pcall(lua,0,0,0);
    sdsfree(code);
}

/* Initialize the scripting environment.
 * It is possible to call this function to reset the scripting environment
 * assuming that we call scriptingRelease() before.
 * See scriptingReset() for more information. */
void scriptingInit(void) {
    lua_State *lua = luaL_newstate();

    luaLoadLibraries(lua);
    luaRemoveUnsupportedFunctions(lua);

    /* Initialize a dictionary we use to map SHAs to scripts.
     * This is useful for replication, as we need to replicate EVALSHA
     * as EVAL, so we need to remember the associated script. */
    server.lua_scripts = dictCreate(&shaScriptObjectDictType,NULL);

    /* Register the redis commands table and fields */
    lua_newtable(lua);

    /* redis.call */
    lua_pushstring(lua,"call");
    lua_pushcfunction(lua,luaRedisCallCommand);
    lua_settable(lua,-3);

    /* redis.pcall */
    lua_pushstring(lua,"pcall");
    lua_pushcfunction(lua,luaRedisPCallCommand);
    lua_settable(lua,-3);

    /* redis.log and log levels. */
    lua_pushstring(lua,"log");
    lua_pushcfunction(lua,luaLogCommand);
    lua_settable(lua,-3);

    lua_pushstring(lua,"LOG_DEBUG");
    lua_pushnumber(lua,REDIS_DEBUG);
    lua_settable(lua,-3);

    lua_pushstring(lua,"LOG_VERBOSE");
    lua_pushnumber(lua,REDIS_VERBOSE);
    lua_settable(lua,-3);

    lua_pushstring(lua,"LOG_NOTICE");
    lua_pushnumber(lua,REDIS_NOTICE);
    lua_settable(lua,-3);

    lua_pushstring(lua,"LOG_WARNING");
    lua_pushnumber(lua,REDIS_WARNING);
    lua_settable(lua,-3);

    /* redis.sha1hex */
    lua_pushstring(lua, "sha1hex");
    lua_pushcfunction(lua, luaRedisSha1hexCommand);
    lua_settable(lua, -3);

    /* redis.error_reply and redis.status_reply */
    lua_pushstring(lua, "error_reply");
    lua_pushcfunction(lua, luaRedisErrorReplyCommand);
    lua_settable(lua, -3);
    lua_pushstring(lua, "status_reply");
    lua_pushcfunction(lua, luaRedisStatusReplyCommand);
    lua_settable(lua, -3);

    /* Finally set the table as 'redis' global var. */
    lua_setglobal(lua,"redis");

    /* Replace math.random and math.randomseed with our implementations. */
    // lua_getglobal(lua,"math");

    // lua_pushstring(lua,"random");
    // lua_pushcfunction(lua,redis_math_random);
    // lua_settable(lua,-3);

    // lua_pushstring(lua,"randomseed");
    // lua_pushcfunction(lua,redis_math_randomseed);
    // lua_settable(lua,-3);

    // lua_setglobal(lua,"math");

    /* Add a helper function that we use to sort the multi bulk output of non
     * deterministic commands, when containing 'false' elements. */
    {
        char *compare_func =    "function __redis__compare_helper(a,b)\n"
                                "  if a == false then a = '' end\n"
                                "  if b == false then b = '' end\n"
                                "  return a<b\n"
                                "end\n";
        luaL_loadbuffer(lua,compare_func,strlen(compare_func),"@cmp_func_def");
        lua_pcall(lua,0,0,0);
    }

    /* Add a helper function we use for pcall error reporting.
     * Note that when the error is in the C function we want to report the
     * information about the caller, that's what makes sense from the point
     * of view of the user debugging a script. */
    {
        char *errh_func =       "function __redis__err__handler(err)\n"
                                "  local i = debug.getinfo(2,'nSl')\n"
                                "  if i and i.what == 'C' then\n"
                                "    i = debug.getinfo(3,'nSl')\n"
                                "  end\n"
                                "  if i then\n"
                                "    return i.source .. ':' .. i.currentline .. ': ' .. err\n"
                                "  else\n"
                                "    return err\n"
                                "  end\n"
                                "end\n";
        luaL_loadbuffer(lua,errh_func,strlen(errh_func),"@err_handler_def");
        redisAssert(lua_pcall(lua,0,0,0) == LUA_OK);
    }

    /* Create the (non connected) client that we use to execute Redis commands
     * inside the Lua interpreter.
     * Note: there is no need to create it again when this function is called
     * by scriptingReset(). */
    if (server.lua_client == NULL) {
        server.lua_client = createClient(-1);
        server.lua_client->flags |= REDIS_LUA_CLIENT;
    }

    /* Lua beginners ofter don't use "local", this is likely to introduce
     * subtle bugs in their code. To prevent problems we protect accesses
     * to global variables. */
    // scriptingEnableGlobalsProtection(lua);

    server.lua = lua;
}

/* Release resources related to Lua scripting.
 * This function is used in order to reset the scripting environment. */
void scriptingRelease(void) {
    dictRelease(server.lua_scripts);
    lua_close(server.lua);
}

void scriptingReset(void) {
    scriptingRelease();
    scriptingInit();
}

/* Perform the SHA1 of the input string. We use this both for hashing script
 * bodies in order to obtain the Lua function name, and in the implementation
 * of redis.sha1().
 *
 * 'digest' should point to a 41 bytes buffer: 40 for SHA1 converted into an
 * hexadecimal number, plus 1 byte for null term. */
void sha1hex(char *digest, char *script, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    char *cset = "0123456789abcdef";
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx,(unsigned char*)script,len);
    SHA1Final(hash,&ctx);

    for (j = 0; j < 20; j++) {
        digest[j*2] = cset[((hash[j]&0xF0)>>4)];
        digest[j*2+1] = cset[(hash[j]&0xF)];
    }
    digest[40] = '\0';
}

void luaReplyToRedisReply(redisClient *c, lua_State *lua) {
    int t = lua_type(lua,-1);

    switch(t) {
    case LUA_TSTRING:
        addReplyBulkCBuffer(c,(char*)lua_tostring(lua,-1),lua_rawlen(lua,-1));
        break;
    case LUA_TBOOLEAN:
        addReply(c,lua_toboolean(lua,-1) ? shared.cone : shared.nullbulk);
        break;
    case LUA_TNUMBER:
        addReplyLongLong(c,(long long)lua_tonumber(lua,-1));
        break;
    case LUA_TTABLE:
        /* We need to check if it is an array, an error, or a status reply.
         * Error are returned as a single element table with 'err' field.
         * Status replies are returned as single element table with 'ok' field */
        lua_pushstring(lua,"err");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            sds err = sdsnew(lua_tostring(lua,-1));
            sdsmapchars(err,"\r\n","  ",2);
            addReplySds(c,sdscatprintf(sdsempty(),"-%s\r\n",err));
            sdsfree(err);
            lua_pop(lua,2);
            return;
        }

        lua_pop(lua,1);
        lua_pushstring(lua,"ok");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            sds ok = sdsnew(lua_tostring(lua,-1));
            sdsmapchars(ok,"\r\n","  ",2);
            addReplySds(c,sdscatprintf(sdsempty(),"+%s\r\n",ok));
            sdsfree(ok);
            lua_pop(lua,1);
        } else {
            void *replylen = addDeferredMultiBulkLength(c);
            int j = 1, mbulklen = 0;

            lua_pop(lua,1); /* Discard the 'ok' field value we popped */
            while(1) {
                lua_pushnumber(lua,j++);
                lua_gettable(lua,-2);
                t = lua_type(lua,-1);
                if (t == LUA_TNIL) {
                    lua_pop(lua,1);
                    break;
                }
                luaReplyToRedisReply(c, lua);
                mbulklen++;
            }
            setDeferredMultiBulkLength(c,replylen,mbulklen);
        }
        break;
    default:
        addReply(c,shared.nullbulk);
    }
    lua_pop(lua,1);
}

/* Set an array of Redis String Objects as a Lua array (table) stored into a
 * global variable. */
void luaSetGlobalArray(lua_State *lua, char *var, robj **elev, int elec) {
    int j;

    lua_newtable(lua);
    for (j = 0; j < elec; j++) {
        lua_pushlstring(lua,(char*)elev[j]->ptr,sdslen(elev[j]->ptr));
        lua_rawseti(lua,-2,j+1);
    }
    lua_setglobal(lua,var);
}

/* Define a lua function with the specified function name and body.
 * The function name musts be a 2 characters long string, since all the
 * functions we defined in the Lua context are in the form:
 *
 *   f_<hex sha1 sum>
 *
 * On success REDIS_OK is returned, and nothing is left on the Lua stack.
 * On error REDIS_ERR is returned and an appropriate error is set in the
 * client context. */
int luaCreateFunction(redisClient *c, lua_State *lua, char *funcname, robj *body) {
    sds funcdef = sdsempty();

    funcdef = sdscat(funcdef,"function ");
    funcdef = sdscatlen(funcdef,funcname,42);
    funcdef = sdscatlen(funcdef,"() ",3);
    funcdef = sdscatlen(funcdef,body->ptr,sdslen(body->ptr));
    funcdef = sdscatlen(funcdef," end",4);

    if (luaL_loadbuffer(lua,funcdef,sdslen(funcdef),"@user_script")) {
        addReplyErrorFormat(c,"Error compiling script (new function): %s\n",
            lua_tostring(lua,-1));
        lua_pop(lua,1);
        sdsfree(funcdef);
        return REDIS_ERR;
    }
    sdsfree(funcdef);
    if (lua_pcall(lua,0,0,0) != LUA_OK) {
        addReplyErrorFormat(c,"Error running script (new function): %s\n",
            lua_tostring(lua,-1));
        lua_pop(lua,1);
        return REDIS_ERR;
    }

    /* We also save a SHA1 -> Original script map in a dictionary
     * so that we can replicate / write in the AOF all the
     * EVALSHA commands as EVAL using the original script. */
    {
        int retval = dictAdd(server.lua_scripts,
                             sdsnewlen(funcname+2,40),body);
        redisAssertWithInfo(c,NULL,retval == DICT_OK);
        incrRefCount(body);
    }
    return REDIS_OK;
}


void evalGenericCommand(redisClient *c, int evalsha) {
    lua_State *lua = server.lua;
    char funcname[43];
    long long numkeys;
    int delhook = 0, err;

    /* We want the same PRNG sequence at every call so that our PRNG is
     * not affected by external state. */
    redisSrand48(0);

    /* We set this flag to zero to remember that so far no random command
     * was called. This way we can allow the user to call commands like
     * SRANDMEMBER or RANDOMKEY from Lua scripts as far as no write command
     * is called (otherwise the replication and AOF would end with non
     * deterministic sequences).
     *
     * Thanks to this flag we'll raise an error every time a write command
     * is called after a random command was used. */
    server.lua_random_dirty = 0;
    server.lua_write_dirty = 0;

    /* Get the number of arguments that are keys */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&numkeys,NULL) != REDIS_OK)
        return;
    if (numkeys > (c->argc - 3)) {
        addReplyError(c,"Number of keys can't be greater than number of args");
        return;
    }

    /* We obtain the script SHA1, then check if this function is already
     * defined into the Lua state */
    funcname[0] = 'f';
    funcname[1] = '_';
    if (!evalsha) {
        /* Hash the code if this is an EVAL call */
        sha1hex(funcname+2,c->argv[1]->ptr,sdslen(c->argv[1]->ptr));
    } else {
        /* We already have the SHA if it is a EVALSHA */
        int j;
        char *sha = c->argv[1]->ptr;

        for (j = 0; j < 40; j++)
            funcname[j+2] = tolower(sha[j]);
        funcname[42] = '\0';
    }

    /* Push the pcall error handler function on the stack. */
    lua_getglobal(lua, "__redis__err__handler");

    /* Try to lookup the Lua function */
    lua_getglobal(lua, funcname);
    if (lua_isnil(lua,-1)) {
        lua_pop(lua,1); /* remove the nil from the stack */
        /* Function not defined... let's define it if we have the
         * body of the function. If this is an EVALSHA call we can just
         * return an error. */
        if (evalsha) {
            lua_pop(lua,1); /* remove the error handler from the stack. */
            addReply(c, shared.noscripterr);
            return;
        }
        if (luaCreateFunction(c,lua,funcname,c->argv[1]) == REDIS_ERR) {
            lua_pop(lua,1); /* remove the error handler from the stack. */
            /* The error is sent to the client by luaCreateFunction()
             * itself when it returns REDIS_ERR. */
            return;
        }
        /* Now the following is guaranteed to return non nil */
        lua_getglobal(lua, funcname);
        redisAssert(!lua_isnil(lua,-1));
    }

    /* Populate the argv and keys table accordingly to the arguments that
     * EVAL received. */
    luaSetGlobalArray(lua,"KEYS",c->argv+3,numkeys);
    luaSetGlobalArray(lua,"ARGV",c->argv+3+numkeys,c->argc-3-numkeys);

    /* Select the right DB in the context of the Lua client */
    selectDb(server.lua_client,c->db->id);
    
    /* Set a hook in order to be able to stop the script execution if it
     * is running for too much time.
     * We set the hook only if the time limit is enabled as the hook will
     * make the Lua script execution slower. */
    server.lua_caller = c;
    server.lua_time_start = mstime();
    server.lua_kill = 0;
    if (server.lua_time_limit > 0 && server.masterhost == NULL) {
        lua_sethook(lua,luaMaskCountHook,LUA_MASKCOUNT,100000);
        delhook = 1;
    }

    /* At this point whether this script was never seen before or if it was
     * already defined, we can call it. We have zero arguments and expect
     * a single return value. */
    err = lua_pcall(lua,0,1,-2);

    /* Perform some cleanup that we need to do both on error and success. */
    if (delhook) lua_sethook(lua,luaMaskCountHook,0,0); /* Disable hook */
    if (server.lua_timedout) {
        server.lua_timedout = 0;
        /* Restore the readable handler that was unregistered when the
         * script timeout was detected. */
        aeCreateFileEvent(server.el,c->fd,AE_READABLE,
                          readQueryFromClient,c);
    }
    server.lua_caller = NULL;
    selectDb(c,server.lua_client->db->id); /* set DB ID from Lua client */
    lua_gc(lua,LUA_GCSTEP,1);

    if (err) {
        addReplyErrorFormat(c,"Error running script (call to %s): %s\n",
            funcname, lua_tostring(lua,-1));
        lua_pop(lua,2); /* Consume the Lua reply and remove error handler. */
    } else {
        /* On success convert the Lua return value into Redis protocol, and
         * send it to * the client. */
        luaReplyToRedisReply(c,lua); /* Convert and consume the reply. */
        lua_pop(lua,1); /* Remove the error handler. */
    }

    /* EVALSHA should be propagated to Slave and AOF file as full EVAL, unless
     * we are sure that the script was already in the context of all the
     * attached slaves *and* the current AOF file if enabled.
     *
     * To do so we use a cache of SHA1s of scripts that we already propagated
     * as full EVAL, that's called the Replication Script Cache.
     *
     * For repliation, everytime a new slave attaches to the master, we need to
     * flush our cache of scripts that can be replicated as EVALSHA, while
     * for AOF we need to do so every time we rewrite the AOF file. */
    if (evalsha) {
        if (!replicationScriptCacheExists(c->argv[1]->ptr)) {
            /* This script is not in our script cache, replicate it as
             * EVAL, then add it into the script cache, as from now on
             * slaves and AOF know about it. */
            robj *script = dictFetchValue(server.lua_scripts,c->argv[1]->ptr);

            replicationScriptCacheAdd(c->argv[1]->ptr);
            redisAssertWithInfo(c,NULL,script != NULL);
            rewriteClientCommandArgument(c,0,
                resetRefCount(createStringObject("EVAL",4)));
            rewriteClientCommandArgument(c,1,script);
            forceCommandPropagation(c,REDIS_PROPAGATE_REPL|REDIS_PROPAGATE_AOF);
        }
    }
}

void evalCommand(redisClient *c) {
    evalGenericCommand(c,0);
}

void evalShaCommand(redisClient *c) {
    if (sdslen(c->argv[1]->ptr) != 40) {
        /* We know that a match is not possible if the provided SHA is
         * not the right length. So we return an error ASAP, this way
         * evalGenericCommand() can be implemented without string length
         * sanity check */
        addReply(c, shared.noscripterr);
        return;
    }
    evalGenericCommand(c,1);
}

/* We replace math.random() with our implementation that is not affected
 * by specific libc random() implementations and will output the same sequence
 * (for the same seed) in every arch. */

/* The following implementation is the one shipped with Lua itself but with
 * rand() replaced by redisLrand48(). */
int redis_math_random (lua_State *L) {
  /* the `%' avoids the (rare) case of r==1, and is needed also because on
     some systems (SunOS!) `rand()' may return a value larger than RAND_MAX */
  lua_Number r = (lua_Number)(redisLrand48()%REDIS_LRAND48_MAX) /
                                (lua_Number)REDIS_LRAND48_MAX;
  switch (lua_gettop(L)) {  /* check number of arguments */
    case 0: {  /* no arguments */
      lua_pushnumber(L, r);  /* Number between 0 and 1 */
      break;
    }
    case 1: {  /* only upper limit */
      int u = luaL_checkint(L, 1);
      luaL_argcheck(L, 1<=u, 1, "interval is empty");
      lua_pushnumber(L, floor(r*u)+1);  /* int between 1 and `u' */
      break;
    }
    case 2: {  /* lower and upper limits */
      int l = luaL_checkint(L, 1);
      int u = luaL_checkint(L, 2);
      luaL_argcheck(L, l<=u, 2, "interval is empty");
      lua_pushnumber(L, floor(r*(u-l+1))+l);  /* int between `l' and `u' */
      break;
    }
    default: return luaL_error(L, "wrong number of arguments");
  }
  return 1;
}

int redis_math_randomseed (lua_State *L) {
  redisSrand48(luaL_checkint(L, 1));
  return 0;
}

/* ---------------------------------------------------------------------------
 * SCRIPT command for script environment introspection and control
 * ------------------------------------------------------------------------- */

void scriptCommand(redisClient *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"flush")) {
        scriptingReset();
        addReply(c,shared.ok);
        replicationScriptCacheFlush();
        server.dirty++; /* Propagating this command is a good idea. */
    } else if (c->argc >= 2 && !strcasecmp(c->argv[1]->ptr,"exists")) {
        int j;

        addReplyMultiBulkLen(c, c->argc-2);
        for (j = 2; j < c->argc; j++) {
            if (dictFind(server.lua_scripts,c->argv[j]->ptr))
                addReply(c,shared.cone);
            else
                addReply(c,shared.czero);
        }
    } else if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr,"load")) {
        char funcname[43];
        sds sha;

        funcname[0] = 'f';
        funcname[1] = '_';
        sha1hex(funcname+2,c->argv[2]->ptr,sdslen(c->argv[2]->ptr));
        sha = sdsnewlen(funcname+2,40);
        if (dictFind(server.lua_scripts,sha) == NULL) {
            if (luaCreateFunction(c,server.lua,funcname,c->argv[2])
                    == REDIS_ERR) {
                sdsfree(sha);
                return;
            }
        }
        addReplyBulkCBuffer(c,funcname+2,40);
        sdsfree(sha);
        forceCommandPropagation(c,REDIS_PROPAGATE_REPL|REDIS_PROPAGATE_AOF);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"kill")) {
        if (server.lua_caller == NULL) {
            addReplySds(c,sdsnew("-NOTBUSY No scripts in execution right now.\r\n"));
        } else if (server.lua_write_dirty) {
            addReplySds(c,sdsnew("-UNKILLABLE Sorry the script already executed write commands against the dataset. You can either wait the script termination or kill the server in a hard way using the SHUTDOWN NOSAVE command.\r\n"));
        } else {
            server.lua_kill = 1;
            addReply(c,shared.ok);
        }
    } else {
        addReplyError(c, "Unknown SCRIPT subcommand or wrong # of args.");
    }
}
