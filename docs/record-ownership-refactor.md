# 记录归属重构方案（`RecordHandle`）

> 状态：**已实施**（2026-10-05）。DbAdapters 全量重建通过，`TestDB.exe` 退出码 0，归属八条用例全 PASS
> （同日复裁后八条已迁入 `test/UnitTests`，`TestDB` 改留一条真后端端到端用例，见 §8）；
> Templates 改 `.tpl` 后 Mdb（11 表）/ QuantTrading（21 表）重跑 `pump.py`，生成物 diff 逐表核对；
> `cmake --install` 已落 `../Libs/DbAdapters/x64-windows`，Mdb 与 QuantTrading 的 `x64-Debug` 双双重编通过（§5.4 全四步已走完）。
> 起因：`DbOperateImpl::DeallocateRecord()` 对 `Insert | BatchInsert | Truncate` 早退，而 `BatchInsert` 交出的记录既不在内存表、也无其他一方释放。

## 0. 已定决策（2026-10-05 用户裁定）

| # | 决策 | 影响 |
| :--- | :--- | :--- |
| 1 | 改后无调用方的函数**一律删除** | `DbOperate::DeallocateRecord()` 虚函数、`TableSchema::DeallocateRecord` 字段、各表 `DeallocateXxx` 静态函数，全删 |
| 2 | 释放回调**按"保证不抛异常"实现** | 句柄析构不加 try/catch；`ReleaseDbOperate` 里那段 try/catch + 日志一并删除 |
| 3 | 批与单次**统一为一种类型** | 批 = `std::vector<RecordHandle>`，不做批级归还函数 |
| 4 | 是否补仓内回归（`AsyncDbWriter` 目前零仓内覆盖） | **并入 `TestDB`**，见 §8；同日复裁后改落 `test/UnitTests`，见 §8 末段 |

---

## 1. 问题与证据（已核实）

### 1.1 现象

`BatchInsert` 路径交出的记录无人释放：对象来自 `ObjectPool`，写完之后既不归还池，也不被任何一方持有，属内存泄漏。`ObjectPool` 按需扩容（`Spark/include/Spark/TemplateLib/ObjectPool/ObjectPool.h:270-303`），故不崩溃，只静默涨内存。

### 1.2 证据链

| 环节 | 位置 | 事实 |
| :--- | :--- | :--- |
| 生产方（快照） | `Mdb/src/Mdb/MdbTables.cpp:41-68`（`TradingDayTable::InitDb`） | `Allocate()` + `memcpy` 造的一批副本**只为写侧而建**，内存表里存在的是别人 |
| 生产方（批插） | `Mdb/src/Mdb/MdbTables.cpp:89-108`（`TradingDayTable::BatchInsert`） | 循环里 `newRecord = Allocate(); memcpy(newRecord, record, …); PrimaryKey->Insert(newRecord);` —— 内存表存**副本**；随后 `for (auto* r : *records) dbRecords->push_back(r);` 交给写侧的是**调用方原始指针**；`delete records` 连容器一并销毁 |
| 生产方（消费方调用点） | `QuantTrading/src/SimExchangeInit/ThostFtdcTraderSpiImpl.cpp:56-63` | `exchanges_` 交出去后不再使用，调用方无法再释放 |
| 内存表持有的是指针本身 | `Mdb/src/Mdb/MdbPrimaryKeys.h:27`、`.cpp:36-39` | `std::unordered_set<TradingDay*>` + `index_.insert(record)`，表通过 `TruncateTable` 里的 `(*it)->Deallocate()` 归还 |
| 写侧不释放批元素 | `src/DbAdapters/AsyncDbWriter/AsyncDbWriter.cpp:342-352` | `BatchInsertRecords` 只调 `db_->BatchInsert` |
| 写侧释放函数不覆盖批元素 | `src/DbAdapters/AsyncDbWriter/DbOperateImpl.cpp:13-33` | `Deallocate()` 只 `batch_data_.clear()`（清指针，不清对象）；`DeallocateRecord()` 对 `BatchInsert` 早退 |
| 适配器不释放 | `src/DbAdapters/SqliteWrapper/SqliteWrapper.cpp:367-375` 等四家 | `BatchInsert` 只循环 `Insert` |
| **契约文档与实现不符** | `README.md:465` / `README.en.md:460` | 白纸黑字写着"`Insert / BatchInsert` 的记录由调用方（内存库）管理生命周期"——**`BatchInsert` 的实现并不符合这句**：表存的是副本，交出去的原件从此无人持有。契约文本本身要改 |

### 1.3 对既有断言的更正

PROGRESS.md:54 的 ③ 措辞**不准确**，需按下列结论改写：

| `DeallocateRecord()` 早退项 | 结论 | 依据 |
| :--- | :--- | :--- |
| `Insert` | **早退正确且必要**，不是漏 | 交给了写侧的是内存表**正在持有**的同一条记录（`MdbTables.cpp:80-86`），释放即悬空 |
| `Truncate` | **两条路都不漏** | `AsyncDbWriter.cpp:187` 里 `Record = nullptr`；即使不早退，`DbOperateImpl.cpp:27` 的 `&& Record` 也拦得住 |
| `BatchInsert` | **确认是漏** | 见 1.2 |

不早退的三条（`Delete` / `Update` / `DeleteByIndex`）**均正确**：生产方在把记录交出去的同时已从内存表摘除（`MdbTables.cpp:110-123` Erase；`:124-146` Update 把内容 `memcpy` 进 `oldRecord`、交出去的是 `newRecord` 副本；`:873-896` EraseByIndex 是专为写侧新 `Allocate` 的临时记录）。

---

## 2. 根因

泄漏只是表征，根因是**归属的表达位置错了**：

1. **归属本应是"移交那一刻"的事实，现在却被编码成"按 `DbOperateType` 反推"的规则**（`DbOperateImpl.cpp:20`）。规则与生产方行为一旦脱节，编译器与测试都不会报错——本次就是这样（连 README 的契约文本也跟着写错了）。
2. **归还方式注册在表级**（`TableSchema::DeallocateRecord`，`include/DbAdapters/DbInterface/Schema.h:55`），宿主侧只拿到一个 `void*`，只能靠"猜"决定要不要回调它。调用方在 Mdb 侧创建、被调方在 DbAdapters 侧释放，跨模块的所有权契约全靠约定。

---

## 3. 目标 / 非目标

**目标**

1. 消除 `BatchInsert` 泄漏。
2. 归属在**移交点**由生产方声明，宿主侧不再按操作类型推断。
3. 保持 2026-10-04 那批加固确立的性质：释放单点、无新增锁、异常路径不重不漏。
   ~~无新增堆分配~~ —— **该条已更正（批后审查）**：句柄本身确实不分配（16 B POD，内嵌在池化的 `DbOperateImpl` 里），但
   `BatchInsertRecords` 为把 `std::vector<RecordHandle>` 转成 `Db::BatchInsert` 要求的 `const void* const*`，
   每批多一次 `std::vector<const void*>` 小分配，见 §7。

**非目标**

- 不改 `Table::Insert / BatchInsert / Erase / Update` 的公开签名——消费方调用点（如 `ThostFtdcTraderSpiImpl.cpp`、`InitMdbFromCsv.cpp`、`InitMdbFromDb.cpp`）**不动**。
- 不引入 `shared_ptr`：此处无共享所有权（内存表持有的是副本），控制块堆分配与原子计数纯属白付，而 `ObjectPool` 存在的意义正是省掉这次分配。
- 不顺手重构其他遗留项（见 PROGRESS.md ❓ 其余条目）。

---

## 4. 设计

### 4.1 `RecordHandle`：单条记录的类型擦除独占句柄

```cpp
// include/DbAdapters/DbInterface/RecordHandle.h
#pragma once

namespace DbAdapters
{
// 记录归还回调，由记录的生产方提供。
// 契约：保证不抛异常——句柄析构为 noexcept，违约直接 std::terminate。
using ReleaseRecord = void (*)(void*);

// 类型擦除的独占句柄：把"这条记录该怎么归还"与指针绑在一起。
// 持有即归还，借用即不归还——宿主侧不再按操作类型推断归属。
class RecordHandle
{
public:
    RecordHandle() noexcept = default;
    RecordHandle(void* record, ReleaseRecord releaseRecord) noexcept
        : record_(record), releaseRecord_(releaseRecord) {}
    ~RecordHandle() noexcept { Reset(); }

    RecordHandle(RecordHandle&& other) noexcept
        : record_(other.record_), releaseRecord_(other.releaseRecord_)
    {
        other.record_ = nullptr;
        other.releaseRecord_ = nullptr;
    }
    RecordHandle& operator=(RecordHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            record_ = other.record_;
            releaseRecord_ = other.releaseRecord_;
            other.record_ = nullptr;
            other.releaseRecord_ = nullptr;
        }
        return *this;
    }
    RecordHandle(const RecordHandle&) = delete;
    RecordHandle& operator=(const RecordHandle&) = delete;

    void* Get() const noexcept { return record_; }
    void Reset() noexcept
    {
        if (releaseRecord_ != nullptr)
        {
            releaseRecord_(record_);
        }
        record_ = nullptr;
        releaseRecord_ = nullptr;
    }

private:
    void* record_ = nullptr;
    ReleaseRecord releaseRecord_ = nullptr;
};

// 移交：自此刻起由接收方归还
template <typename Record>
RecordHandle AdoptRecord(Record* record) noexcept
{
    return RecordHandle(record, [](void* rawRecord) { static_cast<Record*>(rawRecord)->Deallocate(); });
}

// 借用：只借指针写库，归还仍归生产方（内存表同时持有同一条记录时用）
template <typename Record>
RecordHandle BorrowRecord(Record* record) noexcept
{
    return RecordHandle(record, nullptr);
}
}
```

### 4.2 批：统一用 `std::vector<RecordHandle>`

**不做批级归还函数**（决策 3）：单次与批共用同一个 `RecordHandle`，只有一种归属语义、只有一处 move/Reset 实现（Harness §5 DRY 要求）。代价是 staging 数组每元素 8 B → 16 B。

实测记录体量（`Spark/include/Spark/Types.h:163-190`：`DateType`=char[16]、`ExchangeIdType`=char[8]、`ExchangeNameType`=char[64]、`InstrumentIdType`=char[32]）：

| 记录 | 量级 |
| :--- | :--- |
| `TradingDay`（最小） | ≈ 36–40 B |
| `Exchange` | 72 B |
| `DepthMarketData` / `BarMarketData`（QuantTrading 最大） | 上百 B |

即：数组部分翻倍，但相对整批暂存只增约 10%–20%，且写完即释放。**此前方案里"16 B/元素不可接受"的说法已撤回**——那是拍脑袋，不是实测。

### 4.3 接口变更后的签名

```cpp
// include/DbAdapters/DbInterface/MdbSubscriber.h（记录类回调）
virtual void OnRecordInsert(unsigned int tableID, RecordHandle record) {}
virtual void OnRecordBatchInsert(unsigned int tableID, std::vector<RecordHandle> records) {}
virtual void OnRecordErase(unsigned int tableID, RecordHandle record) {}
virtual void OnRecordEraseByIndex(unsigned int tableID, unsigned int indexId, RecordHandle record) {}
virtual void OnRecordUpdate(unsigned int tableID, RecordHandle record) {}
// OnRecordTruncate / OnTableOp 无记录，签名不变

// include/DbAdapters/DbInterface/DbOperate.h
RecordHandle Record;      // 原 void* Record
// virtual void DeallocateRecord() = 0;   ← 删除（决策 1）

// src/DbAdapters/AsyncDbWriter/DbOperateImpl.h
std::vector<RecordHandle> batchRecords_;                                  // 原 std::vector<const void*> batch_data_
const std::vector<RecordHandle>& GetBatchRecords() const;                 // 该头只在 AsyncDbWriter.cpp 内被包含，
void SetBatchRecords(std::vector<RecordHandle> records);                  // 故以访问器承接句柄的移交
```

### 4.4 生产方调用点前后对照

| 表方法 | 现在 | 改后 |
| :--- | :--- | :--- |
| `InitDb` | `records->push_back(record)` | `records->push_back(AdoptRecord(record))`（移交） |
| `Insert` | `OnRecordInsert(TableId, record)` | `OnRecordInsert(TableId, BorrowRecord(record))`（内存表仍持有） |
| `BatchInsert` | `for (auto* r : *records) dbRecords->push_back(r);` | `dbRecords->push_back(AdoptRecord(r))`（移交） |
| `Erase` | `OnRecordErase(TableId, record)` | `OnRecordErase(TableId, AdoptRecord(record))`（已摘索引） |
| `EraseByIndex` | `OnRecordEraseByIndex(…, record)` | 同左，用 `AdoptRecord` |
| `Update` | `OnRecordUpdate(TableId, newRecord)` | `OnRecordUpdate(TableId, AdoptRecord(newRecord))`（内容已 `memcpy` 进 `oldRecord`） |
| `Truncate` | 无记录 | 不变 |

**未订阅分支**（`else { record->Deallocate(); }`）全部不变——归属没交给写侧，表自己还。

### 4.5 写侧简化

- `DbOperateImpl::DeallocateRecord()` **整段删除**。
- `ReleaseDbOperate`（`AsyncDbWriter.cpp:13-28`）只剩 `dbOperate->Deallocate();`——对象的析构（`ObjectPool.h:94` 的 `item->~T()`）自动归还记录，异常路径同样走析构，不重不漏。按决策 2，原来的 try/catch 与日志一并删除。
- `AsyncDbWriter` 的 6 个 `OnRecord*` 改为承接句柄；`ExecuteDbOperate` 各分支取 `dbOperate->Record.Get()`；`BatchInsertRecords` 用 `GetBatchRecords()`。
- `TableSchema::DeallocateRecord` 字段与其配套的 `DeallocateXxx` 静态函数按决策 1 一并删除。

---

## 5. 改动清单

### 5.1 DbAdapters 仓

| 文件 | 改动 |
| :--- | :--- |
| `include/DbAdapters/DbInterface/RecordHandle.h` | 新增 §4.1 |
| `include/DbAdapters/DbInterface/DbOperate.h` | `void* Record` → `RecordHandle Record`；删 `DeallocateRecord()` |
| `include/DbAdapters/DbInterface/MdbSubscriber.h` | 5 个记录类回调签名（§4.3） |
| `include/DbAdapters/DbInterface/Schema.h:55` | 删 `DeallocateRecord` 字段（后续字段同步前移） |
| `include/DbAdapters/AsyncDbWriter/AsyncDbWriter.h` | 5 个 override 签名同步；删私有 `AllocateDbOperate()`，加私有 `EnqueueDbOperate(...)` |
| `src/DbAdapters/AsyncDbWriter/AsyncDbWriter.cpp` | releaser 瘦身；8 个回调收敛到私有助手 `CreateDbOperate` / `EnqueueDbOperate`（见下）；`ExecuteDbOperate` 各分支 `.Get()`；`BatchInsertRecords` |
| `src/DbAdapters/AsyncDbWriter/DbOperateImpl.h/.cpp` | 成员改 `std::vector<RecordHandle>`；删 `DeallocateRecord()` |
| `test/TestDB/MdbStructs.cpp` | 11 处 `TableSchema` 初始化去掉 `DeallocateXxx` 实参；11 个 `DeallocateXxx` 静态函数删除 |
| `test/TestDB/TestDB.cpp` | 4 处手写 `TableSchema` 同步去参；新增 §8 的归属回归（探针记录 / `RecordingDb` 替身）；复裁后整块迁往 `test/UnitTests`，本文件改留一条真后端用例 |
| `test/UnitTests/` | 复裁后新建：doctest 单元测试工程，承接原 §8 八条用例与 `RecordingDb` 替身 |
| `test/Common/` | 复裁后新建：两个测试目标共用的探针记录与 `StaticSchemaRegistry` / `WaitUntil` 支撑头 |
| `test/CMakeLists.txt` | `TestDB` 补链 `AsyncDbWriter`；复裁后新增 `UnitTests` 目标并给两目标挂 `test/Common` |
| `README.md:465`、`README.en.md:460` | 记录所有权约定改写为"归还方式随句柄移交"（现文本与实现不符） |
| `docs/` 本文件 | 方案 |

> `include/` 目录整体安装（`CMakeLists.txt:123`），新头随包发布。

**`CreateDbOperate` / `EnqueueDbOperate`（批后复查补记，2026-10-05）**：`OnTableOp` 与 6 个 `OnRecord*` 原本各自 5 行——
`DbOperate::Allocate()` → 逐字段赋值 → `AddDbOperate()`——共 8 段近乎复制粘贴（含 `OnRecordBatchInsert`），违反 Harness §5。
现分出「建好但不入队」与「建好并入队」两层，8 处共用同一个建对象入口：

```cpp
DbOperate* AsyncDbWriter::CreateDbOperate(DbOperateType operate, unsigned int tableId, RecordHandle record, unsigned int indexId)
{
	DbOperate* dbOperate = DbOperate::Allocate();
	dbOperate->Operate = operate;
	dbOperate->TableId = tableId;
	dbOperate->IndexId = indexId;
	dbOperate->Record = move(record);
	return dbOperate;
}

void AsyncDbWriter::EnqueueDbOperate(DbOperateType operate, unsigned int tableId, RecordHandle record, unsigned int indexId)
{
	AddDbOperate(CreateDbOperate(operate, tableId, move(record), indexId));
}
```

两层是必要的：批路径必须**先设批记录再入队**（否则写线程会先弹出尚未设好批数据的操作），
故它用 `CreateDbOperate(...)` → `SetBatchRecords(...)` → `AddDbOperate(...)` 三步；其余 7 个回调各缩成一行 `EnqueueDbOperate(...)`。
声明处两个函数的后两个参数都有默认值（`RecordHandle record = RecordHandle(), unsigned int indexId = 0`），
故 `OnTableOp(op)` 写成 `EnqueueDbOperate(op, 0)`、`OnRecordTruncate(tableID)` 写成 `EnqueueDbOperate(DbOperateType::Truncate, tableID)`。
**等价性**：`ObjectPool::Allocate()` 走 `new (obj) T()` 值初始化，`Operate`/`TableId`/`IndexId` 归零、
`Record` 为默认空句柄，故"统一赋值 `IndexId` 与 `Record`"与替换前的逐字段赋值可观察行为相同
（原先未触碰的字段本就是零值）；代价是每句柄多一次 move（16 B POD，move 后源回调置空，不会双重释放）。
顺带消掉一处隐患：批路径原先不显式写 `IndexId`，靠"池中新对象必为零"这一隐藏前提；现在由 `CreateDbOperate` 统一赋值。

### 5.2 Templates 仓

| 文件 | 改动 |
| :--- | :--- |
| `Cpp/Mdb/MdbTables.cpp.tpl` | 7 处 notify 调用点（`:82` `:96` `:131` `:162` `:173` `:202` `:243` `:290`）按 §4.4 改；加 `#include <DbAdapters/DbInterface/RecordHandle.h>`、`<utility>` 与 `using namespace DbAdapters;`（后者漏加过一次，见 §5.4 执行记录） |
| `Cpp/Mdb/MdbStructs.cpp.tpl:181,184` | 删 `Deallocate!!$structName!!` 静态函数与 `TableSchema` 初始化里对应实参 |
| 其余 `.tpl` | **不必改**——`InitMdbFrom*.tpl` 走 `Table::Insert`；`Mdb.cpp.tpl` 只有 `OnTableOp`；`MdbTables.h.tpl` 的公开签名不变 |

### 5.3 生成物重跑

| 仓 | 命令 | 影响面 |
| :--- | :--- | :--- |
| Mdb | `pump.py`（逐目标） | `src/Mdb/MdbTables.cpp` 11 表、`src/Mdb/MdbStructs.cpp` 11 表 |
| QuantTrading | `pump.py`（逐目标） | `src/Mdb/MdbTables.cpp` 21 表、`src/Mdb/MdbStructs.cpp` 21 表 |

生成物是"勿手改"文件（文件头有声明），**必须改模板后重跑**，不得手写。

**未跑 `pumpall.py`**：其 `NeedPump` 是 mtime 启发式，会连带重写与本批无关、但模板早已不同步的产物
（实测 `QuantTrading/src/Packages/Packages.cpp` 会漂出 2104 行无关 diff，已 `git checkout` 还原）。
故按 `pumplist.xml` 里那两条 `<pump>` 逐目标调用 `pump.py`，范围精确到本次改动的两个 `.tpl`。

### 5.4 发布顺序

1. DbAdapters 改完 → x64-Debug 构建 + `TestDB.exe` 回归。
2. Templates 改 `.tpl` → Mdb / QuantTrading 各重跑一次 `pumpall.py`，**单独一次提交、逐表核对 diff**（只应出现 notify 调用点与 schema 初始化的变化）。
3. `cmake --install` DbAdapters 到 `../Libs/DbAdapters/x64-windows`（Mdb / QuantTrading 通过 `find_package(DbAdapters)` 取的就是这棵树）。
4. Mdb / QuantTrading 重编。

> 三仓 + 安装树必须同步；任何一仓滞后都会在链接期或运行期以 `DbOperate` 布局不一致的形式炸掉。

**执行记录（2026-10-05）**：四步已全部走完——① `x64-Debug` 重建、`TestDB.exe` 退出码 0、`TestRecordOwnership` 八条全 PASS；
② 两个 `.tpl` 改毕、生成物逐表核对无无关漂移；③ `cmake --install` 落 `../Libs/DbAdapters/x64-windows`（该树此前**已不存在**，
本次为重建；`x64-linux` 未重装，Linux 侧需另行重建）；④ Mdb 与 QuantTrading 的 `x64-Debug` 重编通过。
**下一步发现并已在本次内修掉的问题**：`MdbTables.cpp.tpl` 用到未限定的 `RecordHandle` / `AdoptRecord` / `BorrowRecord`，
却漏了 `using namespace DbAdapters;`（`MdbStructs.cpp.tpl` 本来就有这一句），Mdb 首次重编即报 `C2065 "RecordHandle": 未声明的标识符`；
补上后两仓生成物再跑一次、重编通过。
**批后独立审查与返工（同日）**：改动另过一次 `code-reviewer` 独立审查（0 严重 / 3 高 / 5 中）。按"Harness 明文的改，
动行为 / 动 ABI 的上报"处置：**已改**——README 示例去掉已删除的 `Deallocate` 回调实参与函数定义（`Account::Allocate()` 必须留，
`TypedTable<T>::GetFactory()` 依赖它）、`AsyncDbWriter.cpp` 补 `<utility>`、`AsyncDbWriter.h` 补 `<vector>` 与 `RecordHandle.h`、
`MdbSubscriber.h` 补 `RecordHandle.h`、`TestDB.cpp` 新增 include 归位、删去 4 处说明性注释（只留 `ReleaseRecord` 的
"回调不得抛异常"与 `BorrowRecord` 的"与内存表共持时必须借用"两条语言级陷阱）、抽出 `EnqueueDbOperate`（见 §5.1）。
**复验**：`x64-Debug` 重建、`TestDB.exe` 退出码 0、8/8 PASS、0 FAILED；`cmake --install` 重装（本次全 `--up-to-date`）；
Mdb 与 QuantTrading 双双重建通过。**上报不改**（§3 强制确认点）：`~AsyncDbWriter()` 不抽干待办队列、`DbOperate` 无虚析构、
`RecordHandle::Reset()` 未对 `record_` 判空——三条见 §9。

**第二轮审查（同日，针对上述返工本身）**：结论 **0 严重 / 0 高 / 2 中 / 3 低**，未发现双重释放、泄漏或并发回归；
等价性、move 后源不再释放、`Adopt`/`Borrow` 各调用点、include 自包含性逐项确认通过。两条「中」的处置：
① 批写路径每批新增一次 `std::vector<const void*>` 分配，与 §3 原列的「无新增堆分配」不符——**已更正 §3 并在 §7 登记**；
② `OnRecordBatchInsert` 与 `EnqueueDbOperate` 仍有「建对象 + 赋值」的重复——**已抽 `CreateDbOperate` 消除**（见 §5.1）。
三条「低」：`RecordHandle::Get()` 可收紧为 `const void*`（**属公开头文件签名，按 §3 上报未改**，见 §9.1）、
批路径显式写 `IndexId`（已随上条一并解决）、测试块内的说明性注释（与同文件既有风格一致，保留并在此披露）。

---

## 6. 验证与测试用例

### 6.1 构建与回归

- DbAdapters `x64-Debug` 全量重建；`TestDB.exe` 退出码 0、无新增 ERROR。
- Mdb / QuantTrading 重编通过（模板重跑后的硬性关卡）。

### 6.2 内存归属用例

Debug 版 `ObjectPool` 带 `OwnedItemRegistry`（`ObjectPool.h:136-205`），可据"该指针是否仍在登记表内"判定归属，无需外部工具。

| # | 输入 | 期望输出 |
| :--- | :--- | :--- |
| 1 | 一张表插入 3 条记录，订阅 `AsyncDbWriter` 后调 `Mdb::InitDb()`，库可用 | 3 条写完后：`InitDb` 造的那 3 个副本指针**不在** `LiveItems`（已归池）；库中 3 行 |
| 2 | `Table::Insert(record)` 一条 | 该指针**仍在** `LiveItems`（内存表持有）；库中 1 行；`TruncateTable()` 之后才消失 |
| 3 | 对 `RecordHandle` 做一次移动后原对象析构；批容器跨越扩容边界 | 不得出现 `Deallocate got an item that is not currently held`（Debug 断言），即 move 后源不再释放 |
| 4 | `ExecuteDbOperate` 抛异常（沿用既有仓外探针手法） | 批元素恰好释放一次，队列其余操作不受影响 |

### 6.3 静态核对

- `git grep -In "DeallocateRecord"` 归零（四个仓全查）。
- 生成物 diff 中不应出现与本次无关的漂移；一旦出现，说明模板跑之前就与生成物不同步，需先单独排查。

---

## 7. 风险与回滚

| 风险 | 说明 | 处置 |
| :--- | :--- | :--- |
| **ABI 断裂** | `DbOperate` 布局变更 + `MdbSubscriber` 虚表变更，是导出符号层面的破坏；三仓与安装树必须同时更新 | 按 §5.4 顺序发布，中间不留半更新状态 |
| **`TableSchema` 字段删除的面** | 位置参数初始化，牵动 Mdb 11 + QuantTrading 21 + TestDB 11 处；机械但要逐表核对 | 走生成器，TestDB 手改后单独 diff 复核 |
| **释放回调抛异常** | 决策 2 定的是"回调保证不抛"：`~RecordHandle` 为 `noexcept`，回调违约直接 `std::terminate`。原先 `ReleaseDbOperate` 里的 try/catch + 日志容错随之取消（本次已删） | 在 `RecordHandle.h` 的 `ReleaseRecord` 上写明契约；`T::Deallocate()` 是归还对象池，本身不抛，风险落在未来新增的释放实现上 |
| **生成物漂移** | 重跑 `pumpall.py` 可能带出与本次无关的 diff | 先只改 `.tpl` 并在单表上验证，再全量重跑；生成物单独一次提交、逐表核对 |
| **移动语义** | 移动后的句柄若仍持有旧指针会双重归还（Debug 版池会断言） | 用例 3 覆盖，含 `vector` 扩容路径 |
| **批写路径每批一次小分配** | 决定③把批存储统一为 `std::vector<RecordHandle>`，而 `Db::BatchInsert` 要求 `const void* const*`，故 `BatchInsertRecords` 每次执行新建一个指针数组并 `reserve` 一次；旧实现直接 `batch.data()`，无此分配。批后审查指出 §3 原列「无新增堆分配」与实现不符 | **已接受并登记**：每批一次、随操作对象出池即释放，非正确性问题。若要归零，需把 `Db::BatchInsert` 的入参改成句柄视图——牵动四家适配器与 ABI，属更大改动，**须用户裁定** |
| **多线程** | 移交后记录只由写线程触碰，不引入新竞态；风险仅在"生产方移交后是否还读"——已在 §4.4 逐条核对为否 | 新增 notify 调用点时须重新核对 |

**回滚**：按仓 `git revert` 本次提交；生成物与模板同批提交，回滚不会留下模板/生成物不一致。

---

## 8. 回归覆盖（2026-10-05 复裁：抽出单元测试工程，`TestDB` 留一条真后端用例）

`AsyncDbWriter` 此前零仓内覆盖，验证靠仓外一次性探针。当日首裁定把归属判定固化进
`test/TestDB/TestDB.cpp`，并为其补链 `AsyncDbWriter`（`test/CMakeLists.txt`）。

**同日复裁**：`TestDB` 是集成测试可执行文件，要链四个 Wrapper、要拷 DLL 才能跑，而归属语义
与后端无关（下表八条里三条是纯句柄语义，五条只需 `AsyncDbWriter`），这层放错了位置。故抽出
`test/UnitTests` 单元测试工程（doctest，只链 `doctest::doctest` / `Spark::Core` / `AsyncDbWriter`，
不需要四个 Wrapper、不需要 duckdb.dll），承接下表八条用例；`test/Common` 收两目标共用的探针
记录、`StaticSchemaRegistry` 与 `WaitUntil`。`TestDB` 仍保留**一条**真后端端到端用例
（`TestAsyncWriterRecordOwnership`：真 sqlite + 真写线程，断言记录确实落库且归还恰好一次），
因为"句柄语义跨真实后端与 DLL 边界成立"这一点是替身后端给不出的。

判定手法：探针记录持一个外部 `std::atomic<int>*`，`Deallocate()` 时自增，于是"归还了没""归还了几次"
成为可断言的事实，不必依赖内存工具或 Debug 版对象池的内部登记表。后端用 `RecordingDb` 替身计数，
并可按需在 `Insert` 上抛出。

| # | 用例 | 断言 |
| :--- | :--- | :--- |
| 1 | 借用不归还 | 写侧消费后探针计数仍为 0，且传给后端的指针与内存表持有的是同一个 |
| 2 | 移交恰好归还一次 | 探针计数为 1 |
| 3 | 批元素逐个恰好归还一次 | 三个探针各计 1，后端收到的批大小与首尾指针正确（**改动前此处整体泄漏**） |
| 4 | 移动后源不再归还 | 移动后源 `Get()` 为空，计数为 1 |
| 5 | 移动赋值先归还旧记录 | 赋值当场归还接收方原有记录，作用域结束归还源记录，各 1 |
| 6 | 容器扩容不重复归还 | `reserve(1)` 后连推三个，扩容期间计数为 0，`clear()` 后为 3 |
| 7 | 执行抛异常仍恰好归还一次 | 计数为 1，且写侧已转为断开重连 |
| 8 | 断开丢弃待办时恰好归还一次 | 未连接状态下入队两条，`DisConnect()` 后各计 1 |

复裁后落点：用例 4、5、6 进 `RecordHandle` 套件（纯句柄语义，不碰写线程），用例 1、2、3、7、8
进 `RecordOwnership` 套件（各用一个 RAII harness 持有 `AsyncDbWriter`）。用例 3、7、8 各自对应
一条曾经的泄漏或悬挂路径，任一条回退都会让 `UnitTests` 退出码非 0。

## 9. 本次未改、留待裁定的相邻问题（2026-10-05 已裁决，见 §11）

**问题（原文保留）**：`Table::BatchInsert` 的**未订阅分支**（`mdbSubscriber_ == nullptr || !DbInited`）
里，调用方原始记录只被 `delete records` 释放了容器、元素本身仍无人归还。这与本次修掉的是同一族缺陷，
但方案 §4.4 明确把未订阅分支列为"不变"，故未动。

**当初列为"不变"的理由**：批插当时的归属口径是 `AdoptRecord`——记录交给写侧、由写侧归还。未订阅时
写侧不存在，那批记录便既不在内存表、也无第三方持有。改口径会牵动调用方契约，而本批的既定范围是
"只修被记录归属重构暴露出来的泄漏"，不宜顺手改契约，故只登记不动。

**当初备选（未采用）**：把 `AdoptRecord` 的包装提到 `if` 之前，两条路都交给句柄析构，代价是该分支下
调用方原始记录也会被释放（现有三处调用点 `ThostFtdcTraderSpiImpl.cpp` 交出后均不再使用，已核对）。
**未采用的理由**：它只堵泄漏，不解决 `Insert`（借用）与 `BatchInsert`（移交）两处口径分裂，而分裂
本身是这些泄漏的温床。

**裁决与落地（2026-10-05，用户指令「去做吧」）**：改为把批插的口径与 `Insert` 统一——内存表直接持有
调用方交来的记录，写侧改用 `BorrowRecord` 只借不还。泄漏随之自然消失：未订阅时记录就在内存表里，
由 `TruncateTable(s)` 或 `InitDb()` 接管，"只给写侧的载荷"这个角色不存在了。详见 §11。

### 9.1 批后审查提出的四条（2026-10-05 发布前 API 冻结批分两批处置）

| # | 问题 | 当前是否可达 | 处置（2026-10-05） |
| :--- | :--- | :--- | :--- |
| 1 | `~AsyncDbWriter()` 不抽干待办队列：`DropPendingOperates()` 只在 `DisConnect()` 里调用，析构时队列里的 `DbOperate` 连同其 `RecordHandle` 持有的记录一并泄漏。**非本批引入**（旧代码同样不抽干，只漏操作对象），但重构后同一条路径多漏一份记录 | 是（仍有待办时析构即触发） | **已改**（2026-10-06 连接生命周期批，见 §13）：析构里补 `Stop(); Join(); DropPendingOperates("Destructor");`——`Stop()` / `Join()` 均幂等，与紧随其后的 `~ThreadBase()` 重复调用无副作用；**刻意不调 `DisConnect()`**，那会触发 `dbSubscriber_->OnDbDisConnected()`，而消费方可能先于写线程销毁 `Mdb`。新增用例「析构抽干待办队列并归还记录」钉住 |
| 2 | `DbOperate` 有纯虚 `Deallocate()` 却无虚析构（`rules/cpp-style.md` §6 强制项，既有） | 否——全程经 `ObjectPool<DbOperateImpl>` 按具体类型归还，仓内无 `delete DbOperate*` | **已改**：补 `virtual ~DbOperate() = default;`。虚表多一槽，属导出符号的 ABI 变更，已随本批 `cmake --install` 与 Mdb / QuantTrading 重建一并同步（`DbOperate` 只在本库内构造，消费方从不构造或析构它） |
| 3 | `RecordHandle::Reset()` 未对 `record_` 判空：`AdoptRecord(nullptr)` 会以空指针调用 `record->Deallocate()` | 否——三处工厂实现为 `ObjectPool::Deallocate`（自带空指针早退）与 `delete`（空指针安全） | **已改**：改为 `if (releaseRecord_ != nullptr && record_ != nullptr)`——空记录统一按"未持有"处理，与 `BorrowRecord` 语义一致；**不构成"掩盖错误"**，因为本就无记录可漏，缺的只是不崩。新增用例「空记录不调用归还回调」钉住（改前会在 `Deallocate()` 里以空 `this` 读成员而崩） |
| 4 | `RecordHandle::Get()` 返回可变 `void*`，`rules/cpp-style.md` §7 建议 getter 返回 const | 否——5 处调用点全部把结果交给 `const void*` 形参（`Db::Insert`/`Delete`/`Update`/`BatchInsert`），无一处需要可写指针 | **已改**：`const void* Get() const noexcept`。5 处调用点（`AsyncDbWriter.cpp:308/323/332/345/358`）逐处核对为「传 `const void*` 形参」或「`push_back` 进 `std::vector<const void*>`」，无一处需要可写指针，故是纯收紧、零调用点改动 |


## 10. 发布前 API 冻结批（2026-10-05）

Release 前按 Harness §3 逐条裁定公开 API 面，用户指令「1234 都改了吧」（第 5 条即 §9 的
`Table::BatchInsert` 未订阅分支，用户另议：「我再看看」）。本批四项：

| # | 改动 | 文件 | 性质 |
| :--- | :--- | :--- | :--- |
| A1 | `void* Get() const noexcept` → `const void* Get() const noexcept` | `RecordHandle.h` | 公开头签名收紧 |
| A2 | 三个单参构造成员加 `explicit` | 三个 Wrapper 头 | 公开头签名：删去隐式转换路径 |
| A3 | 补 `virtual ~DbOperate() = default;` | `DbOperate.h` | 导出符号：vtable 多一槽 |
| A4 | `Reset()` 加 `record_ != nullptr` 判空 | `RecordHandle.h` | 头内实现，行为收紧 |

**A2 的调用点核对**：全盘（DbAdapters / Mdb / QuantTrading）三处 Wrapper 的构造**全是直接初始化**
（`new X(...)`、`X name(...)`、`return new X(...)`），无一处依赖隐式转换，故加 `explicit` 零破坏；
`MariadbWrapper` 为 3 参构造本就不受该规范约束，四家由此一致。

**A3 的同步面**：`DbOperate` 只在本库内构造——`DbOperate::Allocate()` 与 `ObjectPool<DbOperateImpl>`
都定义在 `DbOperateImpl.cpp`，消费方既不构造也不析构它。故 vtable 变更的实质影响面是本库 + 安装树；
仍按 §5.4 的纪律把 Mdb / QuantTrading 一并重建，不留半更新状态。

**未纳入本批**：§9 的 `Table::BatchInsert` 未订阅分支（该条已于 §11 裁决落地），以及 §9.1 第 1 条
（`~AsyncDbWriter()` 不抽干待办队列）——后者已由 2026-10-06 的连接生命周期批改掉，见 §13。

**验证**：`UnitTests.exe` → **9 用例 / 26 断言全过、退出码 0**（改前 8 / 24，新增「空记录不调用归还回调」）；
`TestDB.exe` 退出码 0、`TestAsyncWriterRecordOwnership: rowWritten=1 releasedOnce=1`；`cmake --install`
把新头与新 DLL 落到 `../Libs/DbAdapters/x64-windows`（非 Up-to-date）；随后 Mdb `TestMdb` 重建 + 实跑
退出码 0（27 行，`grep "ownership\|Assertion\|ObjectPool"` 零命中）、QuantTrading `SimExchangeInit`
重建退出码 0（未运行，需 CTP 环境）。

## 11. 批插入归属口径与 `Insert` 统一（2026-10-05）

§9 的裁决落地。改的是模板 `Templates/Cpp/Mdb/MdbTables.cpp.tpl` 的 `BatchInsert`，再按各仓
`pumplist.xml` 逐目标重跑 `pump.py` 生成 Mdb（11 表）与 QuantTrading（21 表）的 `MdbTables.cpp`。

### 11.1 改了什么

| | 改前 | 改后 |
| :--- | :--- | :--- |
| 进内存表的 | `Allocate()` + `memcpy` 造出的**副本** | 调用方交来的记录**本身** |
| 交给写侧的 | 调用方原始记录，`AdoptRecord`（写侧归还） | 表中那批记录，`BorrowRecord`（写侧只借） |
| 未订阅时 | 原件无人归还（**§9 的泄漏**） | 记录在表里，由表接管，**无泄漏** |
| 与 `Insert` 的口径 | 两样 | 一致 |

改后每条记录省下一次 `Allocate()` + `memcpy(sizeof(T))`。**但这只是副产品**：真正的收益是 §9 的泄漏
自然消失、`Insert` 与 `BatchInsert` 的归属语义不再分裂。

### 11.2 新增的调用方义务（必须写下来）

`BorrowRecord` 的固有前提此前只隐含在 `Insert` 上，现在批插也适用，故写进 README 契约：
**借用期间调用方不得改写或释放该记录**——`Update` 会 `memcpy` 覆写它的字节、`Erase` 与
`TruncateTable` 会把它归还给对象池，而写线程此刻可能正在读它（`AsyncDbWriter` 是异步消费）。
凡是这批记录写出去之前就要改表，先 `Stop()` / `Join()` 写线程。

判断依据是写侧的消费时机：`OnRecordBatchInsert` 只是入队，真正的 `db_->BatchInsert` 发生在写线程
上（`AsyncDbWriter.cpp:311-325`），这段窗口里主线程动那条记录就是数据竞争。

**为何接受这个代价**：`Insert` 早已在担同一风险（`AdoptRecord` 会让内存表里的活记录变成悬空指针，
故 `Insert` 必须借用）。批插改前靠"给写侧一份副本"回避了它，代价是每条记录一次多余分配、且未订阅时
泄漏。两处口径统一后，风险同为一条、可一句话说清；分裂则要每次分情况推理。

### 11.3 本批未动

`BatchInsert` **不调 `CheckInsert`**，与 `Insert`（查重、失败即 `Deallocate()` 并返回 false）不对称。
这是**行为变更**（现在同主键会一并进表），超出本批范围，仍为悬项。**该悬项已于同日裁定——结论是采用调用方契约、另加零成本 Debug 断言，Release 下不检查，见 §12。**

### 11.4 验证

模板改后重跑 `pump.py`（Mdb、QuantTrading 各一次，均退出码 0），生成的 diff 与预期逐字相符：
每张表的 `BatchInsert` 只少两行（`Allocate` + `memcpy`）、`newRecord` 全部换成 `record`、
`AdoptRecord(r)` 换成 `BorrowRecord(r)`，无其它改动。

新增回归用例 `TestMdb.cpp` 的 `TestBatchInsertTakesOwnership()`（未订阅分支）：
输入 `Exchange::Allocate()` 造 3 条记录交给 `BatchInsert`，期望 `SelectAll()` 数到 3 行**且**其中一条
是**调用方交出的那个指针**（`storedRowCount=3 storedHandedInRecord=1`），随后 `TruncateTable()`
把它们归还池。**改前该断言必为 0**——改前表里存的是 `Allocate()` 造的新槽位，不可能等于调用方已持有的
记录。`main()` 据此返回退出码。

`UnitTests`（9/26）与 `TestDB` 不涉及本路径，未重跑；Mdb `TestMdb` 实跑退出码 0、输出 28 行、
`grep "ownership violation\|Assertion\|ObjectPool"` 零命中；QuantTrading `SimExchangeInit` 重建退出码 0。

### 11.5 顺带发现（未改）

`ThostFtdcTraderSpiImpl.cpp:62/113/169` 把成员 `exchanges_` / `products_` / `instruments_` 交给
`BatchInsert`，而 `BatchInsert` 结尾 `delete records;` 连容器一并销毁，调用方却未把它们重新指向新容器。
**这是本批之前就有的缺陷**（`delete records` 在改前的模板里已有），与本次归属口径无关；断线重连会
再次触发 `ReqQryExchange()` 并对悬空指针 `push_back`。登记待议，本批不动。

## 12. 批插入遇重复键的处置：调用方契约 + 零成本 Debug 断言（2026-10-05）

§11.3 登记的悬项落地。**最终定案：把「批内无重复键、且不与表内已有键冲突」写成调用方契约；
另加一条零额外查找的 Debug 断言（方案 B），让违反契约的行为在调试期立刻暴露。**

### 12.1 讨论过程与两次改主意（记下来，免得下次重议）

1. 用户先提「在循环里调 `CheckInsert` 处理有点麻烦、返回值也不好弄，你怎么看」。
1. 讨论中确认了一件事：`CheckInsert` 不是「查重 API」——它是 `index_.find(record) == index_.end()`，
   而各索引的 `Insert(record)` 本身就是 `index_.insert(record).second`，**返回值已经说明了这条进没进去**。
   它存在的唯一理由是让单条 `Insert` 在动任何索引之前把主键与全部唯一键一起探完，做到
   「预检失败 ⇒ 什么都没插」。故 §11.3 说的不对称不是「批插忘了查」，而是
   **`Insert` 有 all-or-nothing 契约、`BatchInsert` 没有契约**。
1. 据此建议方案 A（逐条预检 + 跳过 + 归还，保留 `void` 返回），用户当时答「按 A 实现吧」。
1. **A 实现并验证通过后，用户复审实现，判定「还是太麻烦了，影响性能」，改为纯契约方案**。A 的模板
   改动、两个仓的生成物与测试**已全部撤回**：`git checkout` 模板后重跑 `pump.py`，两仓
   `MdbTables.cpp` 回到原提交内容；`TestMdb.cpp` 一并还原。
1. 撤回后用户再答「可以，把这个加上吧」，**采纳方案 B**。B 不是检查：它不增加任何查找，只是把
   `PrimaryKey->Insert` / `*UniqueKey->Insert` **本来就返回**的那个 `bool` 存进变量交给 `assert`，
   Release 下断言连同变量一起消失。**契约本身未变，只是多了一条调试期的执行手段。**

### 12.2 契约是什么

**`BatchInsert` 要求调用方保证：这批记录内部无重复键，且其主键与唯一键均不与表中已有记录冲突。**
满足该前提时，`BatchInsert` 只做「写进各索引 + 通知写侧」，逐条零额外查找、零额外分支。

契约的依据是现有三处调用点的实际用法：`ThostFtdcTraderSpiImpl.cpp:62/113/169` 交进来的都是后端查询
回来的全量列表，落在一张刚建好的空表上；`InitMdbFromCsv` / `InitMdbFromDb` 一类回填路径同理。

### 12.3 违反契约的后果（必须写下来——这是这个选择的代价）

**在 Release 下：没有检查、没有日志、不会失败。** 实际发生的是三条：

1. 重复的那条**无人持有**——本表唯一的所有权登记处是 `PrimaryKey->index_`（`TruncateTable(s)` 正是
   遍历它来 `Deallocate()`），竞争失败的记录不在其中，故 `Select` 永远查不到、`TruncateTable` 永远
   不释放 → **池槽位泄漏**。
1. 它**照样被写进数据库**——通知块遍历的是调用方的 `*records`，不是索引 → **内存表与库静默分叉**。
1. 唯一键竞争失败时，记录在主键集合里却不在某个唯一键里 → **索引不变量被破坏**。

第 2 条最难查：库里有、内存表里没有，且没有任何一行日志指向它。

**Debug 下这三条不会静默发生**：方案 B 已落地，重复键会在 `Insert` 返回 `false` 的那一句直接
`assert` 中断进程。换言之，**Release 静默、Debug 立刻炸**——违反契约的用法在开发期就被抓住，
而生产构建仍保持零开销。

### 12.4 三个备选：B 已采纳，A / C 未采纳

| 备选 | 结论 |
| :--- | :--- |
| **A：逐条预检 + 跳过 + 归还**（曾实现并验证通过后撤回） | **未采纳**。用户判定「太麻烦、影响性能」——批插是批量回填路径（合约可达数万条），每条多一次主键查找、每个唯一键又一次，虽都是 O(1) 哈希查找，但在热路径上被放大 |
| **B：零成本 Debug 断言**——把 `Insert` 的返回值存进变量再 `assert(inserted)`；**不可**写成 `assert(Insert(record))`，NDEBUG 下会把有副作用的调用一起删掉 | **已采纳**（2026-10-05 第二轮）。它不增加任何查找——返回值本来就有；变量加 `[[maybe_unused]]` 以免 Release 下报「未引用」，生产构建零开销。形态与 `ObjectPool` 的 `OwnedItemRegistry`（`#ifndef NDEBUG`）一致 |
| **C：整批 all-or-nothing** | **未采纳**。必须让调用方知道结果 → `void` 要改成 `bool` 或返回条数，属公开 API 变更（Harness §3.1），且三处调用点均不看返回值 |

B 的落地范围**只覆盖主键与唯一键**：`PrimaryKey` / `*UniqueKey` 的 `Insert` 返回
`index_.insert(record).second`（`bool`），是唯一可断言的两类；二级索引是 `std::multiset`，
`Insert` 返回 `void`，重复键本就合法，不在断言范围。

### 12.5 验证

**第一轮（撤回 A）**：重建并实跑，Mdb `TestMdb` **退出码 0、输出 28 行**、`BatchInsert ownership:
storedRowCount=3 storedHandedInRecord=1`、`grep "Assertion\|ObjectPool\|violation"` 零命中——与
撤回前那一批（本仓 ✅「批插入归属口径与 `Insert` 统一」）的观测值逐字一致。QuantTrading
`SimExchangeInit` 重建退出码 0（未运行，需 CTP 环境）。Templates 的模板与两仓的 `src/Mdb/MdbTables.cpp`
按 `git status` 均干净，即它们都已逐字节回到原提交内容。

**第二轮（落地 B）**：模板 `BatchInsert` 的插入语句改为「存 `bool` + `assert`」，并补
`#include <cassert>`；按 `pumplist.xml` 重跑 `pump.py`，Mdb `MdbTables.cpp` `+25/-12`（11 表）、
QuantTrading `+45/-22`（21 表，即两仓仅 1 个唯一键：`Order` 表的 `ClientOrderIdUniqueKey`）。
**为观测断言所做的尝试，及其整体撤回（2026-10-06）**。曾加 `TestMdb` 的
`BatchInsertContractIsEnforced()`：以 `std::system` 拉起**自身子进程**（开关
`--verify-batchinsert-contract-abort`），子进程批插两条同 `ExchangeId` 的记录。第一版让子进程被
`assert` 直接崩掉，`std::system` 收到的是**异常码**——用户机器上 `-2147483645`（`0x80000003`
`STATUS_BREAKPOINT`，MSVC 无调试器时 CRT 断言经 WER 终止进程的异常码），Windows 据此弹出
「已停止工作」。第二版在子进程里装 `std::signal(SIGABRT, ExitOnAbortSignal)` → `std::_Exit(42)`，
并调 `MakeAssertTerminateQuietly()`（`_CrtSetReportMode` / `_CrtSetReportFile` /
`_set_abort_behavior(0, _CALL_REPORTFAULT)`）静音 CRT，再把子进程输出重定向到空设备——本机实测
可行（Debug 子进程 `childStatus=42`、Release `=0`，主进程退出码 0），**但用户机器上 `std::system`
一行仍然崩溃**（2026-10-06 第二次反馈）。

**结论：整套死亡测试撤回**。`TestMdb.cpp` 用 `git checkout` 逐字节回到原提交内容，不再生成任何会死的
进程——「测试程序绝不弹崩溃」这个要求，只有**不制造崩溃**能满足；进程故意去死这件事，一旦跨环境就
压不住，再往上加 CRT / WER 相关的处理只会更脆。断言本体保留在模板与两仓生成物里，一个字未动。

**验证（撤回后实跑）**：Debug 与 Release 两版 `TestMdb` 均退出码 0，输出含
`BatchInsert ownership: storedRowCount=3 storedHandedInRecord=1`，无任何断言或子进程痕迹。
QuantTrading `SimExchangeInit` 重建退出码 0（未运行，需 CTP 环境）。

**断言的观测办法（手工，不再自动化）**：违反契约时 Debug 下必然中断进程——这本就是断言的语义，
不可能在同一进程里既触发又继续。要让它响一次，临时把 `TestMdb.cpp` 的 `TestBatchInsertTakesOwnership()`
里第二个 `Exchange` 的 `ExchangeId` 改成与第一个相同（`SHFE` / `SHFE`），Debug 构建下运行即打印
`Assertion failed: insertedIntoPrimaryKey, file ...MdbTables.cpp, line NNN` 后终止；Release 下同输入
不打印、正常跑完。**输入输出示例**：输入 `{ "SHFE", "SHFE" }` 两条 `Exchange`（同主键）→ Debug
输出断言失败并终止，Release 输出正常结束。**已知取舍**：断言因此**没有自动化回归覆盖**——将来若有人
把模板里的「存 `bool` + `assert`」改回「直接调用、不取返回值」，测试不会报警，只能靠评审模板 diff 拦住。

### 12.6 未纳入

§9.1 第 1 条（`~AsyncDbWriter()` 不抽干待办队列，已由 §13 改掉）与 §11.5
（`ThostFtdcTraderSpiImpl` 的悬垂容器）曾为悬项。

---

## 13. 连接生命周期批：`Connect()` 语义统一 + 析构抽干待办（2026-10-06）

用户指令「不要考虑发布前发布后，我需要一个最终定稿的版本，一次做好」，以及
「公开行为变更不是问题，现在还没有外部使用者，还没有正式发布。不要加注释，如果觉得必须要存档，就写文档」。
本批一次收掉 ❓ 区仅存的三个连接类条目（`MysqlWrapper::Connect()` 恒真、`AsyncDbWriter::Connect()`
的赋值时序、`~AsyncDbWriter()` 不抽干待办队列），并把 **`Connect()` 是幂等动作** 定为四个适配器
共同的契约。

完整设计（契约、四家实现对照、失败通道不对称、MySQL X DevAPI 的三条死路与证据、重连与全量 resync、
已知边界）落在新建的 [`docs/connection-lifecycle.md`](connection-lifecycle.md)，本节只记改动与验证。

### 13.1 改动

| # | 文件 | 改动 |
| :--- | :--- | :--- |
| 1 | `MysqlWrapper.h/.cpp` | 抽出 `OpenSessionOrThrow()`（构造函数与 `Connect()` 共用）；`Connect()` 由 `return impl_ != nullptr;` 改为「`impl_` 为空就重建，重建抛异常则返回 `false`」；`DisConnect()` 补 `impl_.reset()`；六个操作入口加 `CheckSessionOpen()` 空守卫 |
| 2 | `SqliteWrapper.h/.cpp` | `Impl` 加 `dbName`；打开逻辑抽成 `OpenOrReportFailure()`；`Connect()` 在 `db == nullptr` 时重开 |
| 3 | `DuckdbWrapper.h/.cpp` | 同上（`Impl` 加 `dbName`，抽出 `OpenOrReportFailure()`，`Connect()` 在 `connection == nullptr` 时重开）；`OpenOrReportFailure()` 内两个句柄独立判空；补上专有的 `SelectWithSqlVectorized` 的空守卫（此前七处入口里唯一漏掉的一处） |
| 4 | `MariadbWrapper.h/.cpp` | `Connect()` 开头加幂等判据（已连接直接返回 `true`）；六个操作入口加 `CheckConnectionOpen()` 空守卫 |
| 5 | `AsyncDbWriter.cpp` | ① 析构改为 `Stop(); Join(); DropPendingOperates("Destructor"); delete db_;`；② `Connect()` 改为先置 `connected_ = true` 再通知，`OnDbConnected()` 抛异常只记日志、不影响返回值；③ `DropPendingOperates(const char* triggerReason)`，日志文案中性化 |
| 6 | `AsyncDbWriter.h` | `DropPendingOperates` 签名加 `triggerReason` |

**第 5 条②的取舍**（原 ❓ 是"赋值在回调之前"）：反过来写（先通知、成功才置真）会让回调一抛就
返回 `false`，而 `CheckConnect()` 按 10 Hz 重试，每轮又跑一次 `Mdb::InitDb()`（11 张表各一条
truncate + 一条全量批插），与此同时 `connected_ == false` 让 `HandleDbOperate` 整队保留——队列会在
OOM 之前一直涨。现在的写法让返回值与 `connected_` 始终一致。**原 ❓ 认定"回调抛出后不再重试"是缺陷，
复查后判定它恰恰是想要的行为**：该回调的语义是"通知 resync"，不是"连接的一部分"。

**未改**：`HandleDbOperate` 的 `catch`（任何异常即判连接可能已死 + `DisConnect()` + 5 秒休眠）保持原样，
依据是连接器能力边界而非偷懒——见 `docs/connection-lifecycle.md` 第四节。

### 13.2 用例

| 用例 | 位置 | 判据 |
| :--- | :--- | :--- |
| 析构抽干待办队列并归还记录 | `test/UnitTests/RecordOwnershipTests.cpp` | **改前必失败**：`WriterHarness(false)` 下入队一条后作用域结束，作用域内计数 0、作用域外计数 1；改前外部仍为 0（记录随队列泄漏） |
| 断连后写线程自行重连 | 同上 | **回归护栏，非改前必失败**：`RecordingDb` 替身无从体现「`Db::Connect()` 恒真」，改前即通过。它的价值是钉住「重连由写线程侧发起」不被后续改动推翻 |
| 重连后可完成真实读写 | `test/TestDB/TestDB.cpp` 的 `TestReconnect(Db&, const char*)` | **改前必失败**：`Connect → DisConnect → Connect` 之后建表、写入、读回一行，断言 `size() == 1 && PK == 7`。**只看返回值不够**——断连后 `Exec` 的失败是静默的（仅记日志），`Connect` 恒真也能"通过"。SQLite 与 DuckDB 两个后端各跑一次 |

### 13.3 验证

`cmake --build out/build/x64-Debug --target UnitTests TestDB` 退出码 0；`UnitTests.exe` →
**11 用例 / 31 断言全过、退出码 0**（改前 9 / 26）；`TestDB.exe` 退出码 0，日志
`TestReconnect Sqlite: firstConnect=1 reconnect=1 rowReadBack=1` 与
`TestReconnect Duckdb: firstConnect=1 reconnect=1 rowReadBack=1` 各一行。§13.5 的返工后用同样
命令复跑，结论不变（守卫生效路径在本套用例里不被触发）。

### 13.4 风险

属**公开行为变更**（`Connect()` 从"查询状态"变为"确保可用"），用户已书面确认无外部使用者、
未正式发布。其余：① 析构新增 `Stop()` / `Join()` 与 `DropPendingOperates()`，均为幂等调用，
但改变了"析构时写线程是否已停"的时序假设——由此析构期间的并发入队会随 `dbOperates_` 一并漏掉，
已记入 `docs/connection-lifecycle.md` §六；② `MysqlWrapper::DisConnect()` 新增 `impl_.reset()`
把原先不起作用的 try/catch 变成活路径，同时让六个操作入口的空解引用窗口首次可达（已加守卫，
见 §13.5）；③ 各 `Impl` 新增一份 `dbName` 字符串副本（每实例一条，与连接本身的开销相比可忽略）。

### 13.5 批后审查与返工

| # | 审查发现 | 处置 |
| :--- | :--- | :--- |
| 1 | 高：`MysqlWrapper` 六处 `impl_->session` 解引用无空守卫。`DisConnect()` 新增 `impl_.reset()` 前，`impl_` 只要非空就非空到底；新增后这条路径首次可达（并发 `DisConnect()`、或 `DisConnect()` 后直接调 `Exec()`），且它是**静默崩溃**而非静默丢操作 | 加私有 `bool CheckSessionOpen(const char*)`，六个入口（`Exec`/`Insert`/`Update`/`Delete`/`SelectAll`/`SelectWithSql`）各一行守卫，命中时记错误日志并跳过。六者均返回 `void`，故守卫是 `return;` |
| 2 | 中：`DuckdbWrapper::OpenOrReportFailure()` 在 `connection` 缺失时会重开 `database`，泄漏旧句柄（重连路径每失败一次漏一个） | 两个句柄各自独立判空（`database == nullptr && duckdb_open(...)`），`database` 已开好时只补 `connection`——**这一处依赖短路求值，不能合并成一次判空** |
| 3 | 中：`MariadbWrapper` 六处 `impl_->m_DBConnection` 解引用同样无守卫，与 `MysqlWrapper` 同形；本批已改其 `Connect()`，留此缺口与「四家统一」的目标相悖 | 同 1 加 `CheckConnectionOpen()`，六个入口各一行守卫 |
| 4 | 中：§13.2 把「断连后写线程自行重连」标为"改前必失败" | 该用例基于 `RecordingDb` 替身，改前即通过，已改写为"回归护栏"，只有「析构抽干待办队列」与 `TestReconnect` 是改前必失败 |
| 5 | 中：并发 `DisConnect()` 丢操作的窗口只隐含在析构条目里 | 已补为 `docs/connection-lifecycle.md` §六第 5 条 |
| 6 | 低：`SqliteWrapper.cpp` 的 796 行 diff 大部分是上一批未提交的缩进收敛（`git diff -w` 仅 17 增 7 删） | 无需处置 |
| 7 | 低：两个守卫函数未加 `const`（只读 `impl_`） | 头文件与实现同步加 `const` |
| 8 | 低：`BatchInsert` 无整批守卫，空句柄时按条数刷 N+2 条日志（MySQL / MariaDB 的 `WriteLog` 无节流） | 两家 `BatchInsert` 各加一次整批守卫，命中时整批只留一条日志 |
| 9 | 低：`DuckdbWrapper::SelectWithSqlVectorized` 是本文件七处入口里唯一没判空的一处（专有 API，不经 `Db` 接口，`AsyncDbWriter` 走不到） | 补空守卫，失败时返回 `"database is not open"`（该方法以空串表示成功） |

第 1 / 3 条的守卫**只在句柄为空时触发**，而句柄为空只发生在 `DisConnect()` 之后或建连失败之后；
连接「虽已死但句柄仍在」的情形仍走原有的异常通道（MySQL/MariaDB 抛，SQLite/DuckDB 记日志），
写线程的重连触发条件不变。返工后 `UnitTests`（11 用例 / 31 断言）与 `TestDB`（含两条
`TestReconnect`）复跑全过，见 §13.3。

**覆盖缺口**：新加守卫的两家是 MySQL / MariaDB，二者都需要可连的服务端，本机没有，故**守卫路径
无自动化覆盖**（`UnitTests` 用的是 `RecordingDb` 替身，不经真 wrapper；`TestBackendLoader` 里
MySQL 构造即如期失败）。SQLite / DuckDB 写路径的同形守卫早在本批之前就有，`TestReconnect` 只覆盖
「断开 → 重连 → 真实读写」，不覆盖「断开后仍调操作」这条被守卫拦下的分支；本批新加守卫的
`SelectWithSqlVectorized`（DuckDB 专有）同样没有用例触及。
