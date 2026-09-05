/*
** ccmsgpack library.
**
** MessagePack encode/decode for LuaJIT. Built-in module (require "ccmsgpack"),
** self-contained codec: no third-party engine, no intermediate document.
**
** Design notes (agreed spec, mirrors ccjson conventions where sensible):
**   * Module is loaded via require("ccmsgpack"), registered in the core
**     preload table by an external plumbing patch (see patches/README.md);
**     the module source does not modify LuaJIT core files itself.
**   * encode(value[, opts]) -> string
**       - opts: depth (int, default 16); other fields ignored.
**       - root may be ANY value, including nil (encoded as msgpack nil).
**   * decode(str[, opts]) -> value
**       - root may be any msgpack value (nil decodes to ccmsgpack.null).
**       - opts: depth (int, default 16, range 1..1024), enforced DURING
**         parsing; other fields ignored.
**       - the whole input must be exactly one msgpack value (trailing or
**         truncated input is an error, reported with a byte offset).
**   * Lua -> msgpack
**       nil (as member)      -> key skipped / ends the array prefix
**       NULL lightuserdata (ccmsgpack.null / ccjson.null) -> nil (0xc0)
**       boolean              -> true/false
**       int32 / number       -> minimal-width integer when integral and in
**                               int64 range, otherwise float64 (NaN/Inf OK)
**       int64/uint64 cdata   -> minimal-width int64/uint64
**       string               -> str header + raw bytes (no UTF-8 checks)
**       table                -> see container rule below
**       function/thread/userdata/other cdata -> error
**   * msgpack -> Lua
**       nil                  -> NULL lightuserdata (ccmsgpack.null)
**       integers             -> int32 (dual-number builds) / number when in
**                               -(2^53)..2^53, else int64/uint64 cdata
**                               (plain double fallback when !LJ_HASFFI)
**       float32/float64      -> Lua number
**       str/bin              -> Lua string (raw bytes)
**       array                -> table carrying the Lua.Array metatable
**       map                  -> plain table; integer keys are kept as Lua
**                               integer keys (no stringification)
**       ext/fixext           -> unsupported (error)
**   * Container rule (R1 prefix truncation, same as ccjson):
**       table with Lua.Array metatable   -> array (empty marker -> array(0))
**       table with a value at index 1    -> array of the longest contiguous
**           prefix 1..k; everything after the first hole (higher integer
**           keys AND string keys) is SILENTLY DROPPED. A hole is treated as
**           a bug signal (visible truncation), deliberately lossy.
**       otherwise (no index 1, incl. {}) -> map (integer keys kept native)
**   * Notes / asymmetries:
**       - nil elements inside a decoded msgpack array become ccmsgpack.null
**         sentinels, so re-encoding preserves array positions.
**       - map keys are limited to strings and int32-range integers on both
**         encode and decode (float or oversized integer keys: error).
**       - empty plain {} <-> map(0); marker array <-> array(0).
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
#include "lj_buf.h"
#include "lj_udata.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif

/* -- Constants ----------------------------------------------------------- */

#define MP_DEFAULT_DEPTH  16
#define MP_MAX_DEPTH      1024

/* Registry name of the shared Lua.Array metatable (shared with ccjson). */
#define MP_ARRAY_MT		"Lua.Array"

#define MP_MAX_COUNT		0x7fffffffL  /* Sanity cap for container sizes. */

/* MessagePack format markers. */
#define MP_NIL			0xc0
#define MP_FALSE		0xc2
#define MP_TRUE			0xc3
#define MP_BIN8			0xc4
#define MP_BIN16		0xc5
#define MP_BIN32		0xc6
#define MP_EXT8			0xc7
#define MP_EXT16		0xc8
#define MP_EXT32		0xc9
#define MP_FLOAT32  0xca
#define MP_FLOAT64  0xcb
#define MP_UINT8		0xcc
#define MP_UINT16		0xcd
#define MP_UINT32		0xce
#define MP_UINT64		0xcf
#define MP_INT8			0xd0
#define MP_INT16		0xd1
#define MP_INT32		0xd2
#define MP_INT64		0xd3
#define MP_FIXEXT1  0xd4
#define MP_FIXEXT2  0xd5
#define MP_FIXEXT4  0xd6
#define MP_FIXEXT8  0xd7
#define MP_FIXEXT16 0xd8
#define MP_STR8			0xd9
#define MP_STR16		0xda
#define MP_STR32		0xdb
#define MP_ARRAY16  0xdc
#define MP_ARRAY32  0xdd
#define MP_MAP16		0xde
#define MP_MAP32		0xdf

/* -- Error helpers ------------------------------------------------------- */

/* These deliberately mirror lib_ccjson.c (identical wording and prefix
** style); keep the two copies in sync when changing error messages. */
static void mp_error(lua_State *L, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  lua_pushvfstring(L, fmt, ap);
  va_end(ap);
  lua_error(L);
}

static const char *mp_typename(cTValue *o)
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

static void mp_argtype(lua_State *L, int narg, cTValue *o,
		       const char *fname, const char *want)
{
  const char *got = o ? mp_typename(o) : "no value";
  mp_error(L, "bad argument #%d to '%s' (%s expected, got %s)",
	   narg, fname, want, got);
}

/* -- Shared helpers ------------------------------------------------------ */

static GCtab *mp_array_mt(lua_State *L)
{
  GCtab *reg = tabV(registry(L));
  cTValue *v = lj_tab_getstr(reg, lj_str_newlit(L, MP_ARRAY_MT));
  return (v && tvistab(v)) ? tabV(v) : NULL;
}

/* Map keys: only strings and int32-range integers are supported. */
static int mp_key_int(cTValue *k, int32_t *iv)
{
  if (tvisint(k)) {
    *iv = intV(k);
    return 1;
  }
  if (tvisnum(k)) {
    lua_Number d = numV(k);
    if (d >= -2147483648.0 && d < 2147483648.0 &&
	d == (lua_Number)(int32_t)d) {
      *iv = (int32_t)d;
      return 1;
    }
  }
  return 0;
}

/* Read the 'depth' option from an options table (def when absent). Shared
** by encode/decode so the two functions expose one uniform options shape. */
static int mp_opt_depth(lua_State *L, GCtab *opts, int def)
{
  cTValue *dv;
  int32_t i;
  if (!opts) return def;
  dv = lj_tab_getstr(opts, lj_str_newlit(L, "depth"));
  if (!dv || tvisnil(dv)) return def;
  if (tvisint(dv)) {
    i = intV(dv);
  } else if (tvisnum(dv)) {
    lua_Number n = numV(dv);
    if (n != (lua_Number)(int32_t)n)
      mp_error(L, "ccmsgpack: option 'depth' must be an integer");
    i = (int32_t)n;
  } else {
    mp_error(L, "ccmsgpack: option 'depth' must be an integer");
    i = def;
  }
  if (i < 1 || i > MP_MAX_DEPTH)
    mp_error(L, "ccmsgpack: option 'depth' out of range 1..%d", MP_MAX_DEPTH);
  return i;
}

#if LJ_HASFFI
/* Create an int64/uint64 cdata from 8 raw bytes and store it in `out`. */
static void mp_fill_intcdata(lua_State *L, TValue *out, CTypeID ctypeid,
			     const void *pv)
{
  GCcdata *cd;
  ctype_loadffi(L);
  cd = lj_cdata_new_(L, ctypeid, 8);
  memcpy(cdataptr(cd), pv, 8);
  setcdataV(L, out, cd);
}
#endif

/* Store the canonical NULL lightuserdata (same value as module .null,
** created identically to lua_pushlightuserdata(L, NULL)). */
static LJ_AINLINE void mp_fill_null(lua_State *L, TValue *out)
{
#if LJ_64
  setrawlightudV(out, lj_lightud_intern(L, NULL));
#else
  setrawlightudV(out, NULL);
#endif
}

/* -- Encoder: low-level writers ------------------------------------------ */

/* Reserve/grow space; mirrors lj_serialize's growth idiom for borrowed SBuf. */
static LJ_AINLINE char *mp_more(SBufExt *sbx, char *w, MSize sz)
{
  if (LJ_UNLIKELY(sz > (MSize)(sbx->e - w))) {
    sbx->w = w;
    w = lj_buf_more2((SBuf *)sbx, sz);
  }
  return w;
}

static LJ_AINLINE char *mp_wu16(char *w, uint32_t v)
{
  w[0] = (char)(v >> 8); w[1] = (char)v;
  return w + 2;
}

static LJ_AINLINE char *mp_wu32(char *w, uint32_t v)
{
  w[0] = (char)(v >> 24); w[1] = (char)(v >> 16);
  w[2] = (char)(v >> 8); w[3] = (char)v;
  return w + 4;
}

static LJ_AINLINE char *mp_wu64(char *w, uint64_t v)
{
  w[0] = (char)(v >> 56); w[1] = (char)(v >> 48);
  w[2] = (char)(v >> 40); w[3] = (char)(v >> 32);
  w[4] = (char)(v >> 24); w[5] = (char)(v >> 16);
  w[6] = (char)(v >> 8); w[7] = (char)v;
  return w + 8;
}

static char *mp_put_raw(SBufExt *sbx, char *w, const char *p, MSize len)
{
  w = mp_more(sbx, w, len);
  w = lj_buf_wmem(w, p, len);
  return w;
}

static char *mp_put_str_header(SBufExt *sbx, char *w, MSize len)
{
  if (len <= 31) {
    w = mp_more(sbx, w, 1);
    *w++ = (char)(0xa0 | len);  /* fixstr */
  } else if (len <= 0xff) {
    w = mp_more(sbx, w, 2);
    *w++ = MP_STR8;
    *w++ = (char)len;
  } else if (len <= 0xffff) {
    w = mp_more(sbx, w, 3);
    *w++ = MP_STR16;
    w = mp_wu16(w, (uint32_t)len);
  } else {
    if (LJ_UNLIKELY(len > 0xffffffffu))
      mp_error(sbufL(sbx), "ccmsgpack.encode: string too long");
    w = mp_more(sbx, w, 5);
    *w++ = MP_STR32;
    w = mp_wu32(w, (uint32_t)len);
  }
  return w;
}

static char *mp_put_len_header(SBufExt *sbx, char *w, uint32_t n,
			       int is_map)
{
  /* Arrays: n <= 15 -> fixarray | n. Maps: n <= 15 -> fixmap | n. */
  if (n <= 15) {
    w = mp_more(sbx, w, 1);
    *w++ = (char)((is_map ? 0x80 : 0x90) | n);
  } else if (n <= 0xffff) {
    w = mp_more(sbx, w, 3);
    *w++ = is_map ? MP_MAP16 : MP_ARRAY16;
    w = mp_wu16(w, n);
  } else {
    w = mp_more(sbx, w, 5);
    *w++ = is_map ? MP_MAP32 : MP_ARRAY32;
    w = mp_wu32(w, n);
  }
  return w;
}

/* -- Encoder: values ----------------------------------------------------- */

static char *mp_put_sint64(SBufExt *sbx, char *w, int64_t v);
static char *mp_put_value(lua_State *L, SBufExt *sbx, char *w, cTValue *o,
			  GCtab *amt, int depth);

static char *mp_put_uint64(SBufExt *sbx, char *w, uint64_t v)
{
  if (v <= 0x7f) {
    w = mp_more(sbx, w, 1);
    *w++ = (char)v;
  } else if (v <= 0xff) {
    w = mp_more(sbx, w, 2);
    *w++ = MP_UINT8; *w++ = (char)v;
  } else if (v <= 0xffff) {
    w = mp_more(sbx, w, 3);
    *w++ = MP_UINT16; w = mp_wu16(w, (uint32_t)v);
  } else if (v <= 0xffffffffu) {
    w = mp_more(sbx, w, 5);
    *w++ = MP_UINT32; w = mp_wu32(w, (uint32_t)v);
  } else {
    w = mp_more(sbx, w, 9);
    *w++ = MP_UINT64; w = mp_wu64(w, v);
  }
  return w;
}

static char *mp_put_sint64(SBufExt *sbx, char *w, int64_t v)
{
  if (v >= 0) {
    return mp_put_uint64(sbx, w, (uint64_t)v);
  } else if (v >= -32) {
    w = mp_more(sbx, w, 1);
    *w++ = (char)v;  /* negative fixint */
  } else if (v >= -128) {
    w = mp_more(sbx, w, 2);
    *w++ = MP_INT8; *w++ = (char)v;
  } else if (v >= -32768) {
    w = mp_more(sbx, w, 3);
    *w++ = MP_INT16; w = mp_wu16(w, (uint32_t)(int16_t)v);
  } else if (v >= -2147483648LL) {
    w = mp_more(sbx, w, 5);
    *w++ = MP_INT32; w = mp_wu32(w, (uint32_t)(int32_t)v);
  } else {
    w = mp_more(sbx, w, 9);
    *w++ = MP_INT64; w = mp_wu64(w, (uint64_t)v);
  }
  return w;
}

static char *mp_put_double(SBufExt *sbx, char *w, double d)
{
  uint64_t u;
  memcpy(&u, &d, 8);
  w = mp_more(sbx, w, 9);
  *w++ = MP_FLOAT64;
  return mp_wu64(w, u);
}

static char *mp_put_string(SBufExt *sbx, char *w, GCstr *s)
{
  w = mp_put_str_header(sbx, w, s->len);
  return mp_put_raw(sbx, w, strdata(s), s->len);
}

/* Structural helpers for direct table iteration (mirrors lj_serialize's
** approach: no lua_next, no Lua-stack traffic). In this tree the array part
** is indexed by the integer key itself: slot i holds the value of key i
** (slot 0 is only populated when the table has a key 0). Hash nodes hold
** everything else (string keys, and integer keys only when >= asize).
** Encode allocates no Lua objects, so GC cannot run and raw pointers into
** the table are safe.
*/

/* Map/object entry count without touching the Lua stack. */
static uint32_t mp_count_entries(GCtab *t)
{
  uint32_t count = 0;
  ptrdiff_t i;
  MSize asize = t->asize;
  cTValue *arr = asize ? tvref(t->array) : NULL;
  for (i = 0; i < (ptrdiff_t)asize; i++)  /* Slot i holds key i. */
    if (arr && !tvisnil(&arr[i])) count++;
  if (t->hmask != 0) {
    MSize hmask = t->hmask;
    Node *node = noderef(t->node);
    for (i = 0; i <= (ptrdiff_t)hmask; i++)
      if (!tvisnil(&node[i].val)) count++;
  }
  return count;
}

/* Array prefix length: first index i >= 1 whose value is nil, minus one.
** The array part is scanned directly; when the array part is completely
** full the probe continues into the hash part with lj_tab_getint (keys at
** or beyond asize can live there). */
static uint32_t mp_prefix_len(GCtab *t)
{
  uint32_t count = 0;
  ptrdiff_t i;
  ptrdiff_t asize = (ptrdiff_t)t->asize;
  cTValue *arr = asize ? tvref(t->array) : NULL;
  for (i = 1; i < asize; i++) {  /* Slot 0 never used. */
    if (arr == NULL || tvisnil(&arr[i])) return count;
    count++;
  }
  for (i = asize > 0 ? asize : 1; ; i++) {
    cTValue *v = lj_tab_getint(t, i);
    if (v == NULL || tvisnil(v)) return count;
    if (count >= (uint32_t)MP_MAX_COUNT)
      return (uint32_t)MP_MAX_COUNT;  /* Caller checks the cap. */
    count++;
  }
}

/* Encode a plain (non-marker) object/map table by direct structural
** iteration: array-part slots (integer keys 1..asize-1) then hash nodes.
** Values from next() can never be nil; count is computed structurally so
** only ONE pass over the entries is needed for emission.
*/
static char *mp_put_map(lua_State *L, SBufExt *sbx, char *w, GCtab *t, GCtab *amt, int depth)
{
  uint32_t count = mp_count_entries(t);
  ptrdiff_t i;
  ptrdiff_t asize = (ptrdiff_t)t->asize;
  cTValue *arr = asize ? tvref(t->array) : NULL;
  if (count > (uint32_t)MP_MAX_COUNT)
    mp_error(L, "ccmsgpack.encode: table too large");
  w = mp_put_len_header(sbx, w, count, 1);
  /* Array part: integer keys 0..asize-1 (slot i holds key i). */
  for (i = 0; i < asize; i++) {
    if (arr && !tvisnil(&arr[i])) {
      w = mp_put_sint64(sbx, w, (int64_t)i);
      w = mp_put_value(L, sbx, w, &arr[i], amt, depth);
    }
  }
  /* Hash part: arbitrary keys (string or int32-range integer). */
  if (t->hmask != 0) {
    MSize hmask = t->hmask;
    Node *node = noderef(t->node);
    for (i = 0; i <= (ptrdiff_t)hmask; i++) {
      cTValue *k;
      if (tvisnil(&node[i].val)) continue;
      k = &node[i].key;
      if (tvisstr(k)) {
	      w = mp_put_string(sbx, w, strV(k));
      } else {
        int32_t iv;
        if (!mp_key_int(k, &iv))
          mp_error(L, "ccmsgpack.encode: unsupported map key type %s", mp_typename(k));
        w = mp_put_sint64(sbx, w, (int64_t)iv);
      }
      w = mp_put_value(L, sbx, w, &node[i].val, amt, depth);
    }
  }
  return w;
}

/* Encode an array: longest contiguous prefix 1..k (R1 rule; probe stops at
** the first hole). The caller has already decided this table is an array.
*/
static char *mp_put_array(lua_State *L, SBufExt *sbx, char *w, GCtab *t,
			  GCtab *amt, int depth)
{
  uint32_t count = mp_prefix_len(t);
  ptrdiff_t i;
  ptrdiff_t asize = (ptrdiff_t)t->asize;
  cTValue *arr = asize ? tvref(t->array) : NULL;
  if (count > (uint32_t)MP_MAX_COUNT)
    mp_error(L, "ccmsgpack.encode: array too long");
  w = mp_put_len_header(sbx, w, count, 0);
  for (i = 1; i <= (ptrdiff_t)count; i++) {
    cTValue *v = (i < asize) ? &arr[i] : lj_tab_getint(t, i);
    w = mp_put_value(L, sbx, w, v, amt, depth);
  }
  return w;
}

static char *mp_put_value(lua_State *L, SBufExt *sbx, char *w, cTValue *o,
			  GCtab *amt, int depth)
{
  if (tvisnil(o) || tvislightud(o)) {
    /* nil and any lightuserdata encode as msgpack nil. */
    w = mp_more(sbx, w, 1);
    *w++ = MP_NIL;
  } else if (tvisstr(o)) {
    w = mp_put_string(sbx, w, strV(o));
  } else if (tvisint(o)) {
    w = mp_put_sint64(sbx, w, (int64_t)intV(o));
  } else if (tvisnum(o)) {
    lua_Number d = numV(o);
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0 && d == (lua_Number)(int64_t)d)
      w = mp_put_sint64(sbx, w, (int64_t)d);
    else
      w = mp_put_double(sbx, w, d);
  } else if (tvistrue(o)) {
    w = mp_more(sbx, w, 1);
    *w++ = MP_TRUE;
  } else if (tvisfalse(o)) {
    w = mp_more(sbx, w, 1);
    *w++ = MP_FALSE;
#if LJ_HASFFI
  } else if (tviscdata(o)) {
    CTState *cts = ctype_cts(L);
    GCcdata *cd = cdataV(o);
    CType *s = cts ? ctype_raw(cts, cd->ctypeid) : NULL;
    if (s && ctype_isinteger(s->info) && s->size == 8) {
      if (s->info & CTF_UNSIGNED)
	      w = mp_put_uint64(sbx, w, *(uint64_t *)cdataptr(cd));
      else
	      w = mp_put_sint64(sbx, w, *(int64_t *)cdataptr(cd));
    } else {
      mp_error(L, "ccmsgpack.encode: unsupported cdata value");
    }
#endif
  } else if (tvisudata(o) || tvisfunc(o) || tvisthread(o)) {
    mp_error(L, "ccmsgpack.encode: unsupported value of type %s",
	     mp_typename(o));
  } else if (tvistab(o)) {
    GCtab *t = tabV(o);
    cTValue *v1;
    if (depth <= 0)
      mp_error(L,
	       "ccmsgpack.encode: container nesting exceeds depth limit "
	       "(possible cycle)");
    if (amt && tabref(t->metatable) == amt) {
      /* Marker table: array (empty marker -> array(0)). */
      return mp_put_array(L, sbx, w, t, amt, depth - 1);
    }
    v1 = lj_tab_getint(t, 1);
    if (v1 == NULL || tvisnil(v1)) {
      /* No element at index 1 (or empty): map. */
      return mp_put_map(L, sbx, w, t, amt, depth - 1);
    }
    /* R1 prefix rule (same semantics and data-loss note as ccjson). */
    return mp_put_array(L, sbx, w, t, amt, depth - 1);
  } else {
    mp_error(L, "ccmsgpack.encode: unsupported value of type %s",
	     mp_typename(o));
  }
  return w;
}

/* -- Encoder entry ------------------------------------------------------- */

static int mp_encode(lua_State *L)
{
  cTValue *o;
  GCtab *opts = NULL;
  GCtab *amt;
  SBufExt sbx;
  char *w;
  int depth = MP_DEFAULT_DEPTH;
  if (L->base < L->top) {
    o = L->base;
  } else {
    mp_error(L, "bad argument #1 to 'encode' (value expected, got no value)");
    return 0;
  }
  if (L->base + 1 < L->top && !tvisnil(&L->base[1])) {
    if (!tvistab(&L->base[1]))
      mp_argtype(L, 2, &L->base[1], "encode", "table");
    opts = tabV(&L->base[1]);
  }
  depth = mp_opt_depth(L, opts, MP_DEFAULT_DEPTH);
  amt = mp_array_mt(L);
  lj_state_checkstack(L, depth * 4 + 64);
  memset(&sbx, 0, sizeof(SBufExt));
  lj_bufx_set_borrow(L, &sbx, &G(L)->tmpbuf);
  w = mp_put_value(L, &sbx, sbx.w, o, amt, depth);
  setstrV(L, L->top, lj_str_new(L, sbx.b, (MSize)(w - sbx.b)));
  L->top++;
  return 1;
}

/* -- Decoder ------------------------------------------------------------- */

/* Parse cursor. `p` advances, `s`/`w` bound the input (never read past w,
** offsets in error messages are relative to s). */
typedef struct {
  const char *p;
  const char *w;
  const char *s;
} MPCursor;

static void mp_trunc(lua_State *L, MPCursor *c)
{
  mp_error(L, "ccmsgpack.decode: truncated input at byte %d",
	   (int)(c->p - c->s));
}

static LJ_AINLINE uint8_t mp_rd8(lua_State *L, MPCursor *c)
{
  if (c->p >= c->w) mp_trunc(L, c);
  return (uint8_t)*c->p++;
}

static LJ_AINLINE uint32_t mp_rd16(lua_State *L, MPCursor *c)
{
  const char *p;
  if ((MSize)(c->w - c->p) < 2) mp_trunc(L, c);
  p = c->p;
  c->p = p + 2;
  return ((uint32_t)(uint8_t)p[0] << 8) | (uint8_t)p[1];
}

static LJ_AINLINE uint32_t mp_rd32(lua_State *L, MPCursor *c)
{
  const char *p;
  if ((MSize)(c->w - c->p) < 4) mp_trunc(L, c);
  p = c->p;
  c->p = p + 4;
  return ((uint32_t)(uint8_t)p[0] << 24) | ((uint32_t)(uint8_t)p[1] << 16) |
	 ((uint32_t)(uint8_t)p[2] << 8) | (uint8_t)p[3];
}

static LJ_AINLINE uint64_t mp_rd64(lua_State *L, MPCursor *c)
{
  uint64_t hi, lo;
  if ((MSize)(c->w - c->p) < 8) mp_trunc(L, c);
  hi = mp_rd32(L, c);
  lo = mp_rd32(L, c);
  return (hi << 32) | lo;
}

static LJ_AINLINE void mp_rdskip(lua_State *L, MPCursor *c, MSize n)
{
  if ((MSize)(c->w - c->p) < n) mp_trunc(L, c);
  c->p += n;
}

/* Store a Lua number from a decoded 64-bit integer directly in `out`,
** following the ccjson policy: int32 -> int (dual-number builds) or
** number; |v| <= 2^53 -> number; beyond -> int64/uint64 cdata (or lossy
** double without FFI). */
static void mp_fill_signed(lua_State *L, TValue *out, int64_t v)
{
  if (v >= -2147483647LL - 1 && v <= 2147483647LL) {
#if LJ_DUALNUM
    setintV(out, (int32_t)v);
#else
    setnumV(out, (lua_Number)v);
#endif
  } else if (v >= -((int64_t)1 << 53) && v <= ((int64_t)1 << 53)) {
    setnumV(out, (lua_Number)v);
  } else {
#if LJ_HASFFI
    mp_fill_intcdata(L, out, CTID_INT64, &v);
#else
    setnumV(out, (lua_Number)v);  /* Lossy fallback (non-FFI build). */
#endif
  }
}

static void mp_fill_unsigned(lua_State *L, TValue *out, uint64_t u)
{
  if (u <= 2147483647ULL) {
#if LJ_DUALNUM
    setintV(out, (int32_t)u);
#else
    setnumV(out, (lua_Number)u);
#endif
  } else if (u <= ((uint64_t)1 << 53)) {
    setnumV(out, (lua_Number)u);
  } else {
#if LJ_HASFFI
    if (u <= (uint64_t)9223372036854775807LL) {
      int64_t v = (int64_t)u;
      mp_fill_intcdata(L, out, CTID_INT64, &v);
    } else {
      mp_fill_intcdata(L, out, CTID_UINT64, &u);
    }
#else
    setnumV(out, (lua_Number)u);  /* Lossy fallback (non-FFI build). */
#endif
  }
}

/* Read an integer map key of any msgpack width; the value must fit int32.
** Returns 0 on success (value in *out), raises otherwise. */
static void mp_rd_int32(lua_State *L, MPCursor *c, uint8_t b, int32_t *out)
{
  int64_t v;
  if (b <= 0x7f) {
    v = (int64_t)b;
  } else if (b >= 0xe0) {
    v = (int64_t)(int8_t)b;
  } else {
    switch (b) {
    case MP_UINT8: v = mp_rd8(L, c); break;
    case MP_UINT16: v = mp_rd16(L, c); break;
    case MP_UINT32: v = mp_rd32(L, c); break;
    case MP_UINT64: {
      uint64_t u = mp_rd64(L, c);
      if (u > (uint64_t)2147483647LL)
	      mp_error(L, "ccmsgpack.decode: integer map key out of int32 range "
		 "at byte %d", (int)(c->p - c->s) - 1);
      v = (int64_t)u;
      break;
    }
    case MP_INT8: v = (int64_t)(int8_t)mp_rd8(L, c); break;
    case MP_INT16: v = (int64_t)(int16_t)mp_rd16(L, c); break;
    case MP_INT32: v = (int64_t)(int32_t)mp_rd32(L, c); break;
    case MP_INT64: v = (int64_t)mp_rd64(L, c); break;
    default:
      mp_error(L, "ccmsgpack.decode: invalid marker 0x%02x at byte %d",
	       b, (int)(c->p - c->s) - 1);
      return;
    }
  }
  if (v < -2147483648LL || v > 2147483647LL)
    mp_error(L, "ccmsgpack.decode: integer map key out of int32 range "
	     "at byte %d", (int)(c->p - c->s) - 1);
  *out = (int32_t)v;
}

/* Decode a map key. Keys are strings (return 0, *kstr set) or int32-range
** integers (return 1, *kint set); anything else raises. String keys are
** interned, so they stay reachable while the value is decoded. */
static int mp_rd_key(lua_State *L, MPCursor *c, GCstr **kstr, int32_t *kint)
{
  uint8_t b = mp_rd8(L, c);
  const char *p;
  MSize len;
  if (b >= 0xa0 && b <= 0xbf) {  /* fixstr */
    len = b & 0x1f;
    mp_rdskip(L, c, len);
    p = c->p - len;
    *kstr = lj_str_new(L, p, len);
    return 0;
  }
  if (b == MP_STR8 || b == MP_STR16 || b == MP_STR32 ||
      b == MP_BIN8 || b == MP_BIN16 || b == MP_BIN32) {
    if (b == MP_STR8 || b == MP_BIN8) len = mp_rd8(L, c);
    else if (b == MP_STR16 || b == MP_BIN16) len = (MSize)mp_rd16(L, c);
    else len = (MSize)mp_rd32(L, c);
    mp_rdskip(L, c, len);
    p = c->p - len;
    *kstr = lj_str_new(L, p, len);
    return 0;
  }
  if (b <= 0x7f || b >= 0xe0 ||
      (b >= MP_UINT8 && b <= MP_INT64)) {
    mp_rd_int32(L, c, b, kint);
    return 1;
  }
  mp_error(L, "ccmsgpack.decode: unsupported map key type (marker 0x%02x) "
	   "at byte %d", b, (int)(c->p - c->s) - 1);
  return 0;
}

/* Decode a container header; returns its element/pair count. */
static uint32_t mp_rd_count(lua_State *L, MPCursor *c, uint8_t b)
{
  uint32_t n;
  if (b >= 0x90 && b <= 0x9f) return b & 0x0f;  /* fixarray */
  if (b >= 0x80 && b <= 0x8f) return b & 0x0f;  /* fixmap */
  if (b == MP_ARRAY16 || b == MP_MAP16) n = mp_rd16(L, c);
  else if (b == MP_ARRAY32 || b == MP_MAP32) n = mp_rd32(L, c);
  else {
    mp_error(L, "ccmsgpack.decode: invalid container marker 0x%02x at byte %d",
	     b, (int)(c->p - c->s) - 1);
    return 0;
  }
  /* Every element occupies at least one byte: reject impossible counts
  ** before allocating anything. */
  if ((uint64_t)n > (uint64_t)(c->w - c->p)) mp_trunc(L, c);
  if (n > (uint32_t)MP_MAX_COUNT)
    mp_error(L, "ccmsgpack.decode: container too large at byte %d",
	     (int)(c->p - c->s) - 1);
  return n;
}

/* Decode one value and store it in `out`. Containers are stored into their
** parent slot as soon as they are created, so the whole tree stays reachable
** from the (pushed) root slot; no per-container Lua-stack anchors or
** per-value push/copy/pop are needed. `depth` is the remaining container
** allowance. */
static void mp_fill_value(lua_State *L, MPCursor *c, GCtab *amt, int depth,
			  TValue *out)
{
  uint8_t b = mp_rd8(L, c);
  MSize len;
  const char *p;
  if (b <= 0x7f || b >= 0xe0) {  /* fixint */
    mp_fill_signed(L, out, b <= 0x7f ? (int64_t)b : (int64_t)(int8_t)b);
  } else if (b >= MP_UINT8 && b <= MP_INT64) {
    switch (b) {
    case MP_UINT8: mp_fill_unsigned(L, out, mp_rd8(L, c)); break;
    case MP_UINT16: mp_fill_unsigned(L, out, mp_rd16(L, c)); break;
    case MP_UINT32: mp_fill_unsigned(L, out, mp_rd32(L, c)); break;
    case MP_UINT64: mp_fill_unsigned(L, out, mp_rd64(L, c)); break;
    case MP_INT8:
      mp_fill_signed(L, out, (int64_t)(int8_t)mp_rd8(L, c)); break;
    case MP_INT16:
      mp_fill_signed(L, out, (int64_t)(int16_t)mp_rd16(L, c)); break;
    case MP_INT32:
      mp_fill_signed(L, out, (int64_t)(int32_t)mp_rd32(L, c)); break;
    case MP_INT64: mp_fill_signed(L, out, (int64_t)mp_rd64(L, c)); break;
    default:
      mp_error(L, "ccmsgpack.decode: invalid marker 0x%02x at byte %d",
	       b, (int)(c->p - c->s) - 1);
    }
  } else if (b == MP_NIL) {
    mp_fill_null(L, out);  /* Same canonical value as .null. */
  } else if (b == MP_FALSE) {
    setboolV(out, 0);
  } else if (b == MP_TRUE) {
    setboolV(out, 1);
  } else if (b == MP_FLOAT32) {
    uint32_t bits = mp_rd32(L, c);
    float f;
    memcpy(&f, &bits, 4);
    setnumV(out, (lua_Number)f);
  } else if (b == MP_FLOAT64) {
    uint64_t bits = mp_rd64(L, c);
    double d;
    memcpy(&d, &bits, 8);
    setnumV(out, d);
  } else if (b >= 0xa0 && b <= 0xbf) {  /* fixstr */
    len = b & 0x1f;
    mp_rdskip(L, c, len);
    p = c->p - len;
    setstrV(L, out, lj_str_new(L, p, len));
  } else if (b == MP_STR8 || b == MP_STR16 || b == MP_STR32 ||
	     b == MP_BIN8 || b == MP_BIN16 || b == MP_BIN32) {
    if (b == MP_STR8 || b == MP_BIN8) len = mp_rd8(L, c);
    else if (b == MP_STR16 || b == MP_BIN16) len = (MSize)mp_rd16(L, c);
    else len = (MSize)mp_rd32(L, c);
    mp_rdskip(L, c, len);
    p = c->p - len;
    setstrV(L, out, lj_str_new(L, p, len));
  } else if ((b >= 0x90 && b <= 0x9f) || b == MP_ARRAY16 || b == MP_ARRAY32) {
    uint32_t n = mp_rd_count(L, c, b);
    uint32_t i;
    GCtab *t;
    MSize ahint;
    TValue *arr;
    if (depth <= 0)
      mp_error(L, "ccmsgpack.decode: container nesting exceeds depth limit");
    /* Pre-size the array part so every element can be decoded straight
    ** into its slot (slot i holds key i; slot 0 is unused). */
    ahint = (MSize)((uint64_t)n + 1 > ((uint64_t)1 << 20) ?
		    ((uint64_t)1 << 20) : (uint64_t)n + 1);
    t = lj_tab_new(L, ahint, 0);
    if (amt)
      setgcref(t->metatable, obj2gco(amt));  /* Table is white: no barrier. */
    lj_gc_anybarriert(L, t);
    settabV(L, out, t);  /* Reachable from here on (root slot is anchored). */
    arr = tvref(t->array);
    for (i = 1; i <= n; i++) {
      TValue *slot;
      if (i < ahint) {  /* Fast path: direct slot write. */
	      slot = &arr[i];
      } else {
	      slot = lj_tab_setint(L, t, (int32_t)i);
      }
      mp_fill_value(L, c, amt, depth - 1, slot);
    }
  } else if ((b >= 0x80 && b <= 0x8f) || b == MP_MAP16 || b == MP_MAP32) {
    uint32_t n = mp_rd_count(L, c, b);
    uint32_t i;
    GCtab *t;
    if (depth <= 0)
      mp_error(L, "ccmsgpack.decode: container nesting exceeds depth limit");
    t = lj_tab_new(L, 0, 0);
    lj_gc_anybarriert(L, t);
    settabV(L, out, t);
    for (i = 0; i < n; i++) {
      GCstr *ks = NULL;
      int32_t ik = 0;
      TValue *slot;
      if (mp_rd_key(L, c, &ks, &ik))
	slot = lj_tab_setint(L, t, ik);
      else
	slot = lj_tab_setstr(L, t, ks);
      mp_fill_value(L, c, amt, depth - 1, slot);
    }
  } else if (b >= MP_FIXEXT1 && b <= MP_FIXEXT16) {
    mp_error(L, "ccmsgpack.decode: unsupported extension value at byte %d",
	     (int)(c->p - c->s) - 1);
  } else if (b == MP_EXT8 || b == MP_EXT16 || b == MP_EXT32) {
    mp_error(L, "ccmsgpack.decode: unsupported extension value at byte %d",
	     (int)(c->p - c->s) - 1);
  } else {
    mp_error(L, "ccmsgpack.decode: invalid marker 0x%02x at byte %d",
	     b, (int)(c->p - c->s) - 1);
  }
}

/* -- Decoder entry ------------------------------------------------------- */

static int mp_decode(lua_State *L)
{
  GCstr *str;
  GCtab *amt;
  GCtab *opts = NULL;
  MPCursor c;
  int depth = MP_DEFAULT_DEPTH;
  if (!(L->base < L->top && tvisstr(L->base)))
    mp_argtype(L, 1, (L->base < L->top) ? L->base : NULL, "decode",
	       "string");
  str = strV(L->base);
  /* Optional 2nd argument: options table (depth). */
  if (L->base + 1 < L->top && !tvisnil(&L->base[1])) {
    if (!tvistab(&L->base[1]))
      mp_argtype(L, 2, &L->base[1], "decode", "table");
    opts = tabV(&L->base[1]);
  }
  depth = mp_opt_depth(L, opts, MP_DEFAULT_DEPTH);
  amt = mp_array_mt(L);
  lj_state_checkstack(L, depth * 4 + 64);
  c.s = strdata(str);
  c.p = c.s;
  c.w = c.s + str->len;
  {
    TValue *out = L->top++;  /* Result slot; rooted on the Lua stack. */
    mp_fill_value(L, &c, amt, depth, out);
  }
  if (c.p != c.w)
    mp_error(L, "ccmsgpack.decode: trailing data at byte %d",
	     (int)(c.p - c.s));
  return 1;
}

/* -- Module open --------------------------------------------------------- */

LUALIB_API int luaopen_ccmsgpack(lua_State *L)
{
  /* Create the module table (never assume stack index 1: require passes the
  ** module name as an argument). */
  lua_createtable(L, 0, 4);
  /* Canonical Lua.Array metatable, shared with ccjson (idempotent). */
  lua_pushliteral(L, "Array"); luaL_newmetatable(L, MP_ARRAY_MT); lua_rawset(L, -3);
  /* NULL lightuserdata; the same canonical value as ccjson.null. */
  lua_pushliteral(L, "null"); lua_pushlightuserdata(L, NULL); lua_rawset(L, -3);
  /* Functions. */
  lua_pushliteral(L, "encode"); lua_pushcfunction(L, mp_encode); lua_rawset(L, -3);
  lua_pushliteral(L, "decode"); lua_pushcfunction(L, mp_decode); lua_rawset(L, -3);
  return 1;
}
