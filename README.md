# LuaJIT - clib extension in built-in core.

  本项目的代码 **不进入 LuaJIT 的 src/**, 独立于`LuaJIT`语言仅通过单一补丁注入集成.

```bash
ext/
  cc-codecs-core-plumbing.patch  # 单一 plumbing 补丁(仅改 LuaJIT 核心/构建文件)
  README.md                      # 本文档
  .github/workflows/ci.yml       # CI: Linux(make/amalg) + Windows(MSVC, cmd 与 pwsh 双 shell)
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
git apply --ignore-whitespace ext/cc-codecs-core-plumbing.patch

# 2. 构建 + 测试
make                                    # Linux/MacOS/BSD/Posix 用这个命令编译
cd src && msvcbuild.bat                 # Windows 用 msvc 编译
src/luajit ext/tests/test_json.lua      # 验证 ccjson
src/luajit ext/tests/test_msgpack.lua   # 验证 ccmsgpack
src/luajit ext/tests/bench.lua          # 集成基准 与 压测

# 还原代码(调试可选)
git apply -R --ignore-whitespace ext/cc-codecs-core-plumbing.patch && make clean && make
```

MSVC:`msvcbuild.bat`(补丁已在编译行显式列出 `..\ext\` 三个源文件并带 `/I.`,`lib_*.c` 通配只扫 src;
链接/打包行显式追加 `yyjson.obj`, 因为它不匹配 `lj_*.obj lib_*.obj`;
脚本开头 `@cd /d "%~dp0"` 先切到 `src/`, 所以从 LuaJIT 根或其它目录调用也能解析 `..\ext\`/`..\dynasm`)。
amalg:`make amalg`(补丁已在 `src/ljamalg.c` 以 `../ext/...` 相对路径并入三份源码,
并给 `ljamalg.o` 规则补上 `-I.`, 否则 ext 源里的 `lua.h`/`lj_*.h` 无从解析)。
yyjson.c 为 C99;若 cl 默认 C 模式报语法错,给该编译行加 `/std:c11`(两个模块自身是 C89 纪律,无需)。

## 补丁内容(5 个文件)

| 文件 | 改动 | 说明 |
| --- | --- | --- |
| `src/Makefile` | `LJCORE_O` 尾部 += `yyjson.o lib_ccjson.o lib_ccmsgpack.o`;对象规则区新增 ext 源 → src 对象的显式规则(`%.o: ../ext/%.c`,静态与 `_dyn.o` 双产物,`-I.` 解析 src 头);另给 `ljamalg.o` 一条带 `-I.` 的规则(amalg 时同上) | 模块**不进** `LJLIB_O`/`LJLIB_C`:避免污染 buildvm 的扫描源列表(buildvm 只需 src 内 lib_*.c) |
| `src/lib_init.c` | `lj_lib_preload[]` += ccjson / ccmsgpack | `require(...)` 预加载,不设全局 |
| `src/lualib.h` | 声明两个 `luaopen_*` | 头文件可见性 |
| `src/msvcbuild.bat` | 开头 `@cd /d "%~dp0"` 把工作目录固定到 `src/`;两条编译行显式追加 `..\ext\yyjson.c ..\ext\lib_ccjson.c ..\ext\lib_ccmsgpack.c` 并加 `/I.`;两条链接/打包行追加 `yyjson.obj`;amalg 两条编译行加 `/I.` | MSVC 通配够不到 ext,`yyjson.obj` 也不匹配 `lj_*.obj lib_*.obj`;`#include "lua.h"`/`"lj_*.h"` 由 cl 的 `/I.`(batch 在 `src/` 下执行)解析,不会搜源文件所在的 `..\ext\` |
| `src/ljamalg.c` | include 尾追加三份 `../ext/...c` | amalg 单文件构建 |

要点:
- 两模块均无 `LJLIB_*` 宏(纯 `luaopen_*`),buildvm 的 `ALL_LIB`/`LJLIB_C` 无需包含它们;
- 对象名沿用 `lib_ccjson.o` 等(**新模块 .c 文件名不得与 src/*.c 重名**,否则对象冲突);
- 引号包含:模块内 `#include "yyjson.h"` 由同目录解析,`#include "lj_obj.h"` 由 `-I.`(GNU,make 工作目录即 src)/`/I "."`(MSVC)解析;
- ccjson 与 ccmsgpack 共享注册表 `Lua.Array` 与 NULL 哨兵,运行期互相独立(require 顺序无关);
  模块测试里跨模块断言(pcall 探测)使单模块可独立全绿。

## CI

`.github/workflows/ci.yml` 用三个 job 验证补丁在两个平台、三种 shell 下都能落地:

| Job | Shell | 构建 | 测试 |
| --- | --- | --- | --- |
| `Linux / GNU make` | bash | `make`;再 `make clean && make amalg` | test_json + test_msgpack(两轮) |
| `Windows / cmd` | cmd | `src\msvcbuild.bat` | test_json + test_msgpack |
| `Windows / pwsh` | pwsh | `cmd /c msvcbuild.bat` | test_json + test_msgpack |

流程(push / PR / 手动触发):
1. `actions/checkout` 拉 LuaJIT(`v2.1`;手动触发时可用 `luajit-ref` 输入改成别的分支/标签);
2. 再把本仓库 checkout 成 `luajit/ext`(工作区根不是 LuaJIT, 所以用两次 checkout, 不在 shell 里搬目录);
3. 在 LuaJIT 根目录 `git apply --ignore-whitespace ext/cc-codecs-core-plumbing.patch`;
4. 构建 + 跑回归, 任一步退出码非 0 即该 job 红.

Windows 细节:
- 先 `git config --global core.autocrlf false`:补丁里 `msvcbuild.bat` 的 hunk 是 CRLF, 其余是 LF, 需要逐字节 checkout;仓库根 `.gitattributes` 用 `-text` 保护补丁本身在提交/checkout 时不被规范化;
- `ilammy/msvc-dev-cmd`(arch x64)提供 `cl`/`link`/`lib` 与 `INCLUDE`/`LIB`;
- cmd 用 `call msvcbuild.bat`, pwsh 用 `cmd /c msvcbuild.bat`, 两者都显式检查退出码(批处理失败不会自动让 PowerShell 步骤变红);
- 测试用 `src\luajit.exe`(pwsh 里需要 `.\` 前缀).

## 如何添加下一个模块 `foo`(重新生成单一补丁)

1. 源码放 `ext/lib_foo.c`(自带引擎则 `ext/eng.c`);需要 `lj_*` 内部符号就必须走本机制进核心;
2. 核心回到未注入态:`git apply -R --ignore-whitespace ext/cc-codecs-core-plumbing.patch`;
3. 手工改 5 个核心文件(照现有行格式):Makefile `LJCORE_O` 尾 += `lib_foo.o`(及 `eng.o`)并在对象规则区补一条显式规则;`lib_init.c`/`lualib.h` 加登记与原型;`msvcbuild.bat` 两编译行加 `..\ext\lib_foo.c`、链接/打包行按需加 `.obj`;`ljamalg.c` include 尾加 `../ext/lib_foo.c`;
4. 生成补丁:对每个改动文件执行 `diff -u --label a/src/<f> --label b/src/<f> src/<f> <改后拷贝> >> ext/cc-codecs-core-plumbing.patch`(或直接在已应用工作区 `git diff > ext/cc-codecs-core-plumbing.patch` 后 `git apply -R` + `git apply` 往返验证);
5. 模块测试放 `ext/tests/test_foo.lua`,独立可跑,依赖其它模块的断言用 pcall 条件化。

## 注意

- **符号可见性**:`libluajit.so` 只导出公开 `lua*`/`luaL*`/`luaJIT*`,`lj_*` 全隐藏;
  需要 `lj_*`(cdata、`lj_str_*`、`lj_tab_*`)就必须编入核心,外置 .so 拿不到。
- 还原补丁后需重新 make(产物不随还原撤销);yyjson 上游更新时替换 ext/yyjson.{h,c} 并回归 test_json。
- 补丁行尾是混合的(`msvcbuild.bat` 段 CRLF, 其余 LF),与 LuaJIT 源文件逐字节对应;`.gitattributes` 用 `-text` 保护它,应用时带 `--ignore-whitespace` 可兼容各平台的 `core.autocrlf`。
- 本补丁只做"接线"; ccjson 与 ccmsgpack 源码头部的注释/命名/错误文案刻意对齐, 改一侧记得同步另一侧。
