#include <DbAdapters/AsyncDbWriter/AsyncDbWriter.h>
#include "DbOperateImpl.h"
#include <Spark/Core/Logger/Logger.h>
#include <cstring>
#include <memory>
#include <vector>

using namespace std;
using namespace Spark::Core;

namespace
{
void ReleaseDbOperate(DbAdapters::DbOperate* dbOperate) noexcept
{
	if (dbOperate == nullptr)
	{
		return;
	}
	try
	{
		dbOperate->DeallocateRecord();
	}
	catch (...)
	{
		WriteLog(LogLevel::Error, "AsyncDbWriter: DeallocateRecord failed. TableId:0x%X", dbOperate->TableId);
	}
	dbOperate->Deallocate();
}

using DbOperateHandle = unique_ptr<DbAdapters::DbOperate, decltype(&ReleaseDbOperate)>;
}

namespace DbAdapters
{
AsyncDbWriter::AsyncDbWriter(Db* db, SchemaRegistry* schemaRegistry)
	:ThreadBase("AsyncDbWriter"), db_(db), schemaRegistry_(schemaRegistry), dbSubscriber_(nullptr)
{
}
AsyncDbWriter::~AsyncDbWriter()
{
	if (db_ != nullptr)
	{
		delete db_;
		db_ = nullptr;
	}
}
void AsyncDbWriter::Subscribe(DbSubscriber* dbSubscriber)
{
	dbSubscriber_ = dbSubscriber;
}
Db* AsyncDbWriter::GetDb()
{
	return db_;
}
bool AsyncDbWriter::Connect()
{
	try
	{
		if (db_->Connect())
		{
			connected_ = true;
			WriteLog(LogLevel::Info, "AsyncDbWriter: Db connected.");
			if (dbSubscriber_ != nullptr)
			{
				dbSubscriber_->OnDbConnected();
			}
			return true;
		}
	}
	catch (const std::exception& e)
	{
		// Connect 内的异常若逃出 Run() 会穿过 ThreadBase::ThreadFunc（无 catch）直达 std::terminate，故在此转为连接失败
		WriteLog(LogLevel::Error, "AsyncDbWriter: Connect throw. Message:%s", e.what());
		return false;
	}
	return false;
}
void AsyncDbWriter::DisConnect()
{
	connected_ = false;
	if (dbSubscriber_ != nullptr)
	{
		try
		{
			dbSubscriber_->OnDbDisConnected();
		}
		catch (const exception& e)
		{
			WriteLog(LogLevel::Error, "AsyncDbWriter: OnDbDisConnected throw. Message:%s", e.what());
		}
	}
	DropPendingOperates();
	try
	{
		db_->DisConnect();
	}
	catch (const exception& e)
	{
		WriteLog(LogLevel::Error, "AsyncDbWriter: Db DisConnect throw. Message:%s", e.what());
	}
}
void AsyncDbWriter::DropPendingOperates()
{
	list<DbOperate*> droppedOperates;
	{
		lock_guard<mutex> guard(mutex_);
		droppedOperates.swap(dbOperates_);
	}
	const size_t droppedCount = droppedOperates.size();
	for (DbOperate* item : droppedOperates)
	{
		ReleaseDbOperate(item);
	}
	if (droppedCount > 0)
	{
		// 这里会连同队列一起丢弃，不记录的话丢数据只能从"库里的行数比预期少"反推
		WriteLog(LogLevel::Warning, "AsyncDbWriter: DisConnect discarded %d pending operations.", (int)droppedCount);
	}
}

// ---- Generic MdbSubscriber overrides ----

void AsyncDbWriter::OnTableOp(DbOperateType op)
{
	DbOperate* dbOperate = AllocateDbOperate();
	dbOperate->Operate = op;
	dbOperate->TableId = 0;
	dbOperate->Record = nullptr;
	AddDbOperate(dbOperate);
}

void AsyncDbWriter::OnRecordInsert(unsigned int tableID, void* record)
{
	DbOperate* dbOperate = AllocateDbOperate();
	dbOperate->Operate = DbOperateType::Insert;
	dbOperate->TableId = tableID;
	dbOperate->Record = record;
	AddDbOperate(dbOperate);
}

void AsyncDbWriter::OnRecordBatchInsert(unsigned int tableID, std::vector<const void*>* records)
{
	DbOperate* dbOperate = AllocateDbOperate();
	dbOperate->Operate = DbOperateType::BatchInsert;
	dbOperate->TableId = tableID;
	dbOperate->Record = nullptr;

	auto& batch = static_cast<DbOperateImpl*>(dbOperate)->GetBatchData();
	batch.swap(*records);
	delete records;
	AddDbOperate(dbOperate);
}

void AsyncDbWriter::OnRecordErase(unsigned int tableID, void* record)
{
	DbOperate* dbOperate = AllocateDbOperate();
	dbOperate->Operate = DbOperateType::Delete;
	dbOperate->TableId = tableID;
	dbOperate->Record = record;
	AddDbOperate(dbOperate);
}

void AsyncDbWriter::OnRecordEraseByIndex(unsigned int tableID, unsigned int indexId, void* record)
{
	DbOperate* dbOperate = AllocateDbOperate();
	dbOperate->Operate = DbOperateType::DeleteByIndex;
	dbOperate->TableId = tableID;
	dbOperate->IndexId = indexId;
	dbOperate->Record = record;
	AddDbOperate(dbOperate);
}

void AsyncDbWriter::OnRecordUpdate(unsigned int tableID, void* record)
{
	DbOperate* dbOperate = AllocateDbOperate();
	dbOperate->Operate = DbOperateType::Update;
	dbOperate->TableId = tableID;
	dbOperate->Record = record;
	AddDbOperate(dbOperate);
}

void AsyncDbWriter::OnRecordTruncate(unsigned int tableID)
{
	DbOperate* dbOperate = AllocateDbOperate();
	dbOperate->Operate = DbOperateType::Truncate;
	dbOperate->TableId = tableID;
	dbOperate->Record = nullptr;
	AddDbOperate(dbOperate);
}


void AsyncDbWriter::Run()
{
	try
	{
		CheckConnect();
		CheckDbOperate();
		HandleDbOperate();
	}
	catch (const exception& e)
	{
		WriteLog(LogLevel::Error, "AsyncDbWriter: unexpected exception escaped, retry after backoff. Message:%s", e.what());
		this_thread::sleep_for(chrono::seconds(1));
	}
	catch (...)
	{
		WriteLog(LogLevel::Error, "AsyncDbWriter: unknown exception escaped, retry after backoff.");
		this_thread::sleep_for(chrono::seconds(1));
	}
}
void AsyncDbWriter::CheckConnect()
{
	if (connected_)
	{
		return;
	}
	if (Connect())
	{
		return;
	}
	const int connectFailureCount = connectFailureLogThrottle_.RegisterFailure();
	if (connectFailureCount > 0)
	{
		WriteLog(LogLevel::Error, "AsyncDbWriter: Db connect failed, %d operations pending and not written. ConnectFailureCount:%d",
			PendingOperateCount(), connectFailureCount);
	}
}
void AsyncDbWriter::CheckDbOperate()
{
	unique_lock<mutex> guard(mutex_);
	conditionVariable_.wait_for(guard, timeOut_, [&] {return !dbOperates_.empty(); });
}
void AsyncDbWriter::ThreadExit()
{
	ThreadBase::ThreadExit();
	const int pendingCount = PendingOperateCount();
	if (pendingCount > 0)
	{
		// 队列非空即退出 = 这些操作从未提交给后端，是"跑完没有落库"这件事在日志里唯一的痕迹
		WriteLog(LogLevel::Error, "AsyncDbWriter: Exit with %d operations never written, database was not available.", pendingCount);
	}
}
void AsyncDbWriter::HandleDbOperate()
{
	if (!connected_)
	{
		// 未连接时整队保留（不抽干、不释放），积压规模与原因由 CheckConnect 同轮上报
		return;
	}
	DbOperate* dbOperate = nullptr;
	while ((dbOperate = GetDbOperate()) != nullptr)
	{
		DbOperateHandle dbOperateHandle(dbOperate, &ReleaseDbOperate);
		try
		{
			ExecuteDbOperate(dbOperate);
		}
		catch (const exception& e)
		{
			const TableSchema* failedSchema = schemaRegistry_ != nullptr ? schemaRegistry_->GetSchema(dbOperate->TableId) : nullptr;
			WriteLog(LogLevel::Error, "AsyncDbWriter: HandleDbOperate failed. TableId:0x%X, Table:%s, Operate:%d, Message:%s",
				dbOperate->TableId, failedSchema != nullptr ? failedSchema->tableName : "unknown", static_cast<int>(dbOperate->Operate), e.what());
			DisConnect();
			this_thread::sleep_for(chrono::seconds(5));
			return;
		}
	}
}
void AsyncDbWriter::ExecuteDbOperate(DbOperate* dbOperate)
{
	switch (dbOperate->Operate)
	{
	case DbOperateType::CreateTables:		CreateTables(dbOperate); break;
	case DbOperateType::DropTables:			DropTables(dbOperate); break;
	case DbOperateType::TruncateTables:		TruncateTables(dbOperate); break;
	case DbOperateType::Insert:				InsertRecord(dbOperate); break;
	case DbOperateType::Delete:				DeleteRecord(dbOperate); break;
	case DbOperateType::DeleteByIndex:		DeleteRecordByIndex(dbOperate); break;
	case DbOperateType::Update:				UpdateRecord(dbOperate); break;
	case DbOperateType::BatchInsert:		BatchInsertRecords(dbOperate); break;
	case DbOperateType::Truncate:			TruncateTable(dbOperate); break;
	default:
		WriteLog(LogLevel::Warning, "Unknown DbOperateType:%d", dbOperate->Operate);
		break;
	}
}
DbOperate* AsyncDbWriter::GetDbOperate()
{
	lock_guard<mutex> guard(mutex_);
	if (dbOperates_.empty())
	{
		return nullptr;
	}
	auto item = dbOperates_.front();
	dbOperates_.pop_front();
	return item;
}
int AsyncDbWriter::PendingOperateCount()
{
	lock_guard<mutex> guard(mutex_);
	return static_cast<int>(dbOperates_.size());
}


void AsyncDbWriter::AddDbOperate(DbOperate* dbOperate)
{
	{
		lock_guard<mutex> guard(mutex_);
		dbOperates_.push_back(dbOperate);
	}
	conditionVariable_.notify_one();
}

DbOperate* AsyncDbWriter::AllocateDbOperate()
{
	DbOperate* op = DbOperate::Allocate();
	static_cast<DbOperateImpl*>(op)->SetSchemaRegistry(schemaRegistry_);
	return op;
}


void AsyncDbWriter::CreateTables(DbOperate* dbOperate)
{
	db_->CreateTables(schemaRegistry_->GetAllSchemas(), schemaRegistry_->GetTableCount());
}
void AsyncDbWriter::DropTables(DbOperate* dbOperate)
{
	db_->DropTables(schemaRegistry_->GetAllSchemas(), schemaRegistry_->GetTableCount());
}
void AsyncDbWriter::TruncateTables(DbOperate* dbOperate)
{
	db_->TruncateTables(schemaRegistry_->GetAllSchemas(), schemaRegistry_->GetTableCount());
}
void AsyncDbWriter::InsertRecord(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (schema)
	{
		db_->Insert(schema, dbOperate->Record);
	}
}
void AsyncDbWriter::BatchInsertRecords(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (!schema) return;

	auto& batch = static_cast<DbOperateImpl*>(dbOperate)->GetBatchData();
	if (!batch.empty())
	{
		db_->BatchInsert(schema, batch.data(), static_cast<int>(batch.size()));
	}
}
void AsyncDbWriter::DeleteRecord(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (schema)
	{
		db_->Delete(schema, dbOperate->Record, schema->primaryKeyIndices, schema->primaryKeyCount);
	}
}

void AsyncDbWriter::DeleteRecordByIndex(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (!schema) return;

	for (int i = 0; i < schema->secondaryIndexCount; ++i)
	{
		if (schema->secondaryIndices[i].indexId == dbOperate->IndexId)
		{
			db_->Delete(schema, dbOperate->Record,
			             schema->secondaryIndices[i].fieldIndices,
			             schema->secondaryIndices[i].fieldCount);
			return;
		}
	}
	WriteLog(LogLevel::Error, "Incorrect TableId/IndexId for DeleteRecordByIndex. TableId:0x%X, IndexId:%d", dbOperate->TableId, dbOperate->IndexId);
}
void AsyncDbWriter::UpdateRecord(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (schema)
	{
		db_->Update(schema, dbOperate->Record);
	}
}
void AsyncDbWriter::TruncateTable(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (schema)
	{
		db_->TruncateTable(schema->tableName);
	}
}
}
