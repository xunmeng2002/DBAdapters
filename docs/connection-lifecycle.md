# 连接生命周期：`Connect()` / `DisConnect()` 契约与断线重连

本文固定 `Db` 接口的连接语义、四个适配器在失败通道上的差异，以及 `AsyncDbWriter`
的断线重连与全量 resync 设计。改动这一层前请先读完本文——其中的取舍有若干条是被
连接器本身的能力边界逼出来的，不是风格偏好。

## 一、契约

`Db` 接口（`include/DbAdapters/DbInterface/Db.h`）只有两个连接相关函数，签名未变：

```cpp
virtual bool Connect() = 0;
virtual void DisConnect() = 0;
```

**`Connect()` 是一个幂等动作**，语义为「确保本连接可用，并返回是否可用」：

- 已连接 → **不重开**，直接返回 `true`；
- 未连接（构造期打开失败，或被 `DisConnect()` 关掉）→ **重建连接**，返回重建结果。

**`DisConnect()` 必须把内部句柄置空**。两条互为前提：不置空，`Connect()` 就没有东西
可以重建；而 `Connect()` 若不是「动作」而是「状态查询」，置空之后重连在类型上就不再可
表达（这正是本仓修复前的状态——`MysqlWrapper::Connect()` 曾是 `return impl_ != nullptr;`，
`impl_` 是构造后永不为空的 `unique_ptr`，故 `DisConnect()` 之后照旧返回 `true`，写线程既
感知不到断连、也不会重连）。

调用方唯一需要遵守的纪律：**把 `Connect()` 的返回值当作「此刻是否可用」**，不要把它当作
「是否曾连接成功」。

## 二、四个适配器的实现

| 适配器 | 构造期行为 | `Connect()` 的幂等判据 | 断开后重建的动作 |
| --- | --- | --- | --- |
| `SqliteWrapper` | `sqlite3_open` | `impl_->db == nullptr` | 重新 `sqlite3_open` |
| `DuckdbWrapper` | `duckdb_open` + `duckdb_connect` | `impl_->connection == nullptr` | 重新 open + connect |
| `MariadbWrapper` | 只取驱动实例 | `impl_->m_DBConnection == nullptr` | 重新 `driver->connect` |
| `MysqlWrapper` | 构造 `mysqlx::Session` | `impl_ == nullptr` | 重新构造 `Session` |

SQLite 与 DuckDB 的文件名此前只作为构造参数用完即弃，断开后无从重建，故两者的 `Impl`
各新增了一份 `dbName` 副本；打开逻辑从构造函数抽成 `OpenOrReportFailure()`，构造函数与
`Connect()` 共用（`MysqlWrapper` 对应的是 `OpenSessionOrThrow()`，失败仍抛出，由构造函数
转发给调用方、由 `Connect()` 转成返回 `false`）。

> **注意**：`MariadbWrapper` 的构造函数仍可能抛出，但抛的是**驱动装载失败**（客户端动态库
> 缺失），与「连不上」是两回事——连不上表现为 `Connect()` 返回 `false`。

`mysqlx::Session` 有移动构造但**没有移动赋值**，且 `Impl` 持有的是 `Session` 本体，故
MySQL 的重建只能是整块换掉：`std::make_unique<Impl>(mysqlx::Session(host_))`。

## 三、四个适配器在失败通道上并不对称

这一点决定了 `AsyncDbWriter` 的异常处理覆盖到哪里，必须清楚：

| 适配器 | 写路径失败时的行为 | `AsyncDbWriter` 的 `catch` 能否命中 |
| --- | --- | --- |
| `SqliteWrapper` | 记日志后返回（`LogOperationFailure`），**不抛** | 否 |
| `DuckdbWrapper` | 记日志后返回，**不抛** | 否 |
| `MysqlWrapper` | 抛 `mysqlx::Error` | 是 |
| `MariadbWrapper` | 抛 `sql::SQLException` | 是 |

于是 `HandleDbOperate` 的 `catch` 实际只对 MySQL / MariaDB 生效。本地文件数据库那一侧
「语句失败」被就地消化成日志，永远走不到重连分支——这不是疏漏，是两家 C 接口本就不以异常
报告失败。

但「**连接句柄已为空**」这一种失败四家是**统一**的：`Db` 接口的每个入口——外加 DuckDB 专有的
`SelectWithSqlVectorized`——先查句柄，为空则记一条错误日志（`<后端>: <操作名> skipped, ... is not
open.`）后直接返回，不再往下解引用。它拦的是
`DisConnect()` 之后（或建连失败之后）仍被调用的情形，与上表「连接虽已死但句柄仍在」不同——
后者仍走上表的异常/日志通道，写线程的重连触发条件不受影响。

## 四、为什么 MySQL 不能做错误分诊

`AsyncDbWriter` 只能采「任何操作异常即判连接可能已死」这一条策略，因为 X DevAPI 把另几条
路都堵死了：

1. **没有错误码**。`mysqlx::Error` 是 `class Error : public std::runtime_error`，只有
   `explicit Error(const char* msg)` 一个构造函数；Oracle 自己在头文件里留了
   `TODO: Derive from std::system_error and introduce proper error codes`。于是「连接断了」
   与「这条 SQL 语法错」在类型与码上都不可分。
2. **没有状态查询**。整棵 `mysqlx/` 头里没有 `isValid` / `isOpen` / `isClosed`，没有任何
   受支持的途径问一句「这条会话还活着吗」。
3. **没有读超时**。`SessionOption` 只提供 `CONNECT_TIMEOUT`，没有读超时。探活若撞上半开
   TCP（对端已死、FIN 未到），探测自己就会挂住。

误判的代价是**性能**而非**正确性**：一次多余的判死会触发重连 + 全量 resync（见下节），
而 resync 是效果幂等的。反之，若为省这一次重建去猜错误码，就可能把「断了」当成「语句错」
永久跳过——那是数据缺失，是不可逆的。

> **参考实现**：MariaDB 的 conncpp 有 `isClosed()`、`isValid()`、`reconnect()`，
> `SQLException` 也带 `getErrorCode()` / `getSQLState()`。本仓**刻意不用**这些能力，
> 四个适配器在 `AsyncDbWriter` 眼里保持同一副面孔。

## 五、重连与全量 resync

`AsyncDbWriter::Run()` 每轮先 `CheckConnect()`，只在 `connected_ == false` 时才调
`Connect()`。`Connect()` 成功后置 `connected_ = true` 并通知 `dbSubscriber_->OnDbConnected()`。

Mdb 侧的 `OnDbConnected()` 落到 `InitDb()`：`CreateTables()` + 每表一条 `OnRecordTruncate`
+ 每表一条全量 `OnRecordBatchInsert`。断开期间内存表的内容原样重放，效果幂等——这就是
「丢弃待办是安全的」的依据。

反向亦然：`OnDbDisConnected()` 把所有表的 `DbInited` 置 `false`，生产方受
`mdbSubscriber_ != nullptr && DbInited` 双守卫（约 121 处调用点），随即停止广播，因此
断连期间待办队列不会无界增长。

异常路径：`HandleDbOperate` 捕获异常后 `DisConnect()`（连带 `DropPendingOperates`）并休眠
5 秒，下一轮 `CheckConnect()` 重连并全量 resync。

### `AsyncDbWriter::Connect()` 的赋值顺序

`connected_ = true` 在通知**之前**设置，且 `OnDbConnected()` 抛异常只记日志、不影响返回值。

若反过来（先通知、回调成功才置真），回调一抛就会让 `Connect()` 返回 `false` 而
`CheckConnect()` 按 10 Hz 重试，每轮又跑一次 `InitDb()`（11 张表各一条 truncate + 一条
全量批插），与此同时 `connected_ == false` 让 `HandleDbOperate` 整队保留——队列会在 OOM
之前一直涨。现在的写法让返回值与 `connected_` 始终一致，resync 入队也能在同一轮被抽干。

### 析构

```text
~AsyncDbWriter() = Stop(); Join(); DropPendingOperates("Destructor"); delete db_;
```

`Stop()` / `Join()` 都是幂等的（`Stop()` 只置标志，`Join()` 先查 `joinable()`），与紧随其后的
`~ThreadBase()`（同样是 `Stop(); Join();`）重复调用无副作用。

**刻意不调 `DisConnect()`**：那会触发 `dbSubscriber_->OnDbDisConnected()`，而消费方完全
可能先于写线程销毁 `Mdb`，那就是 use-after-free。

## 六、已知边界

以下五条是本设计的既定取舍，已知且不修，改动前请先评估是否真的值得动：

1. **析构期间的并发入队**：若另一线程在 `DropPendingOperates()` 与 `delete db_` 之间调用
   `OnRecordInsert`，这些操作会随 `dbOperates_` 一起漏掉。前提是调用方违反「先
   `Stop()` / `Join()` 再改表」的纪律。
2. **resync 不重试**：`OnDbConnected()` 抛异常后 resync 不会补做，要等下一次断连。
3. **MySQL 下的永久失败语句**：会陷入「5 秒重连 + 全量 resync」的循环。resync 会把状态
   修回来，故表现是**停滞**而非**损坏**；本批未改。
4. **重试频率不节流**：固定一个 `timeOut_`（10 Hz），只有日志被 `connectFailureLogThrottle_`
   节流。既有行为，本批未加剧。
5. **写线程排空期间的并发 `DisConnect()`**：`HandleDbOperate` 只在取到操作时查一次
   `connected_`，`DisConnect()` 把后端连接置空后，队列里剩下的操作会撞上已关闭的连接。
   四个适配器 `Db` 接口的每个入口都有空守卫，故**不会崩，但那批操作被静默丢弃**（只留错误日志）。
   MySQL / MariaDB 的 `BatchInsert` 另有整批级守卫，否则一次空句柄的批插会按条数刷 N+2 条日志
   （SQLite / DuckDB 那两家靠 `failureLogThrottle` 节流，不需要）。
   与第 1 条同源：前提是调用方违反「先 `Stop()` / `Join()` 再断开」的纪律。

另有一条语义上的空白：本地文件数据库（SQLite / DuckDB）在实践中不会「断线」，二者的重连
能力是为**构造期打开失败后的重试**与**四家契约一致**而存在的，不是为应对运行期掉线。
