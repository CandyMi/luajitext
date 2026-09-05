-- ccmsgpack regression tests.
-- Run with:  src/luajit tests/test_msgpack.lua
-- (requires patches 0001+0002 applied to the build)

local mp = require("ccmsgpack")
assert(_G.ccmsgpack == nil, "ccmsgpack must not be a global")
assert(type(mp.encode) == "function" and type(mp.decode) == "function")
local cc = require("ccjson")  -- for cross-module identity checks
assert(mp.Array == cc.Array, "shared Lua.Array metatable with ccjson")
assert(mp.null == cc.null, "shared null sentinel with ccjson")

local Array = mp.Array
local null = mp.null
local ffi_ok, ffi = pcall(require, "ffi")

local nchecks = 0
local function check(cond, msg)
  nchecks = nchecks + 1
  if not cond then error("FAIL: " .. tostring(msg), 2) end
end

local function hex(s)
  local t = {}
  for i = 1, #s do t[i] = string.format("%02x", string.byte(s, i)) end
  return table.concat(t, " ")
end

-- ---- Encoded byte-exactness (golden vectors) ----
check(hex(mp.encode(0)) == "00", "uint 0")
check(hex(mp.encode(127)) == "7f", "uint 127")
check(hex(mp.encode(128)) == "cc 80", "uint 128 -> u8")
check(hex(mp.encode(255)) == "cc ff", "uint 255 -> u8")
check(hex(mp.encode(256)) == "cd 01 00", "uint 256 -> u16")
check(hex(mp.encode(65536)) == "ce 00 01 00 00", "uint 65536 -> u32")
check(hex(mp.encode(-1)) == "ff", "int -1")
check(hex(mp.encode(-32)) == "e0", "int -32")
check(hex(mp.encode(-33)) == "d0 df", "int -33 -> i8")
check(hex(mp.encode(-129)) == "d1 ff 7f", "int -129 -> i16")
check(hex(mp.encode(-32769)) == "d2 ff ff 7f ff", "int -> i32")
check(hex(mp.encode(2^31)) == "ce 80 00 00 00", "2^31 -> u32")
check(hex(mp.encode(true)) == "c3", "true")
check(hex(mp.encode(false)) == "c2", "false")
check(hex(mp.encode(null)) == "c0", "null sentinel -> nil")
check(hex(mp.encode("")) == "a0", "empty str -> fixstr 0")
check(hex(mp.encode("a")) == "a1 61", "fixstr")
check(hex(mp.encode(string.rep("x", 31))) == "bf" .. string.rep(" 78", 31),
      "fixstr 31")
check(hex(mp.encode(string.rep("x", 32))):match("^d9 20"), "str8")
check(hex(mp.encode(string.rep("x", 256))):match("^da 01 00"), "str16")
check(hex(mp.encode(1.5)) == "cb 3f f8 00 00 00 00 00 00", "float64 1.5")
check(hex(mp.encode({})) == "80", "empty table -> fixmap 0")
check(hex(mp.encode(setmetatable({}, Array))) == "90", "empty marker -> fixarray 0")
check(hex(mp.encode({ 1, 2, 3 })) == "93 01 02 03", "array 3")
check(hex(mp.encode({ a = 1 })) == "81 a1 61 01", "map 1")
check(hex(mp.encode("abc")) == "a3 61 62 63", "string -> str marker")

-- ---- decode of golden values (raw markers, decimal escapes) ----
check(mp.decode("\0") == 0, "dec 0")
check(mp.decode("\127") == 127, "dec 127")
check(mp.decode("\255") == -1, "dec -1")
check(mp.decode(mp.encode(2^31)) == 2^31, "dec 2^31")
check(mp.decode(mp.encode(true)) == true, "dec true")
check(mp.decode("\192") == null and mp.decode("\192") ~= nil, "dec nil -> null")
check(mp.decode(mp.encode("héllo")) == "héllo", "dec str round trip")
check(mp.decode(mp.encode(2.5)) == 2.5, "dec f64 round trip")
local f32 = mp.decode("\202\064\073\015\219")
check(math.abs(f32 - 3.14) < 1e-2, "dec float32 3.14")
check(mp.decode("\161x") == "x", "dec fixstr")
check(mp.decode("\162ab") == "ab", "dec fixstr 2")

-- scalar roots are allowed (unlike ccjson)
check(mp.decode("\42") == 42, "scalar root int")
check(mp.decode("\162ab") == "ab", "scalar root string")
check(mp.decode("\192") == null, "nil root -> null")

-- ---- numbers & cdata ----
check(mp.decode(mp.encode(9007199254740992)) == 9007199254740992, "2^53")
if ffi_ok then
  local big53 = ffi.new("int64_t", 2)^53
  local big = big53 + 1  -- 9007199254740993, exact
  local v = mp.decode(mp.encode(big))
  check(ffi.istype("int64_t", v) and v == big, "int64 cdata round trip")
  local umax = ffi.new("uint64_t", 0) - 1
  local u = mp.decode(mp.encode(umax))
  check(ffi.istype("uint64_t", u) and u == umax, "uint64 max round trip")
  check(ffi.istype("int64_t",
		   mp.decode("\211\016\0\0\0\0\0\0\0")), "dec raw int64 2^60")
  check(mp.decode("\207\255\255\255\255\255\255\255\255") == umax,
	"dec raw uint64")
  check(hex(mp.encode(umax)) == "cf ff ff ff ff ff ff ff ff", "uint64 golden")
else
  print("# note: FFI disabled, cdata tests skipped")
end

-- ---- containers: R1 prefix rule (same as ccjson) ----
check(hex(mp.encode({ [1] = 1, [3] = 3 })) == "91 01",
      "sparse truncates at hole (R1)")
check(hex(mp.encode({ 1, name = "x" })) == "91 01",
      "string key after prefix dropped (R1)")
check(hex(mp.encode({ [2] = 2 })) == "81 02 02", "no index 1 -> map w/ int key")
-- map: fixmap 3 {3:1, "a":1, "bc":null}
local m = mp.decode("\131\3\1\161a\1\162bc\192")
check(m[3] == 1 and m.a == 1 and m.bc == null, "map str+int keys, nil value")
local arr = mp.decode("\147\1\2\3")
check(getmetatable(arr) == Array and arr[1] == 1 and arr[3] == 3,
      "dec array -> Array mt")
local e = mp.decode("\144")  -- empty array
check(getmetatable(e) == Array and next(e) == nil, "array(0) -> empty marker")
check(hex(mp.encode(e)) == "90", "empty marker array round trips")
local em = mp.decode("\128")  -- empty map
check(next(em) == nil and getmetatable(em) == nil, "map(0) -> plain empty")
check(hex(mp.encode(em)) == "80", "plain empty round trips as map")
-- nil elements keep positions via the sentinel
local withnil = mp.decode("\147\1\192\3")
check(withnil[1] == 1 and withnil[2] == null and withnil[3] == 3,
      "array nil sentinel")
check(hex(mp.encode(withnil)) == "93 01 c0 03", "sentinel array round trips")
-- bin decodes as string
check(mp.decode("\196\3abc") == "abc", "bin8 -> string")

-- ---- unsupported / errors ----
local r = { pcall(mp.decode, "\199\0\0") }
check(not r[1] and r[2]:match("extension"), "ext8 rejected")
r = { pcall(mp.decode, "\146\1") }
check(not r[1] and r[2]:match("truncated"), "truncated input error")
r = { pcall(mp.decode, "\1\2") }
check(not r[1] and r[2]:match("trailing"), "trailing data error")
r = { pcall(mp.decode, "\220\255\255") }
check(not r[1] and r[2]:match("truncated"), "declared huge array rejected")
r = { pcall(mp.decode, "") }
check(not r[1] and r[2]:match("truncated"), "empty input error")
r = { pcall(mp.decode, 42) }
check(not r[1] and r[2]:match("string"), "non-string arg rejected")
r = { pcall(mp.decode, "\147\1\2\3", { depth = 0 }) }
check(not r[1] and r[2]:match("depth"), "depth 0 rejected")
check(mp.decode("\145\1", { depth = 2 })[1] == 1, "depth opts ok")
r = { pcall(mp.encode, { [true] = 3 }) }
check(not r[1] and r[2]:match("key"), "bool map key rejected")
r = { pcall(mp.encode, { a = 1, [true] = 3 }) }
check(not r[1] and r[2]:match("key"), "bool map key rejected in map")
r = { pcall(mp.encode, print) }
check(not r[1] and r[2]:match("unsupported"), "function rejected")
r = { pcall(mp.encode, { f = print }) }
check(not r[1] and r[2]:match("unsupported"), "function member rejected")
r = { pcall(mp.encode, 1, "x") }
check(not r[1] and r[2]:match("table"), "opts must be table")

-- ---- depth (msgpack containers have no end markers: a1(a1(a1(1)))) ----
local nested = "\145\145\145\1"
check(mp.decode(nested, { depth = 3 }) ~= nil, "depth 3 ok")
r = { pcall(mp.decode, nested, { depth = 2 }) }
check(not r[1] and r[2]:match("depth"), "depth exceeded during parse")
local deep = {}
local cur = deep
for i = 1, 20 do cur[1] = {}; cur = cur[1] end
check(mp.decode(mp.encode(deep, { depth = 32 }), { depth = 32 }) ~= nil,
      "opts depth 32")
r = { pcall(mp.encode, deep) }
check(not r[1] and r[2]:match("depth"), "default depth exceeded encode")
local cyc = {}
cyc.self = cyc
r = { pcall(mp.encode, cyc) }
check(not r[1] and r[2]:match("depth"), "cycle caught by depth")

-- ---- NaN / Inf (msgpack supports them) ----
local nan = mp.decode(mp.encode(0 / 0))
check(nan ~= nan, "NaN round trip")
local inf = mp.decode(mp.encode(1 / 0))
check(inf == math.huge, "Inf round trip")

-- ---- cross-module: ccjson arrays share the marker ----
local cj = cc.decode('[1,2,3]')
check(getmetatable(cj) == mp.Array, "ccjson arrays share marker")
check(hex(mp.encode(cj)) == "93 01 02 03", "encode ccjson-decoded array")

-- ---- round trips ----
local sample = {
  name = "ccmsgpack",
  nums = { 1, -2, 3.5, 2^40 },
  flags = { true, false, null },
  nested = { deep = { deeper = "ok" } },
  empty_map = {},
  empty_arr = setmetatable({}, Array),
}
local bin = mp.encode(sample)
local back = mp.decode(bin)
check(back.name == "ccmsgpack", "rt name")
check(back.nums[1] == 1 and back.nums[2] == -2 and back.nums[3] == 3.5,
      "rt nums")
check(back.nums[4] == 2^40, "rt 2^40")
check(back.flags[1] == true and back.flags[3] == null, "rt flags+null")
check(back.nested.deep.deeper == "ok", "rt nested")
check(next(back.empty_map) == nil and getmetatable(back.empty_map) == nil,
      "rt empty map")
check(getmetatable(back.empty_arr) == Array, "rt empty arr marker")
-- Object key ORDER is not a contract (hash-layout dependent); what must
-- hold is that repeated decode/re-encode is content-stable.
local function deep_equal(a, b)
  if a == b then return true end
  if type(a) ~= "table" or type(b) ~= "table" then return false end
  if getmetatable(a) ~= getmetatable(b) then return false end
  for k, v in pairs(a) do
    if b[k] == nil and v ~= nil then return false end
    if not deep_equal(v, b[k]) then return false end
  end
  for k in pairs(b) do
    if a[k] == nil then return false end
  end
  return true
end
local bin2 = mp.encode(back)
check(deep_equal(mp.decode(bin2), mp.decode(mp.encode(mp.decode(bin2)))),
      "rt content-stable")
check(cc.decode(cc.encode({ name = back.name, n = back.nums[1] })).n == 1,
      "cross-codec sanity")

print(string.format("ok: %d checks passed", nchecks))
