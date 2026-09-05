-- bench_serialize.lua — ccjson / ccmsgpack / ccbson / string.buffer 序列化 encode/decode 对比
-- 运行: ./lula bench/bench_serialize.lua                  (默认 N=300000)
--       env BENCH_N=1000000 ./lula bench/bench_serialize.lua
-- 依赖: lula 宿主 — ccjson 经 FFI 绑定内置 yyjson (C); ccmsgpack 为纯 Lua
--       (其 encoder 内部即基于 string.buffer); ccbson 为 C 直写编码器 (src/bson.c);
--       string.buffer 为 LuaJIT 内置 (C)。
-- 计时: os.clock (CPU), 预热 20000 次 + guard 数组防 DCE, 同 bench_amqp_msg.lua 风格。
-- 负载说明: JSON 无 64 位整数, 故不含 cdata 数值 (0ULL-1 仅 msgpack/string.buffer 可编);
--          含 UTF-8 字符串 / 嵌套数组 / 浮点, 三种格式均支持。

local ccjson  = require "ccjson"
local msgpack = require "ccmsgpack"
local buffer  = require "string.buffer"

local N = tonumber(os.getenv("BENCH_N")) or 300000

-- 负载: 混合结构 (3 个 item, 每 item 6 字段 + 嵌套数组)
local payload = {
  items = {
    { a = 1,   b = 2,   name = '我是谁?', ok = true,  score = 3.14, tags = { 'x', 'y', 'z' } },
    { a = 11,  b = 22,  name = '我是谁?', ok = false, score = 1.5,  tags = { 'p', 'q', 'r' } },
    { a = 111, b = 222, name = '我是谁?', ok = true,  score = 9.9,  tags = { 'm', 'n', 'o' } },
    { a = 1,   b = 2,   name = '我是谁?', ok = true,  score = 3.14, tags = { 'x1', 'y', 'z' } },
    { a = 11,  b = 22,  name = '我是谁?', ok = false, score = 1.5,  tags = { 'p1', 'q', 'r' } },
    { a = 111, b = 222, name = '我是谁?', ok = true,  score = 9.9,  tags = { 'm1', 'n', 'o' } },
  },
  page = 7, total = 3,
}

-- 各格式预编码一份, 既作 decode 基准输入, 也作体积对比
local json_str = ccjson.encode(payload)
local mp_str   = msgpack.encode(payload)
local buf_str  = buffer.encode(payload)
-- local bson_str = assert(bson.encode(payload))

-- string.buffer + 字典压缩 (dict: 键名 → 索引, 同 main.lua 用法)
local bufd_opts = { dict = { 'a', 'b', 'name' }, metatable = {} }
local bufd_enc  = buffer.new(1024, bufd_opts)
local bufd_dec  = buffer.new(1024, bufd_opts)
local bufd_str  = bufd_enc:reset():encode(payload):get()

-- 自检: 三种格式往返必须无损, 否则基准无意义
local function check(name, obj)
  local ok = obj and obj.items and #obj.items == 6
    and obj.items[1].a == 1 and obj.items[1].name == '我是谁?'
    and obj.items[1].tags[3] == 'z'
  print(('check %-18s %s'):format(name, ok and 'PASS' or 'FAIL'))
  if not ok then os.exit(1) end
end
check('ccjson',        ccjson.decode(json_str))
check('ccmsgpack',      msgpack.decode(mp_str))
check('string.buffer', buffer.decode(buf_str))
-- check('ccbson',        bson.decode(bson_str))
check('string.buffer+dict', bufd_dec:set(bufd_str):decode())

local guard = { } -- 逃生出口: 阻止 JIT 折叠/消除分配
local function bench(fn, n)
  for i = 1, 20000 do fn() end -- 预热
  collectgarbage(); collectgarbage()
  local t0 = os.clock()
  local acc = 0
  for i = 1, n do
    local v = fn()
    if i % 997 == 0 then
      guard[i % 64] = v
      acc = acc + (type(v) == 'string' and #v or (type(v) == 'table' and 1 or 0))
    end
  end
  local dt = os.clock() - t0
  for i = 1, #guard do
    acc = acc + (type(guard[i]) == 'string' and #guard[i] or 1)
  end
  return dt, acc
end

local rows = {
  { 'ccjson (yyjson C)',    #json_str,
    function() return ccjson.encode(payload) end,
    function() return ccjson.decode(json_str) end },
  { 'ccmsgpack (C codec)',  #mp_str,
    function() return msgpack.encode(payload) end,
    function() return msgpack.decode(mp_str) end },
  { 'string.buffer (C)',    #buf_str,
    function() return buffer.encode(payload) end,
    function() return buffer.decode(buf_str) end },
  -- { 'ccbson (C writer)',    #bson_str,
  --   function() return bson.encode(payload) end,
  --   function() return bson.decode(bson_str) end },
  { 'string.buffer+dict',   #bufd_str,
    function() return bufd_enc:reset():encode(payload):get() end,
    function() return bufd_dec:set(bufd_str):decode() end },
}

print(('N=%d jit=%s'):format(N, jit.status() and 'on' or 'off'))
io.write(('%-22s %10s %10s %8s %10s %10s\n'):format(
  '格式', 'encode ns/op', 'decode ns/op', 'bytes', 'enc MB/s', 'dec MB/s'))
for _, r in ipairs(rows) do
  local te, ae = bench(r[3], N)
  local td, ad = bench(r[4], N)
  local eop, dop = te / N * 1e9, td / N * 1e9
  local emb, dmb = r[2] / te * N / 1e6, r[2] / td * N / 1e6
  io.write(('%-22s %10.1f %10.1f %8d %10.1f %10.1f\n'):format(
    r[1], eop, dop, r[2], emb, dmb))
end

-- 必须 os.exit: 脚本跑完后主协程结束, 引导循环 while true 会空转挂起 (同 bench_xml.lua)
os.exit(0)
