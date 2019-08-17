/*
** $Id: ldebug.c,v 2.121 2016/10/19 12:32:10 roberto Exp $
** Debug Interface
** See Copyright Notice in lua.h
*/

#define ldebug_c
#define LUA_CORE

#include "lprefix.h"


#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"



#define noLuaClosure(f)		((f) == NULL || (f)->c.tt == LUA_TCCL)


/* Active Lua function (given call info) */
#define ci_func(ci)		(clLvalue((ci)->func))


static const char *funcnamefromcode (lua_State *L, CallInfo *ci,
                                    const char **name);

static int currentpc (CallInfo *ci) {
  lua_assert(isLua(ci));
  return pcRel(ci->u.l.savedpc, ci_func(ci)->p);
}


static int currentline (CallInfo *ci) {
  return getfuncline(ci_func(ci)->p, currentpc(ci));
}


/*
** If function yielded, its 'func' can be in the 'extra' field. The
** next function restores 'func' to its correct value for debugging
** purposes. (It exchanges 'func' and 'extra'; so, when called again,
** after debugging, it also "re-restores" ** 'func' to its altered value.
*/
static void swapextra (lua_State *L) {
  if (L->status == LUA_YIELD) {
    CallInfo *ci = L->ci;  /* get function that yielded */
    StkId temp = ci->func;  /* exchange its 'func' and 'extra' values */
    ci->func = restorestack(L, ci->extra);
    ci->extra = savestack(L, temp);
  }
}


/*
** This function can be called asynchronously (e.g. during a signal).
** Fields 'oldpc', 'basehookcount', and 'hookcount' (set by
** 'resethookcount') are for debug only, and it is no problem if they
** get arbitrary values (causes at most one wrong hook call). 'hookmask'
** is an atomic value. We assume that pointers are atomic too (e.g., gcc
** ensures that for all platforms where it runs). Moreover, 'hook' is
** always checked before being called (see 'luaD_hook').
*/
LUA_API void lua_sethook (lua_State *L, lua_Hook func, int mask, int count) {
  if (func == NULL || mask == 0) {  /* turn off hooks? */
    mask = 0;
    func = NULL;
  }
  if (isLua(L->ci))
    L->oldpc = L->ci->u.l.savedpc;
  L->hook = func;
  L->basehookcount = count;
  resethookcount(L);
  L->hookmask = cast_byte(mask);
}


LUA_API lua_Hook lua_gethook (lua_State *L) {
  return L->hook;
}


LUA_API int lua_gethookmask (lua_State *L) {
  return L->hookmask;
}


LUA_API int lua_gethookcount (lua_State *L) {
  return L->basehookcount;
}


LUA_API int lua_getstack (lua_State *L, int level, lua_Debug *ar) {
  int status;
  CallInfo *ci;
  if (level < 0) return 0;  /* invalid (negative) level */
  lua_lock(L);
  for (ci = L->ci; level > 0 && ci != &L->base_ci; ci = ci->previous)
    level--;
  if (level == 0 && ci != &L->base_ci) {  /* level found? */
    status = 1;
    ar->i_ci = ci;
  }
  else status = 0;  /* no such level */
  lua_unlock(L);
  return status;
}


static const char *upvalname (Proto *p, int uv) {
  TString *s = check_exp(uv < p->sizeupvalues, p->upvalues[uv].name);
  if (s == NULL) return "?";
  else return getstr(s);
}


static const char *findvararg (CallInfo *ci, int n, StkId *pos) {
  int nparams = clLvalue(ci->func)->p->numparams;
  if (n >= cast_int(ci->u.l.base - ci->func) - nparams)
    return NULL;  /* no such vararg */
  else {
    *pos = ci->func + nparams + n;
    return "(*vararg)";  /* generic name for any vararg */
  }
}


static const char *findlocal (lua_State *L, CallInfo *ci, int n,
                              StkId *pos) {
  const char *name = NULL;
  StkId base;
  if (isLua(ci)) {
    if (n < 0)  /* access to vararg values? */
      return findvararg(ci, -n, pos);
    else {
      base = ci->u.l.base;
      name = luaF_getlocalname(ci_func(ci)->p, n, currentpc(ci));
    }
  }
  else
    base = ci->func + 1;
  if (name == NULL) {  /* no 'standard' name? */
    StkId limit = (ci == L->ci) ? L->top : ci->next->func;
    if (limit - base >= n && n > 0)  /* is 'n' inside 'ci' stack? */
      name = "(*temporary)";  /* generic name for any valid slot */
    else
      return NULL;  /* no name */
  }
  *pos = base + (n - 1);
  return name;
}


LUA_API const char *lua_getlocal (lua_State *L, const lua_Debug *ar, int n) {
  const char *name;
  lua_lock(L);
  swapextra(L);
  if (ar == NULL) {  /* information about non-active function? */
    if (!isLfunction(L->top - 1))  /* not a Lua function? */
      name = NULL;
    else  /* consider live variables at function start (parameters) */
      name = luaF_getlocalname(clLvalue(L->top - 1)->p, n, 0);
  }
  else {  /* active function; get information through 'ar' */
    StkId pos = NULL;  /* to avoid warnings */
    name = findlocal(L, ar->i_ci, n, &pos);
    if (name) {
      setobj2s(L, L->top, pos);
      api_incr_top(L);
    }
  }
  swapextra(L);
  lua_unlock(L);
  return name;
}


LUA_API const char *lua_setlocal (lua_State *L, const lua_Debug *ar, int n) {
  StkId pos = NULL;  /* to avoid warnings */
  const char *name;
  lua_lock(L);
  swapextra(L);
  name = findlocal(L, ar->i_ci, n, &pos);
  if (name) {
    setobjs2s(L, pos, L->top - 1);
    L->top--;  /* pop value */
  }
  swapextra(L);
  lua_unlock(L);
  return name;
}


static void funcinfo (lua_Debug *ar, Closure *cl) {
  if (noLuaClosure(cl)) {
    ar->source = "=[C]";
    ar->linedefined = -1;
    ar->lastlinedefined = -1;
    ar->what = "C";
  }
  else {
    Proto *p = cl->l.p;
    ar->source = p->source ? getstr(p->source) : "=?";
    ar->linedefined = p->linedefined;
    ar->lastlinedefined = p->lastlinedefined;
    ar->what = (ar->linedefined == 0) ? "main" : "Lua";
  }
  luaO_chunkid(ar->short_src, ar->source, LUA_IDSIZE);
}


static void collectvalidlines (lua_State *L, Closure *f) {
  if (noLuaClosure(f)) {
    setnilvalue(L->top);
    api_incr_top(L);
  }
  else {
    int i;
    TValue v;
    int *lineinfo = f->l.p->lineinfo;
    Table *t = luaH_new(L);  /* new table to store active lines */
    sethvalue(L, L->top, t);  /* push it on stack */
    api_incr_top(L);
    setbvalue(&v, 1);  /* boolean 'true' to be the value of all indices */
    for (i = 0; i < f->l.p->sizelineinfo; i++)  /* for all lines with code */
      luaH_setint(L, t, lineinfo[i], &v);  /* table[line] = true */
  }
}


static const char *getfuncname (lua_State *L, CallInfo *ci, const char **name) {
  if (ci == NULL)  /* no 'ci'? */
    return NULL;  /* no info */
  else if (ci->callstatus & CIST_FIN) {  /* is this a finalizer? */
    *name = "__gc";
    return "metamethod";  /* report it as such */
  }
  /* calling function is a known Lua function? */
  else if (!(ci->callstatus & CIST_TAIL) && isLua(ci->previous))
    return funcnamefromcode(L, ci->previous, name);
  else return NULL;  /* no way to find a name */
}


static int auxgetinfo (lua_State *L, const char *what, lua_Debug *ar,
                       Closure *f, CallInfo *ci) {
  int status = 1;
  for (; *what; what++) {
    switch (*what) {
      case 'S': {
        funcinfo(ar, f);
        break;
      }
      case 'l': {
        ar->currentline = (ci && isLua(ci)) ? currentline(ci) : -1;
        break;
      }
      case 'u': {
        ar->nups = (f == NULL) ? 0 : f->c.nupvalues;
        if (noLuaClosure(f)) {
          ar->isvararg = 1;
          ar->nparams = 0;
        }
        else {
          ar->isvararg = f->l.p->is_vararg;
          ar->nparams = f->l.p->numparams;
        }
        break;
      }
      case 't': {
        ar->istailcall = (ci) ? ci->callstatus & CIST_TAIL : 0;
        break;
      }
      case 'n': {
        ar->namewhat = getfuncname(L, ci, &ar->name);
        if (ar->namewhat == NULL) {
          ar->namewhat = "";  /* not found */
          ar->name = NULL;
        }
        break;
      }
      case 'L':
      case 'f':  /* handled by lua_getinfo */
        break;
      default: status = 0;  /* invalid option */
    }
  }
  return status;
}


LUA_API int lua_getinfo (lua_State *L, const char *what, lua_Debug *ar) {
  int status;
  Closure *cl;
  CallInfo *ci;
  StkId func;
  lua_lock(L);
  swapextra(L);
  if (*what == '>') {
    ci = NULL;
    func = L->top - 1;
    api_check(L, ttisfunction(func), "function expected");
    what++;  /* skip the '>' */
    L->top--;  /* pop function */
  }
  else {
    ci = ar->i_ci;
    func = ci->func;
    lua_assert(ttisfunction(ci->func));
  }
  cl = ttisclosure(func) ? clvalue(func) : NULL;
  status = auxgetinfo(L, what, ar, cl, ci);
  if (strchr(what, 'f')) {
    setobjs2s(L, L->top, func);
    api_incr_top(L);
  }
  swapextra(L);  /* correct before option 'L', which can raise a mem. error */
  if (strchr(what, 'L'))
    collectvalidlines(L, cl);
  lua_unlock(L);
  return status;
}


/*
** {======================================================
** Symbolic Execution
** =======================================================
*/

static const char *getobjname (Proto *p, int lastpc, int reg,
                               const char **name);


/*
** find a "name" for the RK value 'c'
*/
static void kname (Proto *p, int pc, int c, const char **name) {
  if (ISK(c)) {  /* is 'c' a constant? */
    TValue *kvalue = &p->k[INDEXK(c)];
    if (ttisstring(kvalue)) {  /* literal constant? */
      *name = svalue(kvalue);  /* it is its own name */
      return;
    }
    /* else no reasonable name found */
  }
  else {  /* 'c' is a register */
    const char *what = getobjname(p, pc, c, name); /* search for 'c' */
    if (what && *what == 'c') {  /* found a constant name? */
      return;  /* 'name' already filled */
    }
    /* else no reasonable name found */
  }
  *name = "?";  /* no reasonable name found */
}


static int filterpc (int pc, int jmptarget) {
  if (pc < jmptarget)  /* is code conditional (inside a jump)? */
    return -1;  /* cannot know who sets that register */
  else return pc;  /* current position sets that register */
}


/*
** try to find last instruction before 'lastpc' that modified register 'reg'
*/
static int findsetreg (Proto *p, int lastpc, int reg) {
  int pc;
  int setreg = -1;  /* keep last instruction that changed 'reg' */
  int jmptarget = 0;  /* any code before this address is conditional */
  for (pc = 0; pc < lastpc; pc++) {
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    int a = GETARG_A(i);
    switch (op) {
      case OP_LOADNIL: {
        int b = GETARG_B(i);
        if (a <= reg && reg <= a + b)  /* set registers from 'a' to 'a+b' */
          setreg = filterpc(pc, jmptarget);
        break;
      }
      case OP_TFORCALL: {
        if (reg >= a + 2)  /* affect all regs above its base */
          setreg = filterpc(pc, jmptarget);
        break;
      }
      case OP_CALL:
      case OP_TAILCALL: {
        if (reg >= a)  /* affect all registers above base */
          setreg = filterpc(pc, jmptarget);
        break;
      }
      case OP_JMP: {
        int b = GETARG_sBx(i);
        int dest = pc + 1 + b;
        /* jump is forward and do not skip 'lastpc'? */
        if (pc < dest && dest <= lastpc) {
          if (dest > jmptarget)
            jmptarget = dest;  /* update 'jmptarget' */
        }
        break;
      }
      default:
        if (testAMode(op) && reg == a)  /* any instruction that set A */
          setreg = filterpc(pc, jmptarget);
        break;
    }
  }
  return setreg;
}


static const char *getobjname (Proto *p, int lastpc, int reg,
                               const char **name) {
  int pc;
  *name = luaF_getlocalname(p, reg + 1, lastpc);
  if (*name)  /* is a local? */
    return "local";
  /* else try symbolic execution */
  pc = findsetreg(p, lastpc, reg);
  if (pc != -1) {  /* could find instruction? */
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    switch (op) {
      case OP_MOVE: {
        int b = GETARG_B(i);  /* move from 'b' to 'a' */
        if (b < GETARG_A(i))
          return getobjname(p, pc, b, name);  /* get name for 'b' */
        break;
      }
      case OP_GETTABUP:
      case OP_GETTABLE: {
        int k = GETARG_C(i);  /* key index */
        int t = GETARG_B(i);  /* table index */
        const char *vn = (op == OP_GETTABLE)  /* name of indexed variable */
                         ? luaF_getlocalname(p, t + 1, pc)
                         : upvalname(p, t);
        kname(p, pc, k, name);
        return (vn && strcmp(vn, LUA_ENV) == 0) ? "global" : "field";
      }
      case OP_GETUPVAL: {
        *name = upvalname(p, GETARG_B(i));
        return "upvalue";
      }
      case OP_LOADK:
      case OP_LOADKX: {
        int b = (op == OP_LOADK) ? GETARG_Bx(i)
                                 : GETARG_Ax(p->code[pc + 1]);
        if (ttisstring(&p->k[b])) {
          *name = svalue(&p->k[b]);
          return "constant";
        }
        break;
      }
      case OP_SELF: {
        int k = GETARG_C(i);  /* key index */
        kname(p, pc, k, name);
        return "method";
      }
      default: break;  /* go through to return NULL */
    }
  }
  return NULL;  /* could not find reasonable name */
}


/*
** Try to find a name for a function based on the code that called it.
** (Only works when function was called by a Lua function.)
** Returns what the name is (e.g., "for iterator", "method",
** "metamethod") and sets '*name' to point to the name.
*/
static const char *funcnamefromcode (lua_State *L, CallInfo *ci,
                                     const char **name) {
  TMS tm = (TMS)0;  /* (initial value avoids warnings) */
  Proto *p = ci_func(ci)->p;  /* calling function */
  int pc = currentpc(ci);  /* calling instruction index */
  Instruction i = p->code[pc];  /* calling instruction */
  if (ci->callstatus & CIST_HOOKED) {  /* was it called inside a hook? */
    *name = "?";
    return "hook";
  }
  switch (GET_OPCODE(i)) {
    case OP_CALL:
    case OP_TAILCALL:
      return getobjname(p, pc, GETARG_A(i), name);  /* get function name */
    case OP_TFORCALL: {  /* for iterator */
      *name = "for iterator";
       return "for iterator";
    }
    /* other instructions can do calls through metamethods */
    case OP_SELF: case OP_GETTABUP: case OP_GETTABLE:
      tm = TM_INDEX;
      break;
    case OP_SETTABUP: case OP_SETTABLE:
      tm = TM_NEWINDEX;
      break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD:
    case OP_POW: case OP_DIV: case OP_IDIV: case OP_BAND:
    case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
      int offset = cast_int(GET_OPCODE(i)) - cast_int(OP_ADD);  /* ORDER OP */
      tm = cast(TMS, offset + cast_int(TM_ADD));  /* ORDER TM */
      break;
    }
    case OP_UNM: tm = TM_UNM; break;
    case OP_BNOT: tm = TM_BNOT; break;
    case OP_LEN: tm = TM_LEN; break;
    case OP_CONCAT: tm = TM_CONCAT; break;
    case OP_EQ: tm = TM_EQ; break;
    case OP_LT: tm = TM_LT; break;
    case OP_LE: tm = TM_LE; break;
    default:
      return NULL;  /* cannot find a reasonable name */
  }
  *name = getstr(G(L)->tmname[tm]);
  return "metamethod";
}

/* }====================================================== */



/*
** The subtraction of two potentially unrelated pointers is
** not ISO C, but it should not crash a program; the subsequent
** checks are ISO C and ensure a correct result.
*/
static int isinstack (CallInfo *ci, const TValue *o) {
  ptrdiff_t i = o - ci->u.l.base;
  return (0 <= i && i < (ci->top - ci->u.l.base) && ci->u.l.base + i == o);
}


/*
** Checks whether value 'o' came from an upvalue. (That can only happen
** with instructions OP_GETTABUP/OP_SETTABUP, which operate directly on
** upvalues.)
*/
static const char *getupvalname (CallInfo *ci, const TValue *o,
                                 const char **name) {
  LClosure *c = ci_func(ci);
  int i;
  for (i = 0; i < c->nupvalues; i++) {
    if (c->upvals[i]->v == o) {
      *name = upvalname(c->p, i);
      return "upvalue";
    }
  }
  return NULL;
}


static const char *varinfo (lua_State *L, const TValue *o) {
  const char *name = NULL;  /* to avoid warnings */
  CallInfo *ci = L->ci;
  const char *kind = NULL;
  if (isLua(ci)) {
    kind = getupvalname(ci, o, &name);  /* check whether 'o' is an upvalue */
    if (!kind && isinstack(ci, o))  /* no? try a register */
      kind = getobjname(ci_func(ci)->p, currentpc(ci),
                        cast_int(o - ci->u.l.base), &name);
  }
  return (kind) ? luaO_pushfstring(L, " (%s '%s')", kind, name) : "";
}


l_noret luaG_typeerror (lua_State *L, const TValue *o, const char *op) {
  const char *t = luaT_objtypename(L, o);
  luaG_runerror(L, "attempt to %s a %s value%s", op, t, varinfo(L, o));
}


l_noret luaG_concaterror (lua_State *L, const TValue *p1, const TValue *p2) {
  if (ttisstring(p1) || cvt2str(p1)) p1 = p2;
  luaG_typeerror(L, p1, "concatenate");
}


l_noret luaG_opinterror (lua_State *L, const TValue *p1,
                         const TValue *p2, const char *msg) {
  lua_Number temp;
  if (!tonumber(p1, &temp))  /* first operand is wrong? */
    p2 = p1;  /* now second is wrong */
  luaG_typeerror(L, p2, msg);
}


/*
** Error when both values are convertible to numbers, but not to integers
*/
l_noret luaG_tointerror (lua_State *L, const TValue *p1, const TValue *p2) {
  lua_Integer temp;
  if (!tointeger(p1, &temp))
    p2 = p1;
  luaG_runerror(L, "number%s has no integer representation", varinfo(L, p2));
}


l_noret luaG_ordererror (lua_State *L, const TValue *p1, const TValue *p2) {
  const char *t1 = luaT_objtypename(L, p1);
  const char *t2 = luaT_objtypename(L, p2);
  if (strcmp(t1, t2) == 0)
    luaG_runerror(L, "attempt to compare two %s values", t1);
  else
    luaG_runerror(L, "attempt to compare %s with %s", t1, t2);
}


/* add src:line information to 'msg' */
const char *luaG_addinfo (lua_State *L, const char *msg, TString *src,
                                        int line) {
  char buff[LUA_IDSIZE];
  if (src)
    luaO_chunkid(buff, getstr(src), LUA_IDSIZE);
  else {  /* no source available; use "?" instead */
    buff[0] = '?'; buff[1] = '\0';
  }
  return luaO_pushfstring(L, "%s:%d: %s", buff, line, msg);
}


l_noret luaG_errormsg (lua_State *L) {
  if (L->errfunc != 0) {  /* is there an error handling function? */
    StkId errfunc = restorestack(L, L->errfunc);
    setobjs2s(L, L->top, L->top - 1);  /* move argument */
    setobjs2s(L, L->top - 1, errfunc);  /* push function */
    L->top++;  /* assume EXTRA_STACK */
    luaD_callnoyield(L, L->top - 2, 1);  /* call it */
  }
  luaD_throw(L, LUA_ERRRUN);
}


l_noret luaG_runerror (lua_State *L, const char *fmt, ...) {
  CallInfo *ci = L->ci;
  const char *msg;
  va_list argp;
  va_start(argp, fmt);
  msg = luaO_pushvfstring(L, fmt, argp);  /* format message */
  va_end(argp);
  if (isLua(ci))  /* if Lua function, add source:line information */
    luaG_addinfo(L, msg, ci_func(ci)->p->source, currentline(ci));
  luaG_errormsg(L);
}


void luaG_traceexec (lua_State *L) {
  CallInfo *ci = L->ci;
  lu_byte mask = L->hookmask;
  int counthook = (--L->hookcount == 0 && (mask & LUA_MASKCOUNT));
  if (counthook)
    resethookcount(L);  /* reset count */
  else if (!(mask & LUA_MASKLINE))
    return;  /* no line hook and count != 0; nothing to be done */
  if (ci->callstatus & CIST_HOOKYIELD) {  /* called hook last time? */
    ci->callstatus &= ~CIST_HOOKYIELD;  /* erase mark */
    return;  /* do not call hook again (VM yielded, so it did not move) */
  }
  if (counthook)
    luaD_hook(L, LUA_HOOKCOUNT, -1);  /* call count hook */
  if (mask & LUA_MASKLINE) {
    Proto *p = ci_func(ci)->p;
    int npc = pcRel(ci->u.l.savedpc, p);
    int newline = getfuncline(p, npc);
    if (npc == 0 ||  /* call linehook when enter a new function, */
        ci->u.l.savedpc <= L->oldpc ||  /* when jump back (loop), or when */
        newline != getfuncline(p, pcRel(L->oldpc, p)))  /* enter a new line */
      luaD_hook(L, LUA_HOOKLINE, newline);  /* call line hook */
  }
  L->oldpc = ci->u.l.savedpc;
  if (L->status == LUA_YIELD) {  /* did hook yield? */
    if (counthook)
      L->hookcount = 1;  /* undo decrement to zero */
    ci->u.l.savedpc--;  /* undo increment (resume will increment it again) */
    ci->callstatus |= CIST_HOOKYIELD;  /* mark that it yielded */
    ci->func = L->top - 1;  /* protect stack below results */
    luaD_throw(L, LUA_YIELD);
  }
}


/*
*********************************************************************
**
*********************************************************************
**/

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <pthread.h>


/* 7609 -> 0x1db9 -> ldbg */
#define LDBG_PORT 			7609

#define MAX_VARFIELD	  	16
#define MAX_IBUFSIZ 		(64 * 1024 - 1)
#define MAX_ARGV			32

#define SETPAUSE_CLI		1
#define SETPAUSE_NEXT		3
#define SETPAUSE_STEP 		4


#define MAX_BREAKPOINT 		99
#define ID_PSEUDOBP 		(MAX_BREAKPOINT+1)

#define SIZEDCSTR(str)		str,sizeof(str)-1

#define BP_TEMP 			0x01
#define BP_DISABLED 		0x02


typedef struct BreakPoint {
	short id;
	short flags;
	SrcFile *srcfile;
	int line;
	Proto *p;
	int codepos;
	Instruction code;
	struct BreakPoint *next;
}BreakPoint;

typedef struct FileContent {
	char *text;
	size_t fsize;
	int lines;
	int *linepos;
	SrcFile *srcfile;
	struct FileContent *next;
}FileContent;

typedef struct DebugConf {
	int listsize;
}DebugConf;
static const DebugConf DBGCONF = {
	.listsize = 10,
};

typedef struct DebugState {
	jmp_buf jmpbuf;
	const char *fatalerrmsg;
	DebugConf conf;
	void (*interact)(struct DebugState*);
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	char mode;
	
	BreakPoint *freebp;
	BreakPoint *bplist;
	BreakPoint pseudobp;
	BreakPoint *restorebp;
	size_t nr_bp;
	int bpid;  /* count from 1 */
	
	FileContent *fclist;

	/* for lua VM */
	int why_setpause;
	volatile int luacont;
	BreakPoint *rtbp;
	lua_State *L;
	const Instruction *oldpc;
	int oldline;
	CallInfo *citop;
	CallInfo *ci;
	SrcFile *rtsrcfile;
	int rtline;

	/* for debugger client */
	int fdin;
	int fdout;
	char *ibuf;
	size_t sizeibuf;
	size_t capibuf;
	char *obuf;
	size_t sizeobuf;
	size_t capobuf;
	int argc;
	const char *argv[MAX_ARGV];
	char argvbuf[1024];
	size_t argvbufsiz;

	/* for command print */
	TValue varfields[MAX_VARFIELD];
	int nr_varfields;

	/* for command list */
	SrcFile *lastlistsrcfile;
	int lastlistline;
	
	
}DebugState;

typedef struct CmdEntry {
	const char *name;
	const char *shortcut;
	void (*handler)(DebugState*);
}CmdEntry;

#define SETPAUSE(ds)			G(ds->L)->dbgstate = cast(DebugState*, (cast(uintptr_t, ds) | 0x1))
#define UNSETPAUSE(ds)			G(ds->L)->dbgstate = ds 
#define GETDS(L)				cast(DebugState*, (cast(uintptr_t, G(L)->dbgstate) & ~0x01))

#define DBGTHROW(ds, errmsg)	if (1) { ds->fatalerrmsg = errmsg; longjmp(ds->jmpbuf, 1); }

static void* DBGMALLOC(DebugState *ds, size_t siz)
{
	void *p = malloc(siz);
	if (!p) {
		DBGTHROW(ds, "out of memory");
	}
	return p;
}
static void* DBGREALLOC(DebugState *ds, void *oldp, size_t siz)
{
	void *p = realloc(oldp, siz);
	if (!p) {
		DBGTHROW(ds, "out of memory");
	}
	return p;
}

#define DBGFREE(ds, p)			free(p)

static void obpushstr(DebugState *ds, const char *str, size_t len)
{
	size_t needed;
	needed = len + ds->sizeobuf;
	if (needed > ds->capobuf) {
		if (!ds->capobuf) {
			ds->capobuf = 2048;
		}
		do {
			ds->capobuf *= 2;
		} while (ds->capobuf < needed);
		ds->obuf = DBGREALLOC(ds, ds->obuf, ds->capobuf);
	}
	memcpy(ds->obuf + ds->sizeobuf, str, len);
	ds->sizeobuf += len;
}

static void obpushvstr(DebugState *ds, const char *fmt, va_list ap)
{
	char buf[1024];
	int len = vsnprintf(buf, sizeof(buf), fmt, ap);
	obpushstr(ds, buf, (size_t)len);
}

static void obpushfstr(DebugState *ds, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	obpushvstr(ds, fmt, ap);
	va_end(ap);
}

static inline void obsetfstr(DebugState *ds, const char *fmt, ...)
{
	ds->sizeobuf = 0;
	va_list ap;
	va_start(ap, fmt);
	obpushvstr(ds, fmt, ap);
	va_end(ap);
}

static inline void obsetstr(DebugState *ds, const char *str, size_t len)
{
	ds->sizeobuf = 0;
	obpushstr(ds, str, len);
}

static void obflush(DebugState *ds)
{
	size_t sizeobuf = ds->sizeobuf;
	if (sizeobuf == 0 || ds->obuf[sizeobuf - 1] != '\n') {
		obpushstr(ds, "\n", 1);
	}
	obpushstr(ds, "> ", 2);
	if (ds->fdout >= 0) {
		write(ds->fdout, ds->obuf, ds->sizeobuf);
	}
	ds->sizeobuf = 0;
}
static void printvalue(DebugState *ds, TValue *v, int nested);
static void printtable(DebugState *ds, Table *t, int nested)
{
	int nrarray = (int)t->sizearray;
	int nrnode = (int)sizenode(t);
	int i;

	obpushfstr(ds, "(%p, sizearray=%d, sizenode=%d){", t, nrarray, nrnode); 
	if (nested || (nrarray == 0 && nrnode == 0)) {
		obpushstr(ds, SIZEDCSTR("}"));
		return;
	}

	obpushfstr(ds, SIZEDCSTR("\n"));
	if (nrarray > 0) {
		obpushstr(ds, "--array\n\t", 9);
		for (i = 0; i < nrarray; i++) {
			TValue *v = &t->array[i];
			if (ttisnil(v)) {
				break;
			}
			printvalue(ds, v, 1);
			obpushstr(ds, SIZEDCSTR(", "));
			if (i < nrarray - 1 && i % 5 == 4) {
				obpushstr(ds, SIZEDCSTR("\n\t"));
			}
		}
		obpushstr(ds, "\n", 1);
	}
	if (nrnode > 0) {
		obpushstr(ds, "--node\n", 7);
		for (i = 0; i < nrnode; i++) {
			TValue *v = gval(gnode(t, i));
			if (!ttisnil(v)) {
				obpushstr(ds, "\t[", 2);
				printvalue(ds, (TValue*)gkey(gnode(t, i)), 1);
				obpushstr(ds, SIZEDCSTR("] = "));
				printvalue(ds, v, 1);
				obpushstr(ds, ",\n", 2);
			}
		}
	}
	obpushstr(ds, "}\n", 2);
}

#define MAXNUMBER2STR 64
static void printvalue(DebugState *ds, TValue *v, int nested)
{
	switch (ttype(v)) {
	case LUA_TBOOLEAN: {
		if (bvalue(v)) {
			obpushstr(ds, SIZEDCSTR("true")); 
		} else {
			obpushstr(ds, SIZEDCSTR("false"));
		}
		break; }

	case LUA_TNIL: {
		obpushstr(ds, SIZEDCSTR("nil"));
		break; }
		
    case LUA_TNUMINT: {
		char buff[MAXNUMBER2STR];
		size_t len = lua_integer2str(buff, sizeof(buff), ivalue(v));
		obpushstr(ds, buff, len);
		break;}
		
    case LUA_TNUMFLT: {
		char buff[MAXNUMBER2STR];
		size_t len = lua_number2str(buff, sizeof(buff), fltvalue(v));
		obpushstr(ds, buff, len);
		break; }
	
	case LUA_TSHRSTR: {
		TString *ts = tsvalue(v);
		obpushstr(ds, "'", 1);
		obpushstr(ds, getstr(ts), ts->shrlen);
		obpushstr(ds, "'", 1);
		break;}
		
	case LUA_TLNGSTR: {
		TString *ts = tsvalue(v);
		obpushstr(ds, "'", 1);
		obpushstr(ds, getstr(ts), ts->u.lnglen);
		obpushstr(ds, "'", 1);
		break;}
		
	case LUA_TTABLE: {
		Table *t = hvalue(v);
		printtable(ds, t, nested);
		break;}

    default: {
		const char *tname = luaT_objtypename(ds->L, v);
		obpushstr(ds, tname, strlen(tname)); }
  }
}

static void newintfield(DebugState *ds, const char *tk, size_t len)
{
	TValue *v = &ds->varfields[ds->nr_varfields++];
	int intv = 0;
	size_t i;
	for (i = 0; i < len; i++) {
		intv = intv * 10 + (tk[i] - '0');
	}
	setivalue(v, intv);
}

static void newstrfield(DebugState *ds, const char *tk, size_t len)
{
	TValue *v = &ds->varfields[ds->nr_varfields++];
	TString *ts = luaS_newlstr(ds->L, tk, len);
	setsvalue(ds->L, v, ts);
}

static const char* parsevar(DebugState *ds, const char *str)
{
	enum {
		tkvarstart,
		tkvar,
		tkintstr,
		tkint,
		tkstr,
		tkinit,
	}st = tkvarstart;
	char c, quote;
	const char *tk;
	const char *s;

	ds->nr_varfields = 0;
	s = str;
	
	while ((c = *s)!= 0) {
		switch (st) {
		case tkvarstart: {
			if (!isalpha(c) && c != '_') {
				return "illegal variable name";
			}
			tk = s;
			st = tkvar;
			break;}
				
		case tkvar: {
			if (!isalnum(c)) {
				if (c == '.') {
					st = tkvarstart;
					newstrfield(ds, tk, s - tk);
					tk = NULL;
				} else if (c == '[') {
					st = tkintstr;
					newstrfield(ds, tk, s - tk);
					tk = NULL;
				} else if (c != '_') {
					return "illegal variable name";
				}
			}
			break;}

		case tkintstr: {
			if (c == '\'' || c == '\"') {
				tk = s + 1;
				quote = c;
				st = tkstr;
			} else {
				if (!isdigit(c)) {
					return "expecting integer or string in []";
				}
				st = tkint; /*fall through*/
				tk = s;
			}
			break; }

		case tkstr: {
			if (c == '\'' || c == '\"') {
				if (c != quote) {
					return "unmatched quote";
				}
				c = *++s;
				if (c != ']') {
					return "expecting ']'";
				}
				st = tkinit;
				newstrfield(ds, tk, s - 1 - tk);
				tk = NULL;
			}
			break; }

		case tkint: {
			if (!isdigit(c)) {
				if (c != ']') {
					return "expecting ']'";
				}
				st = tkinit;
				newintfield(ds, tk, s - tk);
				tk = NULL;
			}
			break; }

		case tkinit: {
			if (c == '.') {
				st = tkvarstart;
			} else if (c == '[') {
				st = tkintstr;
			} else {
				return "expecting suffix '.' or '['";
			}
			break; }

		default: break;
		}
		s++;		
	}

	if (tk) {
		newstrfield(ds, tk, s - tk);
	}
	return NULL;
}

static char findvar(DebugState *ds, TString *name, TValue *v)
{
	CallInfo *ci = ds->ci;
	LClosure *cl = clLvalue(ci->func);
	Proto *p = cl->p;
	int i;
	/* for searching in locals */
	LocVar *locs;
	int pc;
	/* for searching in upvals */
	Upvaldesc *ups;
	/* for searching in global table */
	Table *reg;
	const TValue *gt, *gv;

	locs = p->locvars;
	pc = currentpc(ci);
	for (i = p->sizelocvars - 1; i >= 0; i--) {
		if (locs[i].varname == name && locs[i].startpc <= pc && locs[i].endpc > pc) {
			setobj(ds->L, v, ci->u.l.base + i);
			return 'l';
		}
	}

	ups = p->upvalues;
	for (i = 0; i < p->sizeupvalues; i++) {
		if (ups[i].name == name) {
			setobj(ds->L, v, cl->upvals[i]->v);
			return 'u';
		}
	}

	reg = hvalue(&G(ds->L)->l_registry);
	gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
	gv = luaH_getstr(hvalue(gt), name);
	if (!gv) {
		setnilvalue(v);
		return '\0';
	}
	setobj(ds->L, v, gv);
	return 'g';
}


static void cmd_print(DebugState *ds)
{
	int i;
	TValue *v, vv;
	const char *err;
	v = &vv;
	for (i = 1; i < ds->argc; i++) {
		const char *varname = ds->argv[i];
		err = parsevar(ds, varname);
		if (err) {
			obpushfstr(ds, "[[%s]] syntax error: %s", varname, err);
			return;
		}
		findvar(ds, tsvalue(ds->varfields), v);
		if (ds->nr_varfields > 1) {
			int k;
			Table *t;
			TValue *f;
			for (k = 1; k < ds->nr_varfields; k++) {
				if (!ttistable(v)) {
					obpushstr(ds, "unable to index non-table", 0);
					break;
				}
				t = hvalue(v);
				f = &ds->varfields[k];
				if (ttype(f) == LUA_TNUMINT) {
					v = (TValue*)luaH_getint(t, ivalue(f));
				} else {
					v = (TValue*)luaH_getstr(t, tsvalue(f));
				}
			}
		}
		obpushfstr(ds, "%s = ", varname);
		printvalue(ds, v, 0);
		obpushstr(ds, "\n", 1);
	}
}


static void info_breaks(DebugState *ds)
{
	BreakPoint *bp = ds->bplist;
	while (bp != NULL) {
		obpushfstr(ds, "#%02d %s:%d\n", bp->id, getstr(bp->srcfile->filepath), bp->line);
		bp = bp->next;
	}
}

static void info_locals(DebugState *ds)
{
	CallInfo *ci = ds->ci;
	LClosure *cl = ci_func(ci);
	Proto *p = cl->p;
	int i;
	LocVar *locs;
	int pc;
	
	locs = p->locvars;
	pc = currentpc(ci);
	for (i = p->sizelocvars - 1; i >= 0; i--) {
		/* FIXME: we have the problem of conlicted locals with the same name  */
		if (locs[i].startpc <= pc && locs[i].endpc > pc) {
			obpushfstr(ds, "%s = ", getstr(locs[i].varname));
			printvalue(ds, ci->u.l.base + i, 0);
			obpushstr(ds, SIZEDCSTR("\n"));
		}
	}

	if (p->is_vararg) {
		obpushstr(ds, SIZEDCSTR("use `info args` to list the variable args"));
	}
}

static void info_upvals(DebugState *ds)
{
	CallInfo *ci = ds->ci;
	LClosure *cl = ci_func(ci);
	Proto *p = cl->p;
	int i;
	Upvaldesc *ups;
	ups = p->upvalues;
	for (i = 0; i < p->sizeupvalues; i++) {
		obpushfstr(ds, "%s = ", getstr(ups[i].name));
		printvalue(ds, cl->upvals[i]->v, i == 0 ? 1 : 0); /* don't expand _ENV */
		obpushstr(ds, SIZEDCSTR("\n"));
	}
}

static void info_args(DebugState *ds)
{
	CallInfo *ci = ds->ci;
	LClosure *cl = ci_func(ci);
	Proto *p = cl->p;
	int i;

	if (p->is_vararg) {
		int actual = ci->u.l.base - ci->func - 1;
		int fixed = p->numparams;
		TValue *v = ci->u.l.base;
		for (i = 1; i <= actual && i <= fixed; i++) {
			obpushfstr(ds, "$%d = ", i);
			printvalue(ds, v++, 0);
			obpushstr(ds, SIZEDCSTR("\n"));
		}
		if (i <= fixed) {
			for (; i <= fixed; i++) {
				obpushfstr(ds, "$%d = nil\n", i);
			}
		} else if (actual > fixed) {
			v = ci->u.l.base - (actual - fixed);
			for (; i <= actual; i++) {
				obpushfstr(ds, "$%d = ", i);
				printvalue(ds, v++, 0);
				obpushstr(ds, SIZEDCSTR("\n"));
			}
		}
	} else {
		for (i = 0; i < p->numparams; i++) {
			obpushfstr(ds, "$%d = ", i + 1);
			printvalue(ds, ci->u.l.base + i, 0);
			obpushstr(ds, SIZEDCSTR("\n"));
		}
	}
}

static void cmd_info(DebugState *ds)
{
	const struct InfoEntry {
		const char *type;
		void (*handler)(DebugState*);
	}infotable[] = {
		{"breaks", info_breaks},
		{"upvals", info_upvals},
		{"locals", info_locals},
		{"args", info_args},
		{NULL, NULL},
	};
	const char *what = ds->argv[1];
	const struct InfoEntry *e = infotable;
	while (e->type) {
		if (strcmp(e->type, what) == 0) {
			e->handler(ds);
			return;
		}
		e++;
	}
	obpushstr(ds, SIZEDCSTR("usage: info breaks|args|locals|upvals"));
}

static FileContent* newfilecontent(DebugState *ds, SrcFile *srcfile)
{
	char *mem, *text;
	struct stat fst;
	int fd;
	FileContent *fc;
	int binary = 0;

	if (stat(getstr(srcfile->filepath), &fst) < 0 || 
		(fd = open(getstr(srcfile->filepath), O_RDONLY)) < 0) {
		return NULL;
	}

	mem = DBGMALLOC(ds, fst.st_size);
	text = mem;
	read(fd, mem, fst.st_size);
	close(fd);
	
	/* skip BOM */
	if (fst.st_size >= 3) {
		if (strncmp(mem, "\xEF\xBB\xBF", 3) == 0) {
			text += 3;
		}
	}

	/* check binary */
	if (text[0] == '#') {
		char *s = text;
		char *e = mem + fst.st_size;
		while (s < e) {
			if (*s++ == '\n') {
				break;
			}
		}
		if (*s == LUA_SIGNATURE[0]) {
			binary = 1;
		}
	} else if (text[0] == LUA_SIGNATURE[0]) {
		binary = 1;
	}
	
	if (binary) {
		DBGFREE(ds, mem);
		text = NULL;
	}

	fc = DBGMALLOC(ds, sizeof(FileContent));
	fc->srcfile = srcfile;
	fc->text = text;
	if (text) {
		const char *s, *e;
		int *linepos;
		int lines;
		
		fc->fsize = (size_t)fst.st_size - (size_t)(text - mem);

		lines = 1;
		s = text;
		e = text + fc->fsize;
		while (s < e) {
			if (*s++ == '\n') {
				lines++;
			}
		}
		fc->lines = lines;
		linepos = DBGMALLOC(ds, lines * sizeof(int));
		fc->linepos = linepos;
		
		linepos[0] = 0;
		lines = 1;
		s = text;
		e = text + fc->fsize;
		while (s < e) {
			if (*s++ == '\n') {
				linepos[lines] = s - text;
				lines++;
			}
		}
	}
	fc->next = ds->fclist;
	ds->fclist = fc;
	srcfile->ud = fc;
	return fc;
}


static void updatecitop(DebugState *ds)
{
	CallInfo *ci = ds->L->ci;
	while (!ttisLclosure(ci->func)) {
		ci = ci->previous;
	}
	ds->ci = ci;
	ds->citop = ci;
}

static void updatecifilepos(DebugState *ds)
{
	CallInfo *ci = ds->ci;
	LClosure *cl;
	SrcFile *srcfile;
	cl = clLvalue(ci->func);
	ds->rtline = currentline(ci);
	srcfile = cl->p->srcfile;
	if (!srcfile->ud) {
		newfilecontent(ds, srcfile);
	}
	ds->rtsrcfile = srcfile;
}

static void listsrc(DebugState *ds, SrcFile *srcfile, int sline,  int nline)
{
	FileContent *fc = srcfile->ud;
	const char *linenumfmt = sline > 9999 ? "% 8d  " : "% 4d  ";
	int eline;
	int i;
	const char *linestr;
	size_t len;

	if (!fc) {
		fc = newfilecontent(ds, srcfile);
		if (!fc) {
			obpushfstr(ds, "<failed to access \"%s\">", getstr(srcfile->filepath));
			return;
		}
	}
	if (!fc->text) {
		obpushstr(ds, SIZEDCSTR("<binary format>"));
		return;
	}

	if (sline < 1) {
		sline = 1;
	} else if (sline > fc->lines) {
		return;
	}

	eline = sline + nline - 1;
	if (eline > fc->lines) {
		eline = fc->lines;
	}

	for (i = sline; i <= eline; i++) {
		obpushstr(ds, (ds->rtline == i && ds->rtsrcfile == srcfile) ? "->" : "  ", 2);
		obpushfstr(ds, linenumfmt, i);
		linestr = fc->text + fc->linepos[i - 1];
		if (i == fc->lines) {
			len = fc->fsize - (size_t)(linestr - fc->text);
		} else {
			len = (fc->text + fc->linepos[i]) - linestr;
		}
		obpushstr(ds, linestr, len);
	}
	
	ds->lastlistsrcfile = srcfile;
	ds->lastlistline = eline + 1;
}

static void listrtsrc(DebugState *ds)
{
	listsrc(ds, ds->rtsrcfile, ds->rtline - 2, ds->conf.listsize);
}

static void listlast(DebugState *ds)
{
	SrcFile *srcfile = ds->lastlistsrcfile;
	FileContent *fc;
	assert(srcfile);
	fc = (FileContent*)srcfile->ud;
	if (ds->lastlistline > fc->lines) {
		obpushstr(ds, "<EOF>", 5);
	} else {
		listsrc(ds, srcfile, ds->lastlistline, ds->conf.listsize);
	}
}

static void cmd_list(DebugState *ds)
{
	SrcFile *srcfile;
	int line;
	
	if (ds->argc == 1) {
		listlast(ds);
		return;
	}

	srcfile = luaE_getsrcfile(ds->L, ds->argv[1]);
	if (!srcfile) {
		obpushstr(ds, SIZEDCSTR("file not found"));
		return;
	}

	line = 1;
	if (ds->argc >= 3) {
		line = atoi(ds->argv[2]);
		if (line == 0) {
			line = 1;
		}
	}
	listsrc(ds, srcfile, line, ds->conf.listsize);
}

static void cmd_frame(DebugState *ds)
{
	int level = 0;
	CallInfo *ci;
	if (ds->argc > 1) {
		level = atoi(ds->argv[1]);
	}
	for (ci = ds->L->ci; level > 0 && ci != &ds->L->base_ci; ci = ci->previous) {
		level--;
	}
	if (level > 0) {
		obpushstr(ds, SIZEDCSTR("frame not found"));
		return;
	}
	if (!isLua(ci)) {
		obpushstr(ds, SIZEDCSTR("unable to enter C-frame"));
		return;
	}
	ds->ci = ci;
	updatecifilepos(ds);
	obpushfstr(ds, "in \"%s\":\n", getstr(ds->rtsrcfile->filepath));
	listrtsrc(ds);
}

static void pushfuncname(DebugState *ds, lua_Debug *ar) 
{
	if (*ar->namewhat != '\0') { /* is there a name from code? */
    	obpushfstr(ds, "%s '%s'", ar->namewhat, ar->name);  /* use it */
	} else if (*ar->what == 'm') { /* main? */
		obpushstr(ds, SIZEDCSTR("main chunk"));
	} else if (*ar->what != 'C') {/* for Lua functions, use <file:line> */
		obpushfstr(ds, "function <%s:%d>", ar->short_src, ar->linedefined);
	} else {/* nothing left... */
    	obpushstr(ds, "?", 1);
	}
}

static void cmd_backtrace(DebugState *ds)
{
	lua_Debug ar;
	lua_State *L = ds->L;
	int level = 0;
	while (lua_getstack(L, level++, &ar)) {
		lua_getinfo(L, "Slnt", &ar);
		obpushfstr(ds, ar.i_ci == ds->ci ? "->  %s:" : "    %s:", ar.short_src);
		if (ar.currentline > 0) {
			obpushfstr(ds, "%d:", ar.currentline);
		}
		obpushstr(ds, SIZEDCSTR(" in "));
		pushfuncname(ds, &ar);
		if (ar.istailcall) {
			obpushstr(ds, SIZEDCSTR("\n    (...tail calls...)"));
		}
		obpushstr(ds, SIZEDCSTR("\n"));
	}
}

static Proto* findproto(Proto *p, int line)
{	
	int i;
	Proto *retp;
	if (p->linedefined && (line < p->linedefined || line > p->lastlinedefined)) {
		return NULL;
	}
	for (i = 0; i < p->sizep; i++) {
		retp = findproto(p->p[i], line);
		if (retp) {
			return retp;
		}
	}
	return p;
}


static BreakPoint* newbreakpoint(DebugState *ds)
{
	BreakPoint *bp = ds->freebp;
	if (!bp) {
		bp = DBGMALLOC(ds, sizeof(*bp));
		memset(bp, 0, sizeof(*bp));
		bp->id = ds->bpid++;
	} else {
		int id = bp->id;
		ds->freebp = bp->next;
		memset(bp, 0, sizeof(*bp));
		bp->id = id;
	}
	bp->flags = 0;
	bp->next = ds->bplist;
	ds->bplist = bp;
	ds->nr_bp++;
	return bp;
}

static BreakPoint* getbreakpoint(DebugState *ds, int id)
{
	if (id == ID_PSEUDOBP) {
		return &ds->pseudobp;
	} else {
		BreakPoint *bp = ds->bplist;
		while (bp && bp->id != id) {
			bp = bp->next;
		}
		return bp;
	}
}

static void freebreakpoint(DebugState *ds, BreakPoint *bp)
{
	BreakPoint **pbp = &ds->bplist;
	assert(bp != &ds->pseudobp);
	while (*pbp != bp) {
		pbp = &bp->next;
	}
	*pbp = bp->next;
	bp->next = ds->freebp;
	ds->freebp = bp;
	ds->nr_bp--;
}

static BreakPoint* setbreakpoint(DebugState *ds)
{
	int line = 0;
	SrcFile *srcfile;
	Proto *p;
	BreakPoint *bp;
	int i, codepos;

	if (ds->nr_bp == MAX_BREAKPOINT) {
		obpushstr(ds, SIZEDCSTR("too many breakpoints"));
		return NULL;
	}
	
	if (ds->argc == 2) {
		line = atoi(ds->argv[1]);
		srcfile = ds->rtsrcfile;
	} else if (ds->argc >= 3) {
		line = atoi(ds->argv[2]);
		srcfile = luaE_getsrcfile(ds->L, ds->argv[1]);
		if (!srcfile) {
			obpushfstr(ds, "file \"%s\" not found", ds->argv[1]);
			return NULL;
		}
	}
	
	if (line <= 0 || !srcfile) {
		obpushstr(ds, SIZEDCSTR("usage: break <file> <line>"));
		return NULL;
	}

	bp = ds->bplist;
	while (bp) {
		if (bp->srcfile == srcfile && bp->line == line) {
			obpushfstr(ds, "breakpoint #%d already exists", bp->id);
			return NULL;
		}
		bp = bp->next;
	}

	p = findproto(srcfile->p, line);
	codepos = -1;
	if (p) {
		for (i = 0; i < p->sizelineinfo; i++) {
			if (p->lineinfo[i] == line) {
				codepos = i;
				break;
			}
		}
	}

	if (codepos == -1) {
		obpushstr(ds, SIZEDCSTR("invalid file line to set a breakpoint"));
		return NULL;
	}

	bp = newbreakpoint(ds);
	if (bp == NULL) {
		obpushstr(ds, SIZEDCSTR("too many breakponits"));
		return NULL;
	}
	bp->srcfile = srcfile;
	bp->line = line;
	bp->p = p;
	bp->codepos = codepos;
	bp->code = p->code[codepos];
	p->code[codepos] = CREATE_Ax(OP_INTERRUPT, bp->id);
	obpushfstr(ds, "breakpoint #%d set at %s:%d", bp->id, getstr(srcfile->filepath), line);
	return bp;
}

static void cmd_break(DebugState *ds)
{
	setbreakpoint(ds);
}

static void cmd_tb(DebugState *ds)
{
	BreakPoint *bp = setbreakpoint(ds);
	if (bp) {
		bp->flags = BP_TEMP;
	}
}

static void enable_breaks(DebugState *ds)
{
	int num = 0;
	if (ds->argc > 2) {
		int i;
		BreakPoint *bp;
		int id;
		for (i = 2; i < ds->argc; i++) {
			id = atoi(ds->argv[i]);
			if (id > 0 && id <= MAX_BREAKPOINT) {
				bp = getbreakpoint(ds, id);
			} else {
				bp = NULL;
			}
			if (bp) {
				if (bp->flags & BP_DISABLED) {
					bp->p->code[bp->codepos] = CREATE_Ax(OP_INTERRUPT, bp->id);
					num++;
				}
			} else {
				obpushfstr(ds, "breakpoint #%d not found", id);
			}
		}
	} else {
		BreakPoint*bp = ds->bplist;
		while (bp != NULL) {
			if ((bp->flags & BP_DISABLED)) {
				bp->p->code[bp->codepos] = CREATE_Ax(OP_INTERRUPT, bp->id);
				num++;
			}
			bp = bp->next;
		}
	}
	obpushfstr(ds, "enabled %d breakpoint(s)", num);
}

static void cmd_enable(DebugState *ds)
{
	if (strncmp(ds->argv[1], SIZEDCSTR("breaks")) == 0) {
		enable_breaks(ds);
	}
}

static void disable_breaks(DebugState *ds)
{
	int num = 0;
	if (ds->argc > 2) {
		int i;
		BreakPoint *bp;
		int id;
		for (i = 2; i < ds->argc; i++) {
			id = atoi(ds->argv[i]);
			if (id > 0 && id <= MAX_BREAKPOINT) {
				bp = getbreakpoint(ds, id);
			} else {
				bp = NULL;
			}
			if (bp) {
				if (!(bp->flags & BP_DISABLED)) {
					bp->p->code[bp->codepos] = bp->code;
					bp->flags |= BP_DISABLED;
					num++;
				}
			} else {
				obpushfstr(ds, "breakpoint #%d not found", id);
			}
		}
	} else {
		BreakPoint*bp = ds->bplist;
		while (bp != NULL) {
			if (!(bp->flags & BP_DISABLED)) {
				bp->p->code[bp->codepos] = bp->code;
				bp->flags |= BP_DISABLED;
				num++;
			}
			bp = bp->next;
		}
	}
	obpushfstr(ds, "disabled %d breakpoint(s)", num);
}


static void cmd_disable(DebugState *ds)
{
	if (strncmp(ds->argv[1], SIZEDCSTR("breaks")) == 0) {
		disable_breaks(ds);
	}
	
}

static void* preparecontlua(DebugState *ds)
{
	BreakPoint *bp;
	CallInfo *ci = ds->ci;
	if (ci != ds->citop) {
		ci = ds->citop;
		ds->ci = ci;
		updatecifilepos(ds);
	}

	bp = ds->rtbp;
	if (bp) {
		bp->p->code[bp->codepos] = bp->code; /*restore user opcode */
		if (bp->id != ID_PSEUDOBP) {
			ds->restorebp = bp;
		}
		ci->u.l.savedpc--;
	}
	return ci;
}

static void setpseudobp(DebugState *ds, Proto *p, int codepos)
{
	BreakPoint *bp;
	bp = &ds->pseudobp;
	bp->codepos = codepos;
	bp->p = p;
	bp->code = p->code[bp->codepos];
	p->code[bp->codepos] = CREATE_Ax(OP_INTERRUPT, bp->id);
}

static void cmd_step(DebugState *ds)
{
	ds->why_setpause = SETPAUSE_STEP;
	SETPAUSE(ds);
	preparecontlua(ds);
	ds->luacont = 1;
}

static void cmd_next(DebugState *ds)
{
	CallInfo *ci;
	Proto *p;
	Instruction code;
	int op;
	int codepos;
	int thisline;

	ci = preparecontlua(ds);

	p = ci_func(ci)->p;
	codepos = ci->u.l.savedpc - p->code;
	thisline = p->lineinfo[codepos];
	while (++codepos < p->sizecode) {
		code = p->code[codepos];
		op = GET_OPCODE(code);
		if (p->lineinfo[codepos] != thisline) {
			break;
		}
		if (op >= OP_JMP && op <= OP_TFORLOOP && op != OP_CALL) {
			codepos = -1;
			break;
		}
	}

	if (codepos < 0 || codepos == p->sizecode) {
		ds->why_setpause = SETPAUSE_STEP;
	} else {
		setpseudobp(ds, p, codepos);
		ds->why_setpause = SETPAUSE_NEXT;
	}
	
	SETPAUSE(ds);
	ds->luacont = 1;
}

static void cmd_finish(DebugState *ds)
{
	CallInfo *ci = preparecontlua(ds);
	ci = ci->previous;
	while (ci != &ds->L->base_ci) {
		if (isLua(ci)) {
			Proto *p = ci_func(ci)->p;
			int codepos = ci->u.l.savedpc - p->code;
			setpseudobp(ds, p, codepos);
			SETPAUSE(ds);
			ds->why_setpause = SETPAUSE_NEXT;
			ds->luacont = 1;
			return;
		}
		ci = ci->previous;
	}
}

static void cmd_until(DebugState *ds)
{
	CallInfo *ci;
	Proto *p;
	const Instruction *endcode, *code;
	Instruction i;
	int op;
	int nr_forprep, nr_tforcall;

	ci = ds->ci;
	if (ci != ds->citop) {
		ci = ds->citop;
		ds->ci = ci;
		updatecifilepos(ds);
	}
	p = ci_func(ci)->p;
	endcode = p->code + p->sizecode;
	code = ci->u.l.savedpc;
	nr_forprep = 0;
	nr_tforcall = 0;
	while (code < endcode) {
		i = *code;
		op = GET_OPCODE(i);
		if (op == OP_JMP) {
			if (code + 1 + GETARG_sBx(i) <= ci->u.l.savedpc) {
				break;
			}
		} else if (op == OP_FORPREP) {
			nr_forprep++;
		} else if (op == OP_FORLOOP) {
			nr_forprep--;
			if (nr_forprep < 0) {
				break;
			}
		} else if (op == OP_TFORCALL) {
			nr_tforcall++;
		} else if (op == OP_TFORLOOP) {
			nr_tforcall--;
			if (nr_tforcall < 0) {
				break;
			}
		}
		code++;
	}
	
	if (code == endcode) {
		obpushstr(ds, SIZEDCSTR("`until` should be used in a loop"));
		return;
	}

	preparecontlua(ds);
	setpseudobp(ds, p, (code + 1) - p->code);
	ds->why_setpause = SETPAUSE_NEXT;
	SETPAUSE(ds);
	ds->luacont = 1;
}

static void cmd_continue(DebugState *ds)
{
	BreakPoint *bp;
	CallInfo *ci = ds->ci;
	if (ci != ds->citop) {
		ci = ds->citop;
		ds->ci = ci;
		updatecifilepos(ds);
	}
	bp = ds->rtbp;
	if (bp) {
		bp->p->code[bp->codepos] = bp->code; /*restore user opcode */
		if (bp->id != ID_PSEUDOBP) {
			ds->restorebp = bp;
		}
		ci->u.l.savedpc--;
		SETPAUSE(ds);
		ds->why_setpause = SETPAUSE_NEXT;
	}
	ds->luacont = 1;
}

static void delete_breaks(DebugState *ds)
{
	int num = 0;
	if (ds->argc > 2) {
		int i;
		BreakPoint *bp;
		int id;
		for (i = 2; i < ds->argc; i++) {
			id = atoi(ds->argv[i]);
			if (id > 0 && id <= MAX_BREAKPOINT) {
				bp = getbreakpoint(ds, id);
			} else {
				bp = NULL;
			}
			if (bp) {
				bp->p->code[bp->codepos] = bp->code;
				if (ds->rtbp == bp) {
					ds->rtbp = NULL;
				}
				if (ds->restorebp == bp) {
					ds->restorebp = NULL;
				}
				num++;
			} else {
				obpushfstr(ds, "breakpoint #%s not found.\n", ds->argv[i]);
			}
		}
	} else {
		BreakPoint *bp = ds->bplist;
		BreakPoint *tail = NULL;
		while (bp) {
			bp->p->code[bp->codepos] = bp->code;
			tail = bp;
			bp = bp->next;
		}
		num = ds->nr_bp;
		if (tail) {
			tail->next = ds->freebp;
			ds->freebp = ds->bplist;
			ds->bplist = NULL;
			ds->nr_bp = 0;
		}
		ds->rtbp = NULL;
		ds->restorebp = NULL;
	}
	obpushfstr(ds, "deleted %d breakpoint(s)", num);
}

static void cmd_delete(DebugState *ds)
{
	if (strncmp(ds->argv[1], SIZEDCSTR("breaks")) == 0) {
		delete_breaks(ds);
	}
}

static void cmd_pause(DebugState *ds)
{
	if (ds->mode == 'b' && ds->luacont == -1) {
		ds->why_setpause = SETPAUSE_CLI;
		SETPAUSE(ds);
	}
}

static void cmd_quit(DebugState *ds)
{
	if (ds->mode != 'b') {
		exit(0);
	}
	if (ds->fdin >= 0) {
		close(ds->fdin);
		close(ds->fdout);
		ds->fdin = -1;
		ds->fdout = -1;
	}
}

const CmdEntry cmdtable[] = {
	{"print", "p", cmd_print},
	{"break", "b", cmd_break},
	{"tb", "tb", cmd_tb},
	{"enable", "ea", cmd_enable},
	{"disable", "da", cmd_disable},
	{"next", "n", cmd_next},
	{"step", "s", cmd_step},
	{"finish", "fi", cmd_finish},
	{"until", "un", cmd_until},
	{"backtrace", "bt", cmd_backtrace},
	{"frame", "f", cmd_frame},
	{"delete", "d", cmd_delete},
	{"list", "l", cmd_list},
	{"continue", "c", cmd_continue},
	{"info", "i", cmd_info},
	{"pause", "pa", cmd_pause},
	{"quit", "q", cmd_quit},
	{NULL, NULL, NULL}
};

static char* parsequotedarg(char *s, char quote)
{
	char c;
	while ((c = *s++) != 0) {
		if (c == quote) {
			*s = 0;
			return s + 1;
		}
	}
	return NULL;
}

static size_t parsecmd(DebugState *ds)
{
	char *s, *e, *ebuf;
	char *arg;
	size_t nparsed;

	ebuf = ds->ibuf + ds->sizeibuf;
	e = ds->ibuf;
	while (e < ebuf && *e != '\n') {
		e++;
	}
	if (e == ebuf) {
		if (ds->sizeibuf < ds->capibuf) {
			return 0;
		} else {
			ds->argc = 0;
			return ds->sizeibuf;
		}
	}
	*e = 0;
	nparsed = e - ds->ibuf + 1;
	if (nparsed == 1) {
		return 1;
	}
	
	memcpy(ds->argvbuf, ds->ibuf, nparsed);
	e = ds->argvbuf + nparsed - 1; 

	ds->argc = 0;
	arg = NULL;
	s = ds->argvbuf;
	while (s < e) {
		char c = *s;
		if (c == '\'' || c == '\"') {
			if (arg) {
				ds->argc = 0;
				return ds->sizeibuf;
			}
			arg = s + 1;
			s = parsequotedarg(s + 1, c);
			if (s) {
				ds->argv[ds->argc++] = arg;
				arg = NULL;
			} else {
				ds->argc = 0;
				return ds->sizeibuf;
			}
			
		} else {
			if (isspace(c)) {
				if (arg) {
					*s = 0;
					ds->argv[ds->argc++] = arg;
					arg = NULL;
				}
			} else {
				if (!arg) {
					arg = s;
				}
			}
			s++;
		}
	}

	if (arg) {
		ds->argv[ds->argc++] = arg;
	}
	
	return nparsed;
}

static void dispatchcmd(DebugState *ds)
{
	const char *cmdname;
	const CmdEntry *e;

	if (ds->argc == 0) {
		obpushstr(ds, SIZEDCSTR("invalid command line"));
		return;
	}
	
	cmdname = ds->argv[0];
	e = cmdtable;
	while (e->name) {
		if (strcmp(e->name, cmdname) == 0 ||
			strcmp(e->shortcut, cmdname) == 0) {
			if (e->handler != cmd_pause && e->handler != cmd_quit && 
				ds->mode == 'b' && ds->luacont == -1) {
				obpushstr(ds, SIZEDCSTR("Lua VM is running, use command `pause` to pause it."));			
			} else {
				e->handler(ds);
			}	
			return;
		}
		e++;
	}	
	obpushfstr(ds, "unknown command \"%s\"", cmdname);
}

/* return:
 0: succeed
>0: errno
-1: client closed
-2: server closed
*/
static int processinput(DebugState *ds)
{
	char *ibuf = ds->ibuf;
	struct pollfd fds;
	int pollret;
	ssize_t nread;
	size_t nparsed;
	int fd = ds->fdin;
	
	if (!ibuf) {
		ds->capibuf = MAX_IBUFSIZ;
		ibuf = malloc(ds->capibuf + 1);
		ds->ibuf = ibuf;
	}

	fds.fd = ds->fdin;
	fds.events = POLLIN;
again:
	pollret = poll(&fds, 1, 1);
	if (pollret == 0) {
		goto again;
	} else if (pollret < 0) {
		if (errno == EINTR) {
			goto again;
		} else {
			return errno;
		}
	}
	
	nread = read(fd, ds->ibuf + ds->sizeibuf, ds->capibuf - ds->sizeibuf);
	if (nread > 0) {
		ds->sizeibuf += (size_t)nread;
parseagain:
		nparsed = parsecmd(ds);
		if (nparsed > 0) {
			dispatchcmd(ds);
			if (ds->fdin < 0) {
				return -2;
			}
			ds->sizeibuf -= nparsed;
			if (ds->sizeibuf > 0) {
				memmove(ibuf, ibuf + nparsed, ds->sizeibuf);
				goto parseagain;
			}
			return 0;
		} else {
			goto again;
		}
		
	} else if (nread == 0) {
		return -1;
		
	} else {
		int err = errno;
		if (err == EAGAIN || err == EWOULDBLOCK) {
			int flags = fcntl(fd, F_GETFL, 0);
			if (flags != -1) {
				flags |= O_NONBLOCK;
				fcntl(fd, F_SETFL, flags);
			}
			goto again;
		} else if (err == EINTR) {
			goto again;
		} else {
			return err;
		}
	}
	return 0;
}

static void fg_interact(DebugState *ds)
{	
	while (ds->luacont != 1) {
		if (setjmp(ds->jmpbuf) == 0) {
			int err = processinput(ds);
			if (err != 0) {
				exit(err);
			}
			obflush(ds);
		} else {
			obsetfstr(ds, "fatal error: %s", ds->fatalerrmsg);
			write(ds->fdout, ds->obuf, ds->sizeobuf);
			close(ds->fdout);
			close(ds->fdin);
			exit(-1);
		}
	}
	ds->luacont = -1;
}

static void bg_interact(DebugState *ds)
{
	while (ds->luacont != 1) {
		pthread_mutex_lock(&ds->mutex);
		pthread_cond_wait(&ds->cond, &ds->mutex);
		pthread_mutex_unlock(&ds->mutex);
	}
	ds->luacont = -1;
}

static void accept_and_serve(DebugState *ds, int listen_fd)
{
	int flags;
	int fd;
	struct sockaddr_in sa;	
	socklen_t socklen = sizeof(sa);
	ds->fdin = -1;
	while (ds->fdin < 0) {
		fd = accept(listen_fd, (struct sockaddr*)&sa, &socklen);
		if (fd >= 0) {
			ds->fdin = fd;
			ds->fdout = fd;
			flags = fcntl(fd, F_GETFL, 0);
			if (flags != -1) {
				flags |= O_NONBLOCK;
				fcntl(fd, F_SETFL, flags);
			}
			ds->sizeibuf = 0;
			obpushstr(ds, SIZEDCSTR("Lua VM is runnning, use `pause` to pause it.\n"));
			obflush(ds);
		}
	}

	while (ds->fdin >= 0) {
		if (setjmp(ds->jmpbuf) == 0) {
			int err = processinput(ds);
			if (err != 0) {
				if (ds->fdin >= 0) {
					close(ds->fdin);
					ds->fdin = -1;
					ds->fdout = -1;
				}
				if (ds->luacont == 0) {
					cmd_continue(ds);
				}
				ds->argc = 2;
				delete_breaks(ds);
			}
			if (ds->luacont == 1) {
				obpushstr(ds, SIZEDCSTR("Lua VM continuing ... "));
				pthread_mutex_lock(&ds->mutex);
				pthread_cond_signal(&ds->cond);
				pthread_mutex_unlock(&ds->mutex);
			}
			obflush(ds);
			
		} else {
			obsetfstr(ds, "fatal error: %s", ds->fatalerrmsg);
			write(ds->fdout, ds->obuf, ds->sizeobuf);
			close(ds->fdout);
			close(ds->fdin);
			exit(-1);
		}
	}	
}

static void* server_thread(void *ptr)
{
	DebugState *ds = ptr;
	int listen_fd = ds->fdin;
	while (1) {
		accept_and_serve(ds, listen_fd);
	}
	return NULL;
}

static int onpanic(lua_State *L)
{
	DebugState *ds = GETDS(L);
	if (ds->fdin >= 0) {
		const char *msg = svalue(L->top - 1);
		obpushstr(ds, msg, strlen(msg));
		ds->why_setpause = 0;
		luaG_interrupt(L, 0);
	} else {
		const char *msg = svalue(L->top - 1);
		int fd;
		pid_t pid;
		char filename[64];
		
		updatecitop(ds);
		updatecifilepos(ds);

		ds->sizeobuf = 0;
		obpushstr(ds, msg, strlen(msg));
		obpushstr(ds, SIZEDCSTR("\n"));
		listrtsrc(ds);
		obpushstr(ds, SIZEDCSTR("\n############# arguments ############\n"));
		info_args(ds);
		obpushstr(ds, SIZEDCSTR("\n############ locals ############\n"));
		info_locals(ds);
		obpushstr(ds, SIZEDCSTR("\n############ upvals ############\n"));
		info_upvals(ds);

		pid = getpid();
		snprintf(filename, sizeof(filename), "ldb-%d.dump", pid);
		fd = creat(filename, 0644);
		if (fd >= 0) {
			write(fd, ds->obuf, ds->sizeobuf);
			close(fd);
		}
	}
	return 0;
}

static int starttcpserver(DebugState *ds, const char *addr)
{
	struct sockaddr_in sa;	
	socklen_t socklen;
	int listen_fd;
	int err;
	int reuseaddr = 1;
	
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		err = errno;
		goto errored;
	}
	
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&reuseaddr, sizeof(reuseaddr));
	sa.sin_family = AF_INET;
	inet_pton(AF_INET, addr, &sa.sin_addr);
	sa.sin_port = htons(LDBG_PORT);
	if (bind(listen_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 || listen(listen_fd, 1) < 0) {
		err = errno;
		goto errored;
	}

	if (ds->mode == 'b') {
		pthread_t pth;
		ds->fdout = -1;
		ds->fdin = listen_fd;
		pthread_mutex_init(&ds->mutex, NULL);
		pthread_cond_init(&ds->cond, NULL);
		err = pthread_create(&pth, NULL, server_thread, ds);
		if (err != 0) {
			goto errored;
		}
		
	} else {
		printf("debug server started, waiting for client ...\n");
		socklen = sizeof(sa);
		while (1) {
			int fd = accept(listen_fd, (struct sockaddr*)&sa, &socklen);
			if (fd >= 0) {
				int flags;
				close(listen_fd);
				listen_fd = -1;
				ds->fdin = fd;
				ds->fdout = fd;
				flags = fcntl(fd, F_GETFL, 0);
				if (flags != -1) {
					flags |= O_NONBLOCK;
					fcntl(fd, F_SETFL, flags);
				}
				printf("client connected \n");
				break;
			}
		}
	}
	return 0;
	
errored:
	if (listen_fd >= 0) {
		close(listen_fd);
	}
	return err;
	
}

int luaG_startserver(lua_State *L, char mode, const char *addr)
{
	DebugState *ds;
	int err;

	if (mode != 'i' && mode != 'f' && mode != 'b') {
		mode = 'i';
	}

	ds = malloc(sizeof(DebugState));
	if (ds == NULL) {
		err = ENOMEM;
		goto errored;
	}
	memset(ds, 0, sizeof(*ds));
	ds->mode = mode;
	ds->fdin = -1;
	ds->fdout = -1;
	ds->conf = DBGCONF;
	ds->pseudobp.id = ID_PSEUDOBP;
	ds->pseudobp.codepos = -1;
	ds->bpid = 1;
	ds->why_setpause = 0;
	ds->interact = mode == 'b' ? bg_interact : fg_interact;
	ds->luacont = -1;
	ds->L = L;
	G(L)->dbgstate = ds;
	G(L)->panic = onpanic;

	if (mode != 'i') {
		err = starttcpserver(ds, addr);
		if (err != 0) {
			goto errored;
		}		
	} else {
		ds->fdin = STDIN_FILENO;
		ds->fdout = STDOUT_FILENO;
	}

	if (mode != 'b') {
		luaG_interrupt(L, 0);
	}
	return 0;

errored:
	if (ds != NULL) {
		free(ds);
	}
	return err;
}


void luaG_interrupt(lua_State *L, int bpid)
{
	DebugState *ds = GETDS(L);
	int pauselua = 1;
	BreakPoint *bp = NULL;

	if (bpid != 0) {
		bp = getbreakpoint(ds, bpid);
		
	} else {
		int why = ds->why_setpause;
		BreakPoint *rbp;
		rbp = ds->restorebp;
		if (rbp) {
			if (rbp->flags & BP_TEMP) {
				freebreakpoint(ds, rbp);
			} else {
				/* restore interrupt code */
				rbp->p->code[rbp->codepos] = CREATE_Ax(OP_INTERRUPT, rbp->id);
			}
			ds->restorebp = NULL;
		}
		
		if (why == SETPAUSE_STEP) {
			CallInfo *ci = L->ci;
			Proto *p = ci_func(ci)->p;
		    int npc = pcRel(ci->u.l.savedpc, p);
			int newline = getfuncline(p, npc);

			if (ds->L == L && ds->ci == L->ci && newline == ds->oldline) {
				pauselua = 0;
			}
			
			if (pauselua) {
				UNSETPAUSE(ds);
				ds->why_setpause = 0;
			}
		} else {
			UNSETPAUSE(ds);
			ds->why_setpause = 0;
			if (why == SETPAUSE_NEXT) {
				pauselua = 0;
			}
		}
	}

	if (pauselua) {
		Proto *p;
		CallInfo *ci;

		if (!ds->L) {
			ds->L = L;
		}
		ds->rtbp = bp;
		updatecitop(ds);

		ci = ds->ci;
		p = ci_func(ci)->p;
		ds->oldpc = ds->ci->u.l.savedpc;
		if (bp) {
			ds->oldpc--;
		}
		ds->oldline = p->lineinfo[ds->oldpc - p->code];
		
		updatecifilepos(ds);
		obpushfstr(ds, "Lua VM paused at %s:%d\n", getstr(ds->rtsrcfile->filepath), ds->rtline);
		listrtsrc(ds);
		obflush(ds);
		ds->luacont = 0;
		ds->interact(ds);
	}
}


