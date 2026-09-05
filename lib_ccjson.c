/*
** ccjson library.
**
** JSON encode/decode for LuaJIT, backed by yyjson (vendored in this tree).
**
** Design notes (agreed spec):
**   * Module is loaded via require("ccjson"), registered in the core
**     preload table by an external plumbing patch (see patches/README.md);
**     it deliberately does NOT modify LuaJIT core files itself. The sibling
**     module ccmsgpack (lib_ccmsgpack.c) follows the same conventions and
**     shares the Lua.Array registry metatable and the NULL-lightuserdata
**     null value with this module.
**   * encode(value[, opts]) -> string
**       - root may be any JSON value (string/number/boolean/table/lightud)
**       - opts: pretty (bool), utf8 (bool), depth (int, default 16),
**         nan_literal (bool; write NaN/Inf as JSON5 NaN/Infinity),
**         nan_null (bool; write NaN/Inf as null, takes precedence over
**         nan_literal). Without the nan_* options NaN/Inf are an error.
**   * decode(str[, opts]) -> table
**       - root MUST be a JSON object or array (scalar roots rejected)
**       - opts: depth (int, default 16; limits doc->Lua conversion
**         recursion), utf8 (bool), json5 (bool; JSON5 reader: comments,
**         trailing commas, single quotes, unquoted keys, hex/extension
**         numbers, NaN/Infinity), nan (bool; bare NaN/Infinity allowance;
**         json5 is a superset). Default is strict RFC 8259.
**         pretty is ignored on decode
**   * Type mapping Lua -> JSON
**       int32      -> number            double -> number
**       int64/uint64 cdata -> number     OTHER cdata -> null
**       lightuserdata (any value) -> null
**       string/boolean -> string/boolean
**       nil as table member -> key skipped (absent)
**       nil at root / function / thread / userdata -> error
**       table with Lua.Array registry metatable -> array (empty marker
**         encodes as []; integer keys 1.. until the first nil)
**       ordinary table with a value at index 1 -> ARRAY of the longest
**         contiguous prefix 1..k. Everything after the first hole (higher
**         integer keys AND string keys) is SILENTLY DROPPED (R1 rule: a
**         hole is a bug signal -- visible truncation -- not a shape that
**         gets reshaped into an object). Plain empty table / no index 1 ->
**         object with keys stringified (number keys via tostring)
**   * Type mapping JSON -> Lua
**       object -> plain table (string keys)
**       array  -> table with the Lua.Array registry metatable attached
**       integer fitting int32  -> Lua int (dual-number builds) / number
**       integer in -(2^53)..2^53 -> Lua number (exact)
**       integer beyond 2^53 -> int64/uint64 cdata (if LJ_HASFFI),
**         otherwise falls back to double
**       real  -> double
**       null  -> NULL lightuserdata (same value as ccjson.null)
**   * Known asymmetries (documented, accepted):
**       - decode("[]") gives an empty table marked Lua.Array so re-encoding
**         yields []; decode("{}") yields a plain empty table -> {}.
**       - a Lua double whose integral value exceeds 2^53 encodes as a JSON
**         integer literal, which then decodes to int64/uint64 cdata.
**       - shared (non-cyclic) references are duplicated on encode; cycles
**         are caught by the depth limit.
**       - opts.utf8=true relaxes UTF-8 validation on both sides (byte-level
**         round trip of arbitrary byte strings). Default is strict RFC 8259.
**       - json5/nan decode treats overflow literals (e.g. 1e999) as +Inf;
**         nan_literal output (NaN/Infinity text) is not standard JSON.
**
** This file depends on LuaJIT internals (lj_*) and must be compiled into
** the core; it cannot be built as an external module against the public
** libluajit.so exports. The plumbing patches live in patches/ (see
** patches/README.md).
*/

#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_state.h"
#include "lj_strfmt.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "yyjson.h"

/* -- Constants ----------------------------------------------------------- */

#define CCJSON_DEFAULT_DEPTH	16
#define CCJSON_MAX_DEPTH	1024

/* Stack buffer for the non-allocating writer fast path. yyjson needs the
** buffer to be strictly larger than the JSON output (tail space for its
** writer context), so this serves documents up to roughly half this size
** for deep nesting, more for shallow ones; overflow falls back to the
** allocating writer.
**
** The 2048 byte cap is deliberate and non-negotiable: this tree is built
** with -DLUAJIT_UNWIND_EXTERNAL (the Makefile auto-detects -funwind-tables),
** so LuaJIT raises errors by unwinding the real C stack through libunwind.
** On macOS x86_64, clang frames larger than 4KB are allocated dynamically
** via ___chkstk_darwin, and libunwind's compact unwinder mis-unwinds such
** frames: any error raised inside ccjson_encode (even with the fast path
** never reached) then segfaults inside libunwind. Keeping the frame under
** the chkstk threshold avoids the broken unwinding. -O0 builds were
** unaffected, which is why this only bit release builds. */
#define CCJSON_STACK_BUF	2048

/* Registry name for the shared Lua.Array metatable. */
#define CCJSON_ARRAY_MT		"Lua.Array"

/* Exact upper bound of integers representable as Lua numbers (2^53). */
#define CCJSON_INT53		(((int64_t)1) << 53)

/* -- Error helpers ------------------------------------------------------- */

static void ccjson_error(lua_State *L, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  lua_pushvfstring(L, fmt, ap);
  va_end(ap);
  lua_error(L);
}

static const char *ccjson_typename(cTValue *o)
{
  if (tvisnil(o)) return "nil";
  if (tvisstr(o)) return "string";
  if (tvisnum(o) || tvisint(o)) return "number";
  if (tvistrue(o) || tvisfalse(o)) return "boolean";
  if (tvistab(o)) return "table";
  if (tvisfunc(o)) return "function";
  if (tvislightud(o)) return "lightuserdata";
  if (tvisudata(o)) return "userdata";
#if LJ_HASFFI
  if (tviscdata(o)) return "cdata";
#endif
  return "thread";
}

static void ccjson_argtype(lua_State *L, int narg, cTValue *o,
			   const char *fname, const char *want)
{
  const char *got = o ? ccjson_typename(o) : "no value";
  ccjson_error(L, "bad argument #%d to '%s' (%s expected, got %s)",
	       narg, fname, want, got);
}

/* -- Option parsing ------------------------------------------------------ */

/* Read a boolean field from an options table. -1 = field absent. */
static int ccjson_optbool(lua_State *L, GCtab *opts, const char *field)
{
  cTValue *v;
  if (!opts) return -1;
  v = lj_tab_getstr(opts, lj_str_new(L, field, strlen(field)));
  if (!v || !tvisbool(v)) return -1;
  return boolV(v);
}

/* Read an integer field from an options table. def = value if absent. */
static int ccjson_optint(lua_State *L, GCtab *opts, const char *field, int def)
{
  cTValue *v;
  int32_t i;
  if (!opts) return def;
  v = lj_tab_getstr(opts, lj_str_new(L, field, strlen(field)));
  if (!v) return def;
  if (tvisint(v)) {
    i = intV(v);
  } else if (tvisnum(v)) {
    lua_Number n = numV(v);
    if (n != (lua_Number)(int32_t)n)
      ccjson_error(L, "ccjson: option '%s' must be an integer", field);
    i = (int32_t)n;
  } else {
    ccjson_error(L, "ccjson: option '%s' must be an integer", field);
    return def;
  }
  if (i < 1 || i > CCJSON_MAX_DEPTH)
    ccjson_error(L, "ccjson: option '%s' out of range 1..%d", field,
		 CCJSON_MAX_DEPTH);
  return i;
}

/* -- Shared helpers ------------------------------------------------------ */

/* The Lua.Array registry metatable (identity of JSON arrays). */
static GCtab *ccjson_array_mt(lua_State *L)
{
  GCtab *reg = tabV(registry(L));
  cTValue *v = lj_tab_getstr(reg, lj_str_newlit(L, CCJSON_ARRAY_MT));
  return (v && tvistab(v)) ? tabV(v) : NULL;
}

#if LJ_HASFFI
/* Create an int64/uint64 cdata from 8 raw bytes and push it. */
static void ccjson_push_intcdata(lua_State *L, CTypeID ctypeid,
				 const void *pv)
{
  GCcdata *cd;
  ctype_loadffi(L);
  cd = lj_cdata_new_(L, ctypeid, 8);
  memcpy(cdataptr(cd), pv, 8);
  setcdataV(L, L->top, cd);
  L->top++;
}
#endif

/* -- Encoder ------------------------------------------------------------- */

static yyjson_mut_val *ccjson_put_value(lua_State *L, yyjson_mut_doc *doc,
					cTValue *o, GCtab *amt, int depth);

/* Push an anchor copy of a container table while iterating/descending, so
** that Lua allocations (numeric key formatting, nested conversions) cannot
** collect tables we hold C pointers into. Stack-neutral on exit.
*/
static yyjson_mut_val *ccjson_put_array(lua_State *L, yyjson_mut_doc *doc,
					GCtab *t, GCtab *amt, int depth)
{
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  int32_t i;
  TValue *ctv;
  if (!arr) ccjson_error(L, "ccjson.encode: out of memory");
  /* Anchor the table copy on the Lua stack. */
  ctv = L->top++;
  settabV(L, ctv, t);
  for (i = 1; ; i++) {
    cTValue *v = lj_tab_getint(t, i);
    yyjson_mut_val *jv;
    if (v == NULL || tvisnil(v)) break;  /* First nil ends the array. */
    jv = ccjson_put_value(L, doc, v, amt, depth);
    if (jv) yyjson_mut_arr_append(arr, jv);
  }
  L->top--;  /* Pop anchor. */
  return arr;
}

/* Encode an object. Iteration is semantic (lua_next), so it is independent
** of the internal array/hash layout of the table. Members whose value is
** nil cannot appear through next() (they are absent keys).
*/
static yyjson_mut_val *ccjson_put_object(lua_State *L, yyjson_mut_doc *doc,
					 GCtab *t, GCtab *amt, int depth)
{
  yyjson_mut_val *obj = yyjson_mut_obj(doc);
  TValue *ctv;
  if (!obj) ccjson_error(L, "ccjson.encode: out of memory");
  ctv = L->top++;
  settabV(L, ctv, t);  /* Anchor the table copy. */
  lua_pushnil(L);
  while (lua_next(L, -2)) {  /* Key at -2, value at -1. */
    cTValue *k = L->top - 2;
    cTValue *val = L->top - 1;
    GCstr *ks;
    const char *kp;
    MSize klen;
    yyjson_mut_val *kv, *jv;
    if (tvisstr(k)) {
      ks = strV(k);
      kp = strdata(ks);
      klen = ks->len;
    } else if (tvisint(k)) {
      ks = lj_strfmt_int(L, intV(k));
      kp = strdata(ks);
      klen = ks->len;
    } else if (tvisnum(k)) {
      ks = lj_strfmt_num(L, k);
      kp = strdata(ks);
      klen = ks->len;
    } else {
      ccjson_error(L, "ccjson.encode: unsupported table key type %s",
		   ccjson_typename(k));
      return NULL;
    }
    kv = yyjson_mut_strncpy(doc, kp, klen);
    jv = ccjson_put_value(L, doc, val, amt, depth);
    if (jv) yyjson_mut_obj_add(obj, kv, jv);
    lua_pop(L, 1);  /* Drop the value; lua_next will pop the key. */
  }
  L->top--;  /* Pop the anchor copy (lua_next popped the key). */
  return obj;
}

/* Encode one Lua value into a yyjson mut value. Returns NULL only for nil
** (which callers skip, except at the root where it is an error).
** Container nesting consumes one level of `depth` each.
*/
static yyjson_mut_val *ccjson_put_value(lua_State *L, yyjson_mut_doc *doc,
					cTValue *o, GCtab *amt, int depth)
{
  if (tvisnil(o)) {
    return NULL;
  } else if (tvisstr(o)) {
    GCstr *s = strV(o);
    return yyjson_mut_strncpy(doc, strdata(s), s->len);
  } else if (tvisint(o)) {
    return yyjson_mut_int(doc, (int64_t)intV(o));
  } else if (tvisnum(o)) {
    /* Integral doubles (e.g. 2.0, 2^40) are emitted as integer literals
    ** so output stays clean ("1", not "1.0"). */
    lua_Number d = numV(o);
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0 &&
	d == (lua_Number)(int64_t)d)
      return yyjson_mut_int(doc, (int64_t)d);
    return yyjson_mut_double(doc, d);
  } else if (tvistrue(o)) {
    return yyjson_mut_true(doc);
  } else if (tvisfalse(o)) {
    return yyjson_mut_false(doc);
  } else if (tvislightud(o)) {
    /* Any lightuserdata encodes as JSON null (spec). */
    return yyjson_mut_null(doc);
#if LJ_HASFFI
  } else if (tviscdata(o)) {
    /* Any 8-byte integer cdata (int64_t/uint64_t or user aliases) encodes
    ** as an integer; all other cdata encodes as null (spec). Classification
    ** follows lj_serialize: resolve the ctype, don't compare type ids. */
    CTState *cts = ctype_cts(L);
    GCcdata *cd = cdataV(o);
    CType *s = cts ? ctype_raw(cts, cd->ctypeid) : NULL;
    if (s && ctype_isinteger(s->info) && s->size == 8) {
      if (s->info & CTF_UNSIGNED)
	return yyjson_mut_uint(doc, *(uint64_t *)cdataptr(cd));
      return yyjson_mut_sint(doc, *(int64_t *)cdataptr(cd));
    }
    return yyjson_mut_null(doc);
#endif
  } else if (tvisudata(o) || tvisfunc(o) || tvisthread(o)) {
    ccjson_error(L, "ccjson.encode: unsupported value of type %s",
		 ccjson_typename(o));
    return NULL;
  } else if (tvistab(o)) {
    GCtab *t = tabV(o);
    if (depth <= 0)
      ccjson_error(L,
		   "ccjson.encode: container nesting exceeds depth limit "
		   "(possible cycle)");
    if (amt && tabref(t->metatable) == amt) {
      /* Marker table: JSON array (empty marker encodes as []). */
      return ccjson_put_array(L, doc, t, amt, depth - 1);
    } else if (lj_tab_getint(t, 1) == NULL || tvisnil(lj_tab_getint(t, 1))) {
      /* No element at index 1 (or empty table): encode as an object. */
      return ccjson_put_object(L, doc, t, amt, depth - 1);
    } else {
      /* R1 prefix rule: a table with a value at index 1 encodes as the
      ** longest contiguous prefix array 1..k (probe stops at the first
      ** hole). Everything after the hole -- higher integer keys AND string
      ** keys -- is silently dropped: a hole is treated as a bug signal
      ** (visible truncation) rather than being reshaped into an object.
      ** This is a deliberate, documented data-loss semantic. */
      return ccjson_put_array(L, doc, t, amt, depth - 1);
    }
  } else {
    ccjson_error(L, "ccjson.encode: unsupported value of type %s",
		 ccjson_typename(o));
    return NULL;
  }
}

/* Try to write the document into a fixed buffer owned by the caller (no
** malloc). yyjson's writer uses the buffer itself as its working area
** (content grows from the front, a context stack from the back) with a NULL
** allocator, so a buffer overflow surfaces as YYJSON_WRITE_ERROR_MEMORY_ALLOCATION
** and the write aborts early near the buffer end. Returns 1 on success (bytes
** written in *outlen), 0 if the document does not fit or on a real error
** (error code in *ecode).
*/
static int ccjson_write_stackbuf(yyjson_mut_doc *doc, yyjson_write_flag wflg,
				 char *buf, size_t buf_len,
				 size_t *outlen, yyjson_write_code *ecode)
{
  yyjson_write_err werr;
  size_t n;
  memset(&werr, 0, sizeof(werr));
  n = yyjson_mut_write_buf(buf, buf_len, doc, wflg, &werr);
  if (n > 0 && n < buf_len) {
    *outlen = n;
    return 1;
  }
  *ecode = werr.code;
  return 0;
}

static int ccjson_encode(lua_State *L)
{
  cTValue *o;
  GCtab *opts = NULL;
  GCtab *amt;
  yyjson_mut_doc *doc;
  yyjson_mut_val *root;
  yyjson_write_flag wflg = YYJSON_WRITE_NOFLAG;
  char *out;
  size_t outlen = 0;
  yyjson_write_err werr;
  int depth = CCJSON_DEFAULT_DEPTH;
  int pretty, utf8, nan_literal, nan_null;
  if (L->base < L->top) {
    o = L->base;
  } else {
    ccjson_error(L, "bad argument #1 to 'encode' (value expected, got no value)");
    return 0;
  }
  if (L->base + 1 < L->top && !tvisnil(&L->base[1])) {
    if (!tvistab(&L->base[1]))
      ccjson_argtype(L, 2, &L->base[1], "encode", "table");
    opts = tabV(&L->base[1]);
  }
  pretty = ccjson_optbool(L, opts, "pretty");
  utf8 = ccjson_optbool(L, opts, "utf8");
  depth = ccjson_optint(L, opts, "depth", CCJSON_DEFAULT_DEPTH);
  nan_literal = ccjson_optbool(L, opts, "nan_literal");
  nan_null = ccjson_optbool(L, opts, "nan_null");
  amt = ccjson_array_mt(L);
  lj_state_checkstack(L, depth * 4 + 64);
  if (pretty == 1) wflg |= YYJSON_WRITE_PRETTY_TWO_SPACES;
  if (utf8 == 1) wflg |= YYJSON_WRITE_ALLOW_INVALID_UNICODE;
  if (nan_literal == 1) wflg |= YYJSON_WRITE_ALLOW_INF_AND_NAN;
  if (nan_null == 1) wflg |= YYJSON_WRITE_INF_AND_NAN_AS_NULL;
  /* YYJSON_WRITE_INF_AND_NAN_AS_NULL takes precedence over the literal
  ** flag inside yyjson; default stays strict (NaN/Inf are an error). */
  doc = yyjson_mut_doc_new(NULL);
  if (!doc) ccjson_error(L, "ccjson.encode: out of memory");
  root = ccjson_put_value(L, doc, o, amt, depth);
  if (!root) {
    yyjson_mut_doc_free(doc);
    ccjson_error(L, "ccjson.encode: cannot encode nil at the root");
    return 0;
  }
  yyjson_mut_doc_set_root(doc, root);
  /* Fast path: non-allocating write into a stack buffer; fall back to the
  ** allocating writer when the document does not fit (yyjson reports that
  ** as MEMORY_ALLOCATION because the fixed-buffer writer runs without an
  ** allocator) or on any real write error (which the allocating writer
  ** then reports again with a proper message). */
  {
    char stackbuf[CCJSON_STACK_BUF];
    size_t flen = 0;
    yyjson_write_code fecode = 0;
    if (ccjson_write_stackbuf(doc, wflg, stackbuf, sizeof(stackbuf), &flen,
			      &fecode)) {
      setstrV(L, L->top, lj_str_new(L, stackbuf, flen));
      L->top++;
      yyjson_mut_doc_free(doc);
      return 1;
    }
  }
  out = yyjson_mut_write_opts(doc, wflg, NULL, &outlen, &werr);
  if (!out) {
    yyjson_mut_doc_free(doc);
    ccjson_error(L, "ccjson.encode: %s", werr.msg ? werr.msg : "write failed");
    return 0;
  }
  setstrV(L, L->top, lj_str_new(L, out, outlen));
  L->top++;
  free(out);
  yyjson_mut_doc_free(doc);
  return 1;
}

/* -- Decoder ------------------------------------------------------------- */

/* Convert a yyjson value and push the Lua result on the stack.
** Containers are kept anchored on the stack while being filled, so nested
** allocation can never collect a table under construction.
** `depth` is the number of container levels still allowed.
** The integer policy below is shared with ccmsgpack (lib_ccmsgpack.c);
** keep the two modules in sync.
*/
static void ccjson_get_value(lua_State *L, yyjson_val *v, GCtab *amt,
			     int depth)
{
  yyjson_type type = yyjson_get_type(v);
  if (type == YYJSON_TYPE_NULL) {
    /* NULL lightuserdata, same canonical value as ccjson.null. */
    lua_pushlightuserdata(L, NULL);
  } else if (type == YYJSON_TYPE_BOOL) {
    setboolV(L->top, yyjson_get_bool(v) ? 1 : 0);
    L->top++;
  } else if (type == YYJSON_TYPE_NUM) {
    if (yyjson_is_sint(v)) {
      int64_t i = yyjson_get_sint(v);
      if (i >= -2147483647LL - 1 && i <= 2147483647LL) {
#if LJ_DUALNUM
	setintV(L->top, (int32_t)i);
	L->top++;
#else
	setnumV(L->top, (lua_Number)i);
	L->top++;
#endif
      } else if (i >= -CCJSON_INT53 && i <= CCJSON_INT53) {
	setnumV(L->top, (lua_Number)i);
	L->top++;
      } else {
#if LJ_HASFFI
	ccjson_push_intcdata(L, CTID_INT64, &i);
#else
	setnumV(L->top, (lua_Number)i);  /* Lossy fallback (non-FFI build). */
	L->top++;
#endif
      }
    } else if (yyjson_is_uint(v)) {
      uint64_t u = yyjson_get_uint(v);
      if (u <= 2147483647ULL) {
#if LJ_DUALNUM
	setintV(L->top, (int32_t)u);
	L->top++;
#else
	setnumV(L->top, (lua_Number)u);
	L->top++;
#endif
      } else if (u <= (uint64_t)CCJSON_INT53) {
	setnumV(L->top, (lua_Number)u);
	L->top++;
      } else {
	/* Values above 2^53 cannot round-trip through double; make them
	** int64 cdata when they fit, uint64 cdata otherwise. yyjson stores
	** every non-negative integer as UINT, so reclassify here. */
#if LJ_HASFFI
	if (u <= (uint64_t)9223372036854775807LL) {
	  int64_t i = (int64_t)u;
	  ccjson_push_intcdata(L, CTID_INT64, &i);
	} else {
	  ccjson_push_intcdata(L, CTID_UINT64, &u);
	}
#else
	setnumV(L->top, (lua_Number)u);  /* Lossy fallback (non-FFI build). */
	L->top++;
#endif
      }
    } else {  /* YYJSON_SUBTYPE_REAL */
      setnumV(L->top, yyjson_get_real(v));
      L->top++;
    }
  } else if (type == YYJSON_TYPE_STR) {
    GCstr *s = lj_str_new(L, yyjson_get_str(v), yyjson_get_len(v));
    setstrV(L, L->top, s);
    L->top++;
  } else if (type == YYJSON_TYPE_ARR || type == YYJSON_TYPE_OBJ) {
    GCtab *t;
    size_t i, max = yyjson_get_len(v);
    if (depth <= 0)
      ccjson_error(L, "ccjson.decode: container nesting exceeds depth limit");
    if (max > (size_t)2147483647L)
      ccjson_error(L, "ccjson.decode: array too long");
    /* Pre-size the Lua table from the JSON container length: JSON arrays
    ** are dense, so pass the full array-part size (slot 0 is unused, hence
    ** max+1); JSON objects get enough hash slots to avoid rehashing while
    ** inserting max keys. Caps keep absurd inputs from over-allocating. */
    if (type == YYJSON_TYPE_ARR) {
      size_t ahint = max + 1;
      if (ahint > ((size_t)1 << 20)) ahint = (size_t)1 << 20;
      t = lj_tab_new(L, (uint32_t)ahint, 0);
    } else {
      uint32_t hbits = 1;
      size_t want = max + max;  /* ~50% load factor headroom. */
      while (hbits < 20 && want > ((size_t)1 << hbits)) hbits++;
      t = lj_tab_new(L, 0, hbits);
    }
    if (type == YYJSON_TYPE_ARR && amt)
      setgcref(t->metatable, obj2gco(amt));  /* Table is white: no barrier. */
    lj_gc_anybarriert(L, t);  /* Future raw stores need no barriers. */
    settabV(L, L->top, t);
    L->top++;
    if (type == YYJSON_TYPE_ARR) {
      for (i = 0; i < max; i++) {
	yyjson_val *el = yyjson_arr_get(v, i);
	TValue *slot;
	ccjson_get_value(L, el, amt, depth - 1);
	slot = lj_tab_setint(L, t, (int32_t)(i + 1));
	copyTV(L, slot, L->top - 1);
	L->top--;
      }
    } else {
      size_t idx;
      yyjson_val *key, *val;
      yyjson_obj_foreach(v, idx, max, key, val) {
	GCstr *ks = lj_str_new(L, yyjson_get_str(key), yyjson_get_len(key));
	TValue *slot;
	ccjson_get_value(L, val, amt, depth - 1);
	slot = lj_tab_setstr(L, t, ks);
	copyTV(L, slot, L->top - 1);
	L->top--;
      }
    }
    /* Container stays on the stack (anchored); leave it as the result. */
  } else {
    ccjson_error(L, "ccjson.decode: unsupported JSON value type %u",
		 (unsigned)type);
  }
}

static int ccjson_decode(lua_State *L)
{
  GCstr *str;
  GCtab *amt;
  GCtab *opts = NULL;
  yyjson_doc *doc;
  yyjson_val *root;
  yyjson_read_err rerr;
  yyjson_read_flag rflg = YYJSON_READ_NOFLAG;
  int depth = CCJSON_DEFAULT_DEPTH;
  int utf8, json5, nan;
  if (!(L->base < L->top && tvisstr(L->base)))
    ccjson_argtype(L, 1, (L->base < L->top) ? L->base : NULL, "decode",
		   "string");
  str = strV(L->base);
  /* Optional 2nd argument: options table (depth/utf8/json5/nan). */
  if (L->base + 1 < L->top && !tvisnil(&L->base[1])) {
    if (!tvistab(&L->base[1]))
      ccjson_argtype(L, 2, &L->base[1], "decode", "table");
    opts = tabV(&L->base[1]);
  }
  depth = ccjson_optint(L, opts, "depth", CCJSON_DEFAULT_DEPTH);
  utf8 = ccjson_optbool(L, opts, "utf8");
  json5 = ccjson_optbool(L, opts, "json5");
  nan = ccjson_optbool(L, opts, "nan");
  if (utf8 == 1) rflg |= YYJSON_READ_ALLOW_INVALID_UNICODE;
  /* JSON5 is a superset of the bare nan switch; default stays strict. */
  if (nan == 1) rflg |= YYJSON_READ_ALLOW_INF_AND_NAN;
  if (json5 == 1) rflg |= YYJSON_READ_JSON5;
  amt = ccjson_array_mt(L);
  lj_state_checkstack(L, depth * 4 + 64);
  memset(&rerr, 0, sizeof(rerr));
  doc = yyjson_read_opts((char *)strdata(str), str->len, rflg, NULL, &rerr);
  if (!doc) {
    ccjson_error(L, "ccjson.decode: %s (at byte %u)",
		 rerr.msg ? rerr.msg : "parse error", (unsigned)rerr.pos);
    return 0;
  }
  root = yyjson_doc_get_root(doc);
  if (!root || (yyjson_get_type(root) != YYJSON_TYPE_ARR &&
		yyjson_get_type(root) != YYJSON_TYPE_OBJ)) {
    yyjson_doc_free(doc);
    ccjson_error(L, "ccjson.decode: root value must be an object or an array");
    return 0;
  }
  ccjson_get_value(L, root, amt, depth);
  yyjson_doc_free(doc);
  return 1;
}

/* -- Module open --------------------------------------------------------- */

LUALIB_API int luaopen_ccjson(lua_State *L)
{
  /* Create the module table (the open function may be called with the
  ** module name as argument, e.g. by require; so never assume index 1). */
  lua_createtable(L, 0, 4);
  /* Register the canonical Lua.Array metatable in the registry. The table
  ** is left on the stack (either the fresh one or a pre-existing one). */
  /* module.Array = registry mt (identity marker for JSON arrays). */
  lua_pushliteral(L, "Array"); luaL_newmetatable(L, CCJSON_ARRAY_MT); lua_rawset(L, -3);
  /* module.null = NULL lightuserdata (JSON null mapping). */
  lua_pushliteral(L, "null"); lua_pushlightuserdata(L, NULL); lua_rawset(L, -3);
  /* Functions. */
  lua_pushliteral(L, "encode"); lua_pushcfunction(L, ccjson_encode); lua_rawset(L, -3);
  lua_pushliteral(L, "decode"); lua_pushcfunction(L, ccjson_decode); lua_rawset(L, -3);
  return 1;
}
