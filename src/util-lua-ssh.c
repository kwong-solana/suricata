/* Copyright (C) 2014 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */


/**
 * \file
 *
 * \author Mats Klepsland <mats.klepsland@gmail.com>
 *
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "pkt-var.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-print.h"
#include "util-unittest.h"

#include "util-debug.h"

#include "output.h"
#include "app-layer.h"
#include "app-layer-parser.h"
#include "app-layer-ssh.h"
#include "util-privs.h"
#include "util-buffer.h"
#include "util-proto-name.h"
#include "util-logopenfile.h"
#include "util-time.h"

#ifdef HAVE_LUA

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "util-lua.h"
#include "util-lua-common.h"

static int GetServerProtoVersion(lua_State *luastate, const Flow *f)
{
    void *state = FlowGetAppState(f);
    if (state == NULL)
        return LuaCallbackError(luastate, "error: no app layer state");

    SshState *ssh_state = (SshState *)state;

    if (ssh_state->srv_hdr.proto_version == NULL)
        return LuaCallbackError(luastate, "error: no server proto version");

    return LuaPushStringBuffer(luastate, ssh_state->srv_hdr.proto_version,
                               strlen((char *)ssh_state->srv_hdr.proto_version));
}

static int SshGetServerProtoVersion(lua_State *luastate)
{
    int r;

    if (!(LuaStateNeedProto(luastate, ALPROTO_SSH)))
        return LuaCallbackError(luastate, "error: protocol not ssh");

    int lock_hint = 0;
    Flow *f = LuaStateGetFlow(luastate, &lock_hint);
    if (f == NULL)
        return LuaCallbackError(luastate, "internal error: no flow");

    if (lock_hint == LUA_FLOW_NOT_LOCKED_BY_PARENT) {
        FLOWLOCK_RDLOCK(f);
        r = GetServerProtoVersion(luastate, f);
        FLOWLOCK_UNLOCK(f);
    } else {
        r = GetServerProtoVersion(luastate, f);
    }
    return r;
}

static int GetServerSoftwareVersion(lua_State *luastate, const Flow *f)
{
    void *state = FlowGetAppState(f);
    if (state == NULL)
        return LuaCallbackError(luastate, "error: no app layer state");

    SshState *ssh_state = (SshState *)state;

    if (ssh_state->srv_hdr.software_version == NULL)
        return LuaCallbackError(luastate, "error: no server software version");

    return LuaPushStringBuffer(luastate, ssh_state->srv_hdr.software_version,
                               strlen((char *)ssh_state->srv_hdr.software_version));
}

static int SshGetServerSoftwareVersion(lua_State *luastate)
{
    int r;

    if (!(LuaStateNeedProto(luastate, ALPROTO_SSH)))
        return LuaCallbackError(luastate, "error: protocol not ssh");

    int lock_hint = 0;
    Flow *f = LuaStateGetFlow(luastate, &lock_hint);
    if (f == NULL)
        return LuaCallbackError(luastate, "internal error: no flow");

    if (lock_hint == LUA_FLOW_NOT_LOCKED_BY_PARENT) {
        FLOWLOCK_RDLOCK(f);
        r = GetServerSoftwareVersion(luastate, f);
        FLOWLOCK_UNLOCK(f);
    } else {
        r = GetServerSoftwareVersion(luastate, f);
    }
    return r;
}

static int GetClientProtoVersion(lua_State *luastate, const Flow *f)
{
    void *state = FlowGetAppState(f);
    if (state == NULL)
        return LuaCallbackError(luastate, "error: no app layer state");

    SshState *ssh_state = (SshState *)state;

    if (ssh_state->cli_hdr.proto_version == NULL)
        return LuaCallbackError(luastate, "error: no client proto version");

    return LuaPushStringBuffer(luastate, ssh_state->cli_hdr.proto_version,
                               strlen((char *)ssh_state->cli_hdr.proto_version));
}

static int SshGetClientProtoVersion(lua_State *luastate)
{
    int r;

    if (!(LuaStateNeedProto(luastate, ALPROTO_SSH)))
        return LuaCallbackError(luastate, "error: protocol not ssh");

    int lock_hint = 0;
    Flow *f = LuaStateGetFlow(luastate, &lock_hint);
    if (f == NULL)
        return LuaCallbackError(luastate, "internal error: no flow");

    if (lock_hint == LUA_FLOW_NOT_LOCKED_BY_PARENT) {
        FLOWLOCK_RDLOCK(f);
        r = GetClientProtoVersion(luastate, f);
        FLOWLOCK_UNLOCK(f);
    } else {
        r = GetClientProtoVersion(luastate, f);
    }
    return r;
}

static int GetClientSoftwareVersion(lua_State *luastate, const Flow *f)
{
    void *state = FlowGetAppState(f);
    if (state == NULL)
        return LuaCallbackError(luastate, "error: no app layer state");

    SshState *ssh_state = (SshState *)state;

    if (ssh_state->cli_hdr.software_version == NULL)
        return LuaCallbackError(luastate, "error: no client software version");

    return LuaPushStringBuffer(luastate, ssh_state->cli_hdr.software_version,
                               strlen((char *)ssh_state->cli_hdr.software_version));
}

static int SshGetClientSoftwareVersion(lua_State *luastate)
{
    int r;

    if (!(LuaStateNeedProto(luastate, ALPROTO_SSH)))
        return LuaCallbackError(luastate, "error: protocol not ssh");

    int lock_hint = 0;
    Flow *f = LuaStateGetFlow(luastate, &lock_hint);
    if (f == NULL)
        return LuaCallbackError(luastate, "internal error: no flow");

    if (lock_hint == LUA_FLOW_NOT_LOCKED_BY_PARENT) {
        FLOWLOCK_RDLOCK(f);
        r = GetClientSoftwareVersion(luastate, f);
        FLOWLOCK_UNLOCK(f);
    } else {
        r = GetClientSoftwareVersion(luastate, f);
    }
    return r;
}

/** \brief register ssh lua extensions in a luastate */
int LuaRegisterSshFunctions(lua_State *luastate)
{
    /* registration of the callbacks */
    lua_pushcfunction(luastate, SshGetServerProtoVersion);
    lua_setglobal(luastate, "SshGetServerProtoVersion");

    lua_pushcfunction(luastate, SshGetServerSoftwareVersion);
    lua_setglobal(luastate, "SshGetServerSoftwareVersion");

    lua_pushcfunction(luastate, SshGetClientProtoVersion);
    lua_setglobal(luastate, "SshGetClientProtoVersion");

    lua_pushcfunction(luastate, SshGetClientSoftwareVersion);
    lua_setglobal(luastate, "SshGetClientSoftwareVersion");

    return 0;
}

#endif /* HAVE_LUA */
