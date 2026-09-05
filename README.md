# LuaJIT - clib extension in built-in core.

  本项目的代码 **不进入 LuaJIT 的 src/**, 独立于`LuaJIT`语言仅通过单一补丁注入集成.

```bash
ext/
  cc-codecs-core-plumbing.patch  # 单一 plumbing 补丁(仅改 LuaJIT 核心/构建文件)
  README.md                      # 本文档
  lib_ccjson.c                   # ccjson 模块(JSON,引擎 yyjson)
  lib_ccmsgpack.c                # ccmsgpack 模块(MessagePack,自研)
  yyjson.c  yyjson.h             # vendored yyjson(与 lib_ccjson.c 同目录,供引号包含)
  tests/
    test_json.lua                # ccjson 回归(独立可跑)
    test_msgpack.lua             # ccmsgpack 回归(对 ccjson 的共享断言为条件跳过,可独立跑)
    bench.lua                    # 双模块 + string.buffer 集成基准(需两模块都已注入)
```

## 快速开始

```bash
# 1. 使用git源码或zip解压
git clone https://github.com/LuaJIT/LuaJIT LuaJIT

# 2. 进入目录并将本项目克隆到`ext`文件夹下(或者改成你喜欢的命名, 下面一起改)
cd LuaJIT && git clone https://github.com/CandyMi/luajitext ext
```

```bash
# 1. 注入核心接线(5 个文件:Makefile/lib_init.c/lualib.h/msvcbuild.bat/ljamalg.c)
git apply ext/cc-codecs-core-plumbing.patch

# 2. 构建 + 测试
make                                    # Linux/MacOS/BSD/Posix 用这个命令编译
cd src && msvcbuild.bat                 # Windows 用 msvc 编译
src/luajit ext/tests/test_json.lua      # 验证 ccjson
src/luajit ext/tests/test_msgpack.lua   # 验证 ccmsgpack
src/luajit ext/tests/bench.lua          # 集成基准 与 压测

# 还原代码(调试可选)
git apply -R ext/cc-codecs-core-plumbing.patch && make clean && make
```

MSVC:`msvcbuild.bat`(补丁已在编译行显式列出 `..\ext\` 三个源文件,`lib_*.c` 通配只扫 src)。
amalg:`make amalg`(补丁已在 `src/ljamalg.c` 以 `../ext/...` 相对路径并入三份源码)。
yyjson.c 为 C99;若 cl 默认 C 模式报语法错,给该编译行加 `/std:c11`(两个模块自身是 C89 纪律,无需)。

## 补丁内容(5 hunks 集)

| 文件 | 改动 | 说明 |
| --- | --- | --- |
| `src/Makefile` | `LJCORE_O` 尾部 += `yyjson.o lib_ccjson.o lib_ccmsgpack.o`;对象规则区新增 ext 源 → src 对象的显式规则(`%.o: ../ext/%.c`,静态与 `_dyn.o` 双产物,`-I.` 解析 src 头) | 模块**不进** `LJLIB_O`/`LJLIB_C`:避免污染 buildvm 的扫描源列表(buildvm 只需 src 内 lib_*.c) |
| `src/lib_init.c` | `lj_lib_preload[]` += ccjson / ccmsgpack | `require(...)` 预加载,不设全局 |
| `src/lualib.h` | 声明两个 `luaopen_*` | 头文件可见性 |
| `src/msvcbuild.bat` | 两条编译行显式追加 `..\ext\yyjson.c ..\ext\lib_ccjson.c ..\ext\lib_ccmsgpack.c` | MSVC 通配够不到 ext |
| `src/ljamalg.c` | include 尾追加三份 `../ext/...c` | amalg 单文件构建 |

要点:
- 两模块均无 `LJLIB_*` 宏(纯 `luaopen_*`),buildvm 的 `ALL_LIB`/`LJLIB_C` 无需包含它们;
- 对象名沿用 `lib_ccjson.o` 等(**新模块 .c 文件名不得与 src/*.c 重名**,否则对象冲突);
- 引号包含:模块内 `#include "yyjson.h"` 由同目录解析,`#include "lj_obj.h"` 由 `-I.`(GNU,make 工作目录即 src)/`/I "."`(MSVC)解析;
- ccjson 与 ccmsgpack 共享注册表 `Lua.Array` 与 NULL 哨兵,运行期互相独立(require 顺序无关);
  模块测试里跨模块断言(pcall 探测)使单模块可独立全绿。

## 如何添加下一个模块 `foo`(重新生成单一补丁)

1. 源码放 `ext/lib_foo.c`(自带引擎则 `ext/eng.c`);需要 `lj_*` 内部符号就必须走本机制进核心;
2. 核心回到未注入态:`git apply -R ext/cc-codecs-core-plumbing.patch`;
3. 手工改 5 个核心文件(照现有行格式):Makefile `LJCORE_O` 尾 += `lib_foo.o`(及 `eng.o`)并在对象规则区补一条显式规则;`lib_init.c`/`lualib.h` 加登记与原型;`msvcbuild.bat` 两编译行加 `..\ext\lib_foo.c`;`ljamalg.c` include 尾加 `../ext/lib_foo.c`;
4. 生成补丁:对每个改动文件执行 `diff -u --label a/src/<f> --label b/src/<f> src/<f> <改后拷贝> >> ext/cc-codecs-core-plumbing.patch`(或直接在已应用工作区 `git diff > ext/cc-codecs-core-plumbing.patch` 后 `git apply -R` + `git apply` 往返验证);
5. 模块测试放 `ext/tests/test_foo.lua`,独立可跑,依赖其它模块的断言用 pcall 条件化。

## 注意

- **符号可见性**:`libluajit.so` 只导出公开 `lua*`/`luaL*`/`luaJIT*`,`lj_*` 全隐藏;
  需要 `lj_*`(cdata、`lj_str_*`、`lj_tab_*`)就必须编入核心,外置 .so 拿不到。
- 还原补丁后需重新 make(产物不随还原撤销);yyjson 上游更新时替换 ext/yyjson.{h,c} 并回归 test_json。
- 本补丁只做"接线"; ccjson 与 ccmsgpack 源码头部的注释/命名/错误文案刻意对齐, 改一侧记得同步另一侧。
