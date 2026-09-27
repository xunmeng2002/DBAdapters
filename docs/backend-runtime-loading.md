# 数据库后端的运行时按配置装载

[`LoadDatabaseBackend`](../include/DbAdapters/BackendLoader/DbBackendLoader.h) 让某个数据库后端
「按配置引用、可以不随发布包发运」。本文件说明它的由来、契约、查找次序与已知边界。

## 一、为什么需要它

四个适配器过去都是被链接进来的，于是它们连同各自的客户端库在**进程加载那一刻**就全部进了
地址空间 —— 即便当前配置只走其中一个。回测与三个服务默认都只走 SQLite（`DbType = "1"`），
SQLite 约 1.1 MB + DuckDB 约 29 MB 是真正用得到的，而 MySQL 与 MariaDB 两系约 14 MB 是白装的。

Windows 上这一层能用 `/DELAYLOAD` 省掉，**ELF 没有对应物**：

| 手段 | 能否延迟库加载 |
| ---- | ---- |
| `DT_NEEDED` | 否，是加载期决议，无开关 |
| `-z lazy` / `RTLD_LAZY` | 否，只延迟符号绑定 |
| `--as-needed` | 否，只丢「无符号引用」的库 |
| 运行时 `dlopen` | **是，唯一一条路** |

于是把机制下沉到本库，两个平台共用一套，调用方一个 `#ifdef` 都不需要。

## 二、契约：一行 C 入口

运行时装载只能靠 `dlsym` / `GetProcAddress` 取符号，而它们只认 C 符号名 —— C++ 构造函数的
mangled 名跨编译器与 stdlib 版本都不可靠。故每个适配器各导出一个签名**完全相同**的 C 函数：

```cpp
extern "C" MYSQLWRAPPER_EXPORTS DbAdapters::Db* DbAdapters_CreateBackend(const char* connectionTarget,
    const char* /*userName*/, const char* /*password*/, char* failureText, int failureTextCapacity);
```

符号名 `DbBackendCreateSymbolName` 与签名类型 `BackendCreateFunction` 定义在
[`DbBackendFactory.h`](../include/DbAdapters/DbInterface/DbBackendFactory.h)。

四个模块同名同签名**不冲突**：它们是各自独立的模块，符号按模块句柄去取。

签名有三处取舍：

- **三个连接参数**（而非每后端一份签名）覆盖了四个构造函数入参的并集：DuckDB / SQLite / MySQL
  只用得上第一个，MariaDB 三个都要，其余各取所需。共用签名是装载器保持通用的前提。
- **错误缓冲由调用方持有**，工厂只往里写 —— 不让任何分配跨越模块边界。
- **参数一律 `const char*`**：`Db` 的 15 个纯虚函数里没有一个 STL 类型，这条边界才能保持干净，
  不会踩上「两侧 STL 版本或调试级别不一致」那类跨模块传对象的坑。

### 异常绝不跨 C ABI

`MysqlWrapper` 与 `MariadbWrapper` 的构造函数在连不上时是 **`throw`** 的。让 C++ 异常穿过
`extern "C"` 边界是**未定义行为**。故工厂一律经 `CreateBackendOrReportFailure` 收住异常，
把 `what()` 写进调用方的缓冲并返回 `nullptr`；调用方据此报一条可读的错，而不是看进程当场终止。
这是必须的，不是风格问题。

`test/TestDB` 的 `TestBackendLoader` 就是这条约束的可执行证据。

## 三、查找次序

固定两步，先「调用方模块所在目录 + 文件名」，再裸文件名。

| 次序 | Windows | Linux |
| ---- | ---- | ---- |
| 1 | `<调用方模块目录>\<Base>.dll` | `<调用方模块目录>/lib<Base>.so` |
| 2 | 裸文件名（标准搜索序） | 裸文件名（**调用方 `RUNPATH`**） |

`<Base>` 是模块基名（`DuckdbWrapper` / `SqliteWrapper` / `MysqlWrapper` / `MariadbWrapper`），
由装载器按 `DbTypeType` 查表得出；平台前后缀与 Debug 后缀的拼装同样在装载器内，
调用方既不指认基名、也不参与拼装。

- **Windows 第一步不可省**：显式 `LoadLibrary` 的标准搜索序里**不含调用方模块自己的目录**。
  宿主是 `python.exe` 时，进程的「应用程序目录」是解释器目录而非引擎根，引擎根根本不在搜索
  范围内。实测：少了这一步会以 `0xC06D007E` 终止进程。
- **Linux 第一步通常落空**（`libBackTest.so` 在 `lib/Release`，适配器在别处），第二步裸名
  `dlopen` 走的是**调用方 `RUNPATH`**；规则与过去 `DT_NEEDED` 逐字相同，故失败模式不变。
  将来打包成 `$ORIGIN` 自包含时第一步即命中。

**两次尝试的原因都要留在报错文案里**，不能只报最后一次。Linux 上尤其要紧：模块明明就在眼前、
只是它依赖的某个库找不到时，前一次会点出缺的是哪个库，而后一次只会说「这个模块找不到」。

推论：适配器摆在**引擎目录**与摆在 **`RUNPATH` 覆盖的目录**两条路都成立，无需改代码。

## 四、装载器住在哪：静态库，不进接口层

装载器**不是** header-only，它有一个 `.cpp`：

```text
include/DbAdapters/BackendLoader/DbBackendLoader.h 公开头：2 个函数声明（按种类 / 按基名）
src/DbAdapters/BackendLoader/DbBackendLoader.cpp   实现：平台头、#ifdef、模块基名表与调试后缀全在这里
```

后端种类用 **Spark 的 `DbTypeType`**（`Spark/Types.h`，全局作用域：`DuckDb=0` / `SqliteDb=1` /
`MysqlDb=2` / `MariaDb=3`），本库**不自备枚举** —— 引擎配置里 `DbType` 那些字面量正是照它生成
的，本库再来一套就是同一件事的第三种写法。该头的包含路径由 `BackendLoaderStatic` 私有链接
`Spark::Core` 取得；消费方本就是 Spark 系工程，各自链接 `Spark::Core`。

若做成 header-only，任何 `#include` 它的编译单元都会被拖进 `<windows.h>` 与 `NOMINMAX` /
`WIN32_LEAN_AND_MEAN` 两道宏 —— 那是实现细节，不该由使用者承担。收进 `.cpp` 之后，公开头里
没有宏、没有平台头、没有 `#ifdef`。**唯一的编译定义加在目标上**（`PUBLIC`，随导出集传给
消费方）：它携带 Debug 后缀的值，见第五节。

构建成**静态库**（CMake 目标 `DbAdapters::BackendLoaderStatic`）而不是共享库，是被下面第五节
那条「取本函数地址反查所属模块」定死的：

- **静态库**：那段代码被链进 `BackTest.dll` / `libBackTest.so`，函数地址就落在引擎模块里，
  反查到的正是引擎目录，一等候选路径成立；
- **共享库**：地址落在装载器自己身上，反查到的就是装载器所在目录 —— Windows 上第一步必然
  落空，而兜底的裸名查找又不含引擎目录，直接回归。

代价是每个链接它的模块各持一份实现。这正是想要的：每一份各自反查到自己所在的模块目录。

## 五、平台差异一共三处

全部收在 `DbBackendLoader.cpp` 内，调用方看不见：

| 差异 | Windows | Linux |
| ---- | ---- | ---- |
| 模块文件名 | `<Base>.dll` | `lib<Base>.so` |
| 本模块所在目录 | `GetModuleHandleExA` + `GetModuleFileNameA` | `dladdr` 的 `dli_fname` |
| 装载与取符号 | `LoadLibraryExA` / `GetProcAddress` | `dlopen` / `dlsym` |

**Debug 后缀不算平台差异，但它同样收在装载器内**：`CMAKE_DEBUG_POSTFIX "d"` 是本库自己设的，
故后缀值经 `BACKENDLOADER_FILENAME_DEBUG_POSTFIX` 编译定义（生成器表达式 `$<$<CONFIG:Debug>:d>`）
注入 `BackendLoaderStatic`，与平台前后缀一同在 `DatabaseBackendModuleFilename()` 里拼。缺了这个
定义就编译不过，而不是在 Debug 构装里静默装到 Release 那份同名模块 —— 那种失效正是把后缀从
消费方收回来要消灭的东西。生成器表达式而非裸变量：单配置生成器（Ninja）下裸变量会让
Debug 与 Release 取到同一个值。

三处实现细节：

- 取**本文件内某函数的地址**再反查所属模块，是为了拿到 `BackTest.dll` / `libBackTest.so` 的
  目录，而不是宿主（`python.exe`）的目录。
- `dlopen` 用 `RTLD_NOW` 而非 `RTLD_LAZY`：要的就是「缺依赖在装载这一刻就露头」，与 Windows 上
  `LoadLibrary` 急切解析导入表的行为对齐。`RTLD_LOCAL` 则不把后端符号倒进全局名字空间。
  Windows 侧 `LoadLibraryExA` 配 `LOAD_WITH_ALTERED_SEARCH_PATH`，让系统改用目标模块自己的
  目录去找它连带拉起的依赖。
- `dlerror()` 是**粘的**，每次尝试前先清一次，否则读到的可能是上一步留下的旧错。

## 六、已知边界

三条都属刻意，不是缺陷：

1. **装载进来的模块永不卸载**（`FreeLibrary` / `dlclose` 一概不调）。后端对象的虚表与驱动
   内部的静态状态要活到进程结束，卸载只会换来之后某个时刻的崩溃。每次调用至多装载一两次，
   不构成需要回收的量。
2. **不做装载缓存**。缓存要引入一把锁与一处可变共享状态，而四个入口至多各调一两次，收益为零。
3. **路径按窄字符处理**（Windows 上是 A 系列 API）。中文路径下走系统 ANSI 代码页往返，
   同机同页时仍成立，但不再向下兼容。

## 七、调用方怎么用

以 QuantTrading 为例，它只保留「配置整数 → `DbTypeType`」这一段校验，
见 `src/QuantTradingCommon/DatabaseAdapterFactory.h`：

| `DbType` | 后端种类 | 取法 |
| ---- | ---- | ---- |
| `0` DuckDB | `DbTypeType::DuckDb` | `LoadDatabaseBackend(DbTypeType::DuckDb, …)` |
| `1` SQLite（缺省） | `DbTypeType::SqliteDb` | `LoadDatabaseBackend(DbTypeType::SqliteDb, …)` |
| `2` MySQL | `DbTypeType::MysqlDb` | `LoadDatabaseBackend(DbTypeType::MysqlDb, …)` |
| `3` MariaDB | `DbTypeType::MariaDb` | `LoadDatabaseBackend(DbTypeType::MariaDb, …)` |

**四个后端走同一条路**，消费方不写「哪个后端怎么建」：模块名、平台前后缀、Debug 后缀全在本库内
（§四）。某个仓与某个适配器有编译期依赖时，那条依赖写在它自己的 `CMakeLists` 链接行上，不写在
建适配器的那个函数里 —— 依赖变了不必回头改那段代码。

**种类枚举就是配置取值**：QuantTrading 的配置模型（`Model/Configs/*.xml`）把 `DbType` 声明成
`int`，生成出来的 `Config` 逐字读它，取值即 `DbTypeType` 的枚举值 —— 两处是同一个数，故消费方
只剩「越界校验 + 恒等转换」，没有自己的字面量表。本库只需要知道「有哪四个后端、各自模块叫什么」。

**越界一律拒，不退化成缺省**：配置模型只声明 `DbType` 是 `int`，没有取值域，一个写错的取值本来
会静默跑出一份看着正常的回测结果。实测 `DbType = 9`：一条列明四个合法取值的 ERROR，退出码 `1`
——走的是下面那个空指针出口，不是崩溃。

> **注意**：`DbType` 由字符串改成 `int` 之后，旧配置里 `"1"` 那种写法不再被接受 —— jsoncpp 的
> `asInt()` 遇上字符串会抛 `LogicError`，而抛出点同样在宿主 `try` 之外，实测退出码
> `0xC0000409`、日志 **0 字节**、无任何提示。凡手工持有旧配置处都要把值改成裸整数。

**装载 ≠ 不链接**：QuantTrading 的 `MdReader` 仍把 `DuckdbWrapper` 当成员类型用（编译期依赖），
故该模块既在导入表里、又被装载器 `dlopen` —— 同一模块，第二次取到的是已加载的句柄。
`SqliteWrapper` 没有这层依赖，链接行里留着它是为了随 `$<TARGET_RUNTIME_DLLS>` 拷进
`bin/$<CONFIG>`，让全新克隆的默认路径不缺文件（§八）。MySQL 与 MariaDB 有意不列。

**Linux 上的实测落点**：装载器的两条候选（模块目录 / 裸名）在这些机器上都没命中引擎目录，
最终由消费方的 `RUNPATH` 落到本库的安装树 —— `LD_DEBUG=libs` 实测 `libSqliteWrapper.so` 从
`/…/Libs/DbAdapters/x64-linux/lib` 装载，而全树没有任何二进制把 `SqliteWrapper` 写进导入表，
即这一次装载只可能来自装载器。**发运 Linux 引擎包时要留意**：模块得摆在引擎模块旁边，否则就要
靠 `RUNPATH` 指到本库的安装树。

**失败出口是空指针，不是异常**：调用链上适配器是在 `SimExchange` 构造函数里建的，抛出点不在
宿主的 `try` 作用域内，异常会一路走到 `std::terminate`。而在 Windows 上那是 `abort`，它既不
flush stdio 缓冲、也不走日志器线程的 `ThreadExit`（日志器是后台线程 + 缓冲，落盘在 `ThreadExit`），
写在抛出前的那条日志**会随进程一起消失**。故消费方要「写日志 + 返回空指针」，由引擎既有的判空
通路接管，进程正常退出，文案才落得进日志。抛出那条路的历史实测（字符串时代的 `DbType="9"`）：
退出码 `0xC0000409`、日志 0 字节；换成「写日志 + 返回空指针」之后，同样的坏取值实测退出码 `1`，
日志里留有那条 ERROR。

## 八、代价

不再链接 = `$<TARGET_RUNTIME_DLLS>` 不再把这两个适配器及其客户端链拷进 `bin/$<CONFIG>`。
**已存在的文件不会消失**，故现有工作树不受影响；只有**全新克隆后从头构建**才会缺，且缺了不
影响任何默认路径（`DbType` 取 `1` = SQLite）。这正是「可选」的含义。

## 相关文档

- [`README.md`](../README.md) 的「运行时按配置装载」一节 —— 最小用法示例
- [`environment-setup.md`](environment-setup.md) —— 构建与依赖
