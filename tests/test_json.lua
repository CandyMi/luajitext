-- ccjson regression tests.
-- Run with:  src/luajit tests/test_json.lua
-- (requires the ccjson core plumbing patch to be applied to the build)

local ccjson = require("ccjson")
assert(_G.ccjson == nil, "ccjson must not be installed as a global")
assert(type(ccjson.encode) == "function")
assert(type(ccjson.decode) == "function")
assert(ccjson.Array ~= nil and type(ccjson.Array) == "table")
assert(ccjson.null ~= nil and type(ccjson.null) == "userdata")

local Array = ccjson.Array
local null = ccjson.null
local ffi_ok, ffi = pcall(require, "ffi")

local nchecks = 0
local function check(cond, msg)
  nchecks = nchecks + 1
  if not cond then error("FAIL: " .. tostring(msg), 2) end
end

local function ok(...)  -- no error raised
  local results = { pcall(...) }
  if not results[1] then
    error("FAIL (expected success): " .. tostring(results[2]), 2)
  end
  return select(2, unpack(results))
end

local function fails(...)  -- an error must be raised
  local results = { pcall(...) }
  if results[1] then
    error("FAIL (expected error): " .. tostring(results[2]), 2)
  end
  return results[2]
end

local function decerr(f, ...)  -- decode DATA errors return (nil, errmsg)
  local v, e = f(...)
  if v ~= nil or type(e) ~= "string" then
    error("FAIL (expected decode error, got " .. tostring(v) .. " / "
	  .. tostring(e) .. ")", 2)
  end
  return e
end

-- ---- Scalars (encode) ----
check(ccjson.encode("abc") == '"abc"', "string")
check(ccjson.encode("") == '""', "empty string")
check(ccjson.encode('a"b\\c') == '"a\\"b\\\\c"', "escape quote/backslash")
check(ccjson.encode("a\nb") == '"a\\nb"', "escape control")
check(ccjson.encode(true) == "true", "true")
check(ccjson.encode(false) == "false", "false")
check(ccjson.encode(null) == "null", "null constant")
check(fails(ccjson.encode, nil):match("nil"), "nil root rejected")

-- ---- Scalars (decode root restriction) ----
check(decerr(ccjson.decode, "1"):match("root"), "scalar root rejected")
check(decerr(ccjson.decode, "null"):match("root"), "null root rejected")
check(decerr(ccjson.decode, '"x"'):match("root"), "string root rejected")

-- ---- Numbers ----
check(ccjson.decode("[1]")[1] == 1, "int")
check(ccjson.decode("[2147483647]")[1] == 2147483647, "int32 max")
check(ccjson.decode("[2147483648]")[1] == 2147483648, "2^31")
check(ccjson.decode("[2.5]")[1] == 2.5, "real")
check(ccjson.decode("[1e10]")[1] == 1e10, "exponent")
check(ccjson.decode("[9007199254740992]")[1] == 9007199254740992, "2^53")
check(ccjson.decode("[-9007199254740992]")[1] == -9007199254740992, "-2^53")
check(ccjson.encode(1) == "1", "int text")
check(ccjson.encode(2.5) == "2.5", "real text")
check(ccjson.encode(1e10) == "10000000000" or ccjson.encode(1e10) == "1e+10",
      "1e10 text")
check(ccjson.encode(-0.0) == "0" or ccjson.encode(-0.0) == "-0",
      "-0 text")

if ffi_ok then
  -- Exact int64/uint64 construction must use cdata arithmetic, because
  -- plain Lua numeric literals beyond 2^53 are already rounded doubles.
  local big53 = ffi.new("int64_t", 2)^53   -- 9007199254740992LL, exact
  local big   = big53 + 1                  -- 9007199254740993LL, exact
  -- >2^53 must decode to int64/uint64 cdata
  local v = ccjson.decode("[9007199254740993]")[1]
  check(ffi.istype("int64_t", v), "big int64 cdata")
  check(v == big, "big int64 value")
  local u = ccjson.decode("[18446744073709551615]")[1]
  check(ffi.istype("uint64_t", u), "uint64 cdata")
  check(u == ffi.new("uint64_t", 0) - 1, "uint64 max value")
  check(ccjson.encode(u) == "18446744073709551615", "uint64 round trip")
  check(ccjson.encode(-big) == "-9007199254740993", "negative int64 text")
  check(ccjson.encode(ffi.new("int64_t", 42)) == "42", "small int64 text")
  check(ccjson.encode(big53 + 2) == "9007199254740994", "uint64-range text")
  check(ccjson.encode(big) == "9007199254740993", "int64 text round trip")
  check(ccjson.decode("[" .. ccjson.encode(big) .. "]")[1] == big,
        "int64 text -> cdata rt")
  -- Unknown cdata must encode as null
  check(ccjson.encode(ffi.new("double", 1.5)) == "null", "double cdata -> null")
  check(ccjson.encode(ffi.new("int", 7)) == "null", "int cdata -> null")
else
  print("# note: FFI disabled, cdata tests skipped")
end

-- ---- null / lightuserdata semantics ----
check(ccjson.decode("[null]")[1] == null, "array null identity")
check(ccjson.decode("[null]")[1] ~= nil, "array null not nil")
local o = ccjson.decode('{"a":null}')
check(o.a == null, "object null identity")
check(o.a ~= nil, "object null not nil (distinct from absent key)")
check(o.b == nil, "absent key stays nil")
check(ccjson.encode({ a = null }) == '{"a":null}', "null member text")
check(ccjson.encode({ a = nil }) == '{}', "nil member skipped")
check(ccjson.encode({}) == "{}", "empty plain table is object")

-- ---- Arrays ----
check(ccjson.encode({ 1, 2, 3 }) == "[1,2,3]", "dense table -> array")
check(ccjson.encode({ "a", 1, true, null }) == '["a",1,true,null]',
      "mixed array")
check(ccjson.encode({}) == "{}", "empty table default {}")
local a = ccjson.decode("[1,2,3]")
check(getmetatable(a) == Array, "decoded array carries Lua.Array mt")
check(a[1] == 1 and a[2] == 2 and a[3] == 3, "decoded array values")
local e = ccjson.decode("[]")
check(getmetatable(e) == Array and next(e) == nil, "[] decodes to empty Array")
check(ccjson.encode(e) == "[]", "[] round trips to []")

-- ---- Lua.Array marker tables ----
local m0 = setmetatable({}, Array)
check(ccjson.encode(m0) == "[]", "empty marker -> []")
local m1 = setmetatable({}, Array)
m1[1] = 1; m1[2] = 2; m1[3] = 3
check(ccjson.encode(m1) == "[1,2,3]", "marker array values")
local m2 = setmetatable({}, Array)  -- sparse: first nil ends the array
m2[1] = 1; m2[3] = 3
check(ccjson.encode(m2) == "[1]", "marker sparse truncates at first nil")
local m3 = setmetatable({}, Array)  -- stray keys ignored
m3[1] = 1; m3.extra = "x"
check(ccjson.encode(m3) == "[1]", "marker stray keys ignored")
check(getmetatable(m1) == Array, "user marker mt is the canonical one")

-- ---- Objects ----
check(ccjson.encode({ a = 1 }) == '{"a":1}', "single-key object")
local oo = ccjson.decode('{"a":1,"b":[2,3]}')
check(oo.a == 1 and oo.b[1] == 2 and oo.b[2] == 3, "nested object decode")
check(getmetatable(oo.b) == Array, "nested array carries mt")
check(ccjson.decode('{"1":2}')[1] == nil, "string key '1' not number 1")
check(ccjson.decode('{"1":2}')["1"] == 2, "string key '1' value")
check(ccjson.decode('{"a":1,"a":2}').a == 2, "duplicate keys last wins")
-- ---- R1 prefix-truncation rule ----
-- A table with a value at index 1 encodes as the contiguous prefix array;
-- everything after the first hole (higher integer keys AND string keys) is
-- deliberately dropped. A hole is a bug signal: truncation is visible.
check(ccjson.encode({ [1] = 1, [3] = 3 }) == "[1]",
      "sparse numeric table truncated at hole (R1)")
check(ccjson.encode({ 1, a = 2 }) == "[1]", "string key after prefix dropped (R1)")
check(ccjson.encode({ 1, 2, name = "x" }) == "[1,2]",
      "array+metadata truncated to prefix (R1, documented data loss)")
local sp = ccjson.decode(ccjson.encode({ [1] = 1, [3] = 3 }))
check(sp[1] == 1 and sp[2] == nil, "truncated result has no tail")
-- Without an element at index 1 the table is an object (lossless)
check(ccjson.encode({ [2] = 2 }) == '{"2":2}', "no index 1 -> object")
check(ccjson.encode({ a = 1 }) == '{"a":1}', "single-key object")
check(fails(ccjson.encode, { [true] = 1 }):match("key"), "bool key rejected")

-- ---- Unsupported values raise ----
check(fails(ccjson.encode, print):match("unsupported"), "function rejected")
check(fails(ccjson.encode, { f = print }):match("unsupported"),
      "function member rejected")
check(fails(ccjson.encode, coroutine.create(function() end)):match("unsupported"),
      "thread rejected")

-- ---- Depth ----
check(ccjson.decode("[1]", { depth = 1 })[1] == 1, "depth opts 1 ok")
check(decerr(ccjson.decode, "[[1]]", { depth = 1 }):match("depth"),
      "depth exceeded decode")
local deep = {}
local cur = deep
for i = 1, 20 do cur[1] = {}; cur = cur[1] end
check(ccjson.decode(ccjson.encode(deep, { depth = 32 }), { depth = 32 }) ~= nil,
      "depth opts 32 ok")
check(fails(ccjson.encode, deep):match("depth"), "default depth exceeded")
check(fails(ccjson.decode, "[1]", { depth = 0 }):match("depth"),
      "depth 0 rejected")
check(fails(ccjson.decode, "[1]", { depth = 2000 }):match("depth"),
      "depth > max rejected")
local cyc = {}
cyc.self = cyc
check(fails(ccjson.encode, cyc):match("depth"), "cycle caught by depth")

-- ---- Options ----
local pretty = ccjson.encode({ a = { b = 1 } }, { pretty = true })
check(pretty:find("\n") ~= nil, "pretty output")
check(ccjson.encode(deep, { depth = 200 }) ~= nil, "depth opt accepted")

-- utf8 = false (default): invalid byte string must fail on encode
local bad = "\255\254"
check(fails(ccjson.encode, bad):match("encode"), "invalid utf8 rejected")
-- utf8 = true: byte-level round trip
local rt = ccjson.encode(bad, { utf8 = true })
local back = ccjson.decode("[" .. rt .. "]", { utf8 = true })[1]
check(back == bad, "utf8=true byte round trip")
-- invalid utf8 input is now a returned decode error (not a raise)
check(decerr(ccjson.decode, '["\255"]'):match("decode"),
      "invalid utf8 input is a decode error")

-- ---- Syntax / error paths ----
check(decerr(ccjson.decode, ""):match("decode"), "empty input")
check(decerr(ccjson.decode, "[1] x"):match("decode"), "trailing content")
check(decerr(ccjson.decode, "[1,"):match("decode"), "truncated input")
check(decerr(ccjson.decode, "{a:1}"):match("decode"), "bare key rejected")
local err = decerr(ccjson.decode, "[] garbage")
check(err:match("at byte"), "error reports byte position")
check(ccjson.decode("  [1]  ")[1] == 1, "whitespace tolerated")
check(fails(ccjson.decode, 42):match("string"), "non-string arg rejected")
check(fails(ccjson.encode, 1, "x"):match("table"), "opts must be table")

-- ---- JSON5 / NaN ----
-- strict default: NaN/Infinity are NOT valid JSON
check(decerr(ccjson.decode, "[NaN]"):match("decode"), "NaN rejected by default")
check(fails(ccjson.encode, 0 / 0):match("nan"), "encode NaN errors by default")
check(fails(ccjson.encode, math.huge):match("nan"), "encode Inf errors by default")
-- decode opts.nan: bare Infinity/NaN allowance, rest stays strict
check(ccjson.decode("[Infinity]", { nan = true })[1] == math.huge, "nan +Inf")
check(ccjson.decode("[-Infinity]", { nan = true })[1] == -math.huge, "nan -Inf")
local nv = ccjson.decode("[NaN]", { nan = true })[1]
check(nv ~= nv, "nan NaN")
decerr(ccjson.decode, "{a:1}", { nan = true })  -- nan does not allow json5
-- decode opts.json5: comments/trailing/single-quote/unquoted/hex/NaN
local j5a = ccjson.decode("[1/*x*/, 2,]", { json5 = true })
check(j5a[1] == 1 and j5a[2] == 2, "json5 comments + trailing comma")
check(ccjson.decode("['ab']", { json5 = true })[1] == "ab", "json5 single quotes")
check(ccjson.decode("{a:1}", { json5 = true }).a == 1, "json5 unquoted keys")
check(ccjson.decode("[0x10, .5, +2]", { json5 = true })[1] == 16, "json5 hex")
check(ccjson.decode("[-0x10]", { json5 = true })[1] == -16, "json5 -hex")
local j5n = ccjson.decode('{"a":NaN,"b":Infinity}', { json5 = true })
check(j5n.a ~= j5n.a and j5n.b == math.huge, "json5 NaN/Infinity")
check(ccjson.decode("[1e999]", { json5 = true })[1] == math.huge,
      "json5 overflow -> Inf")
check(ccjson.decode("[1e999]", { nan = true })[1] == math.huge,
      "nan overflow -> Inf")
-- round trip NaN through json5/nan_literal
local rt_nan = ccjson.encode(
  ccjson.decode('{"a":NaN}', { json5 = true }), { nan_literal = true })
local rt_back = ccjson.decode(rt_nan, { nan = true })
check(rt_back.a ~= rt_back.a, "NaN round trip via nan_literal")
check(ccjson.encode({ a = 0 / 0 }, { nan_literal = true }) == '{"a":NaN}',
      "encode NaN literal")
check(ccjson.encode({ a = math.huge }, { nan_literal = true }) ==
      '{"a":Infinity}', "encode Inf literal")
check(ccjson.encode({ a = -math.huge }, { nan_literal = true }) ==
      '{"a":-Infinity}', "encode -Inf literal")
-- nan_null: standard-compliant downgrade, takes precedence over literal
check(ccjson.encode({ a = 0 / 0 }, { nan_null = true }) == '{"a":null}',
      "encode NaN -> null")
check(ccjson.encode({ a = math.huge }, { nan_null = true, nan_literal = true }) ==
      '{"a":null}', "nan_null overrides nan_literal")
local rt_null = ccjson.decode(ccjson.encode({ a = 0 / 0 }, { nan_null = true }))
check(rt_null.a == null, "NaN -> null decodes to null sentinel")

-- ---- Unicode ----
check(ccjson.decode('["\\u0041"]')[1] == "A", "unicode escape")
local smile = ccjson.decode('["\\uD83D\\uDE00"]')[1]
check(smile == "\240\159\152\128", "surrogate pair -> utf8")
local esc = ccjson.encode("中文")
check(ccjson.decode("[" .. esc .. "]")[1] == "中文", "non-ascii round trip")

-- ---- Round trips ----
local sample = {
  name = "ccjson",
  nums = { 1, -2, 3.5, 9007199254740992 },
  flags = { true, false, null },
  nested = { deep = { deeper = { deepest = "ok" } } },
  empty_obj = {},
  empty_arr = setmetatable({}, Array),
}
local txt = ccjson.encode(sample)
local back2 = ccjson.decode(txt)
check(back2.name == "ccjson", "rt name")
check(back2.nums[4] == 9007199254740992, "rt nums")
check(back2.flags[1] == true and back2.flags[2] == false, "rt flags")
check(back2.flags[3] == null, "rt null")
check(back2.nested.deep.deeper.deepest == "ok", "rt nested")
check(next(back2.empty_obj) == nil and getmetatable(back2.empty_obj) == nil,
      "rt empty obj plain")
check(getmetatable(back2.empty_arr) == Array, "rt empty arr marker")
-- Re-encoding a decoded object is byte-idempotent from the second
-- generation on (object key ORDER is not specified; the decoded table's
-- hash layout is deterministic for a given input).
local txt2 = ccjson.encode(back2)
check(ccjson.encode(ccjson.decode(txt2)) == txt2, "rt idempotent")

print(string.format("ok: %d checks passed", nchecks))
