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

## 9. 本次未改、留待裁定的相邻问题

`Table::BatchInsert` 的**未订阅分支**（`mdbSubscriber_ == nullptr || !DbInited`）里，调用方原始记录
只被 `delete records` 释放了容器、元素本身仍无人归还。这与本次修掉的是同一族缺陷，但方案 §4.4 明确
把未订阅分支列为"不变"，故未动。若要一并修，做法是把 `AdoptRecord` 的包装提到 `if` 之前，两条路
都交给句柄析构，代价是该分支下调用方原始记录也会被释放（现有三处调用点 `ThostFtdcTraderSpiImpl.cpp`
交出后均不再使用，已核对）。**待用户裁定。**

### 9.1 批后审查提出、按 §3 上报未改的三条

| # | 问题 | 当前是否可达 | 改动代价 |
| :--- | :--- | :--- | :--- |
| 1 | `~AsyncDbWriter()` 不抽干待办队列：`DropPendingOperates()` 只在 `DisConnect()` 里调用，析构时队列里的 `DbOperate` 连同其 `RecordHandle` 持有的记录一并泄漏。**非本批引入**（旧代码同样不抽干，只漏操作对象），但重构后同一条路径多漏一份记录 | 是（仍有待办时析构即触发） | 析构里补一次 `DropPendingOperates()`，或先 `Stop()` / `Join()`；牵动"析构时写线程是否已停"的时序假设 |
| 2 | `DbOperate` 有纯虚 `Deallocate()` 却无虚析构（`rules/cpp-style.md` §6 强制项，既有） | 否——全程经 `ObjectPool<DbOperateImpl>` 按具体类型归还，仓内无 `delete DbOperate*` | 补虚析构会改 `DbOperate` 布局与虚表，属导出符号的 ABI 断裂，须三仓 + 安装树同步 |
| 3 | `RecordHandle::Reset()` 未对 `record_` 判空：`AdoptRecord(nullptr)` 会以空指针调用 `record->Deallocate()` | 否——三处工厂实现为 `ObjectPool::Deallocate`（自带空指针早退）与 `delete`（空指针安全） | 一行判空；但会掩盖调用方传入空记录的错误，需先定"空记录算不算合法入参" |
| 4 | `RecordHandle::Get()` 返回可变 `void*`，`rules/cpp-style.md` §7 建议 getter 返回 const | 否——5 处调用点全部把结果交给 `const void*` 形参（`Db::Insert`/`Delete`/`Update`/`BatchInsert`），无一处需要可写指针 | 改成 `const void* Get() const noexcept` 即收紧契约。**属公开头文件签名，按 §3 上报未改**；趁首次提交前改掉最省事，一旦提交、下游跟进后就变成真正的 API 变更 |
