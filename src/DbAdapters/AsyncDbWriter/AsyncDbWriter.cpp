#include <DbAdapters/AsyncDbWriter/AsyncDbWriter.h>
#include "DbOperateImpl.h"
#include <Spark/Core/Logger/Logger.h>
#include <cstring>
#include <vector>

using namespace std;
using namespace Spark::Core;

namespace DbAdapters
{
AsyncDbWriter::AsyncDbWriter(Db* db, SchemaRegistry* schemaRegistry)
	:ThreadBase("AsyncDbWriter"), m_Db(db), schemaRegistry_(schemaRegistry), dbSubscriber_(nullptr)
{
}
AsyncDbWriter::~AsyncDbWriter()
{
	if (m_Db != nullptr)
	{
		delete m_Db;
		m_Db = nullptr;
	}
}
void AsyncDbWriter::Subscribe(DbSubscriber* dbSubscriber)
{
	dbSubscriber_ = dbSubscriber;
}
Db* AsyncDbWriter::GetDb()
{
	return m_Db;
}
bool AsyncDbWriter::Connect()
{
	try
	{
		if (m_Db->Connect())
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
		dbSubscriber_->OnDbDisConnected();
	}
	m_Db->DisConnect();
	lock_guard<mutex> guard(mutex_);
	const size_t pendingCount = dbOperates_.size();
	for (auto item : dbOperates_)
	{
		item->DeallocateRecord();
		item->Deallocate();
	}
	dbOperates_.clear();
	if (pendingCount > 0)
	{
		// 这里会连同队列一起丢弃，不记录的话丢数据只能从"库里的行数比预期少"反推
		WriteLog(LogLevel::Warning, "AsyncDbWriter: DisConnect discarded %d pending operations.", (int)pendingCount);
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
	CheckConnect();
	CheckDbOperate();
	HandleDbOperate();
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
	// 未连接期间 HandleDbOperate 每轮都整队丢弃（见其入口分支），此处按固定尝试次数节流上报积压规模
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
	try
	{
		while ((dbOperate = GetDbOperate()) != nullptr)
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
			dbOperate->Deallocate();
		}
	}
	catch(const exception& e)
	{
		const unsigned int failedTableID = dbOperate != nullptr ? dbOperate->TableId : 0;
		const int failedOperateType = dbOperate != nullptr ? static_cast<int>(dbOperate->Operate) : -1;
		const TableSchema* failedSchema = schemaRegistry_ != nullptr ? schemaRegistry_->GetSchema(failedTableID) : nullptr;
		WriteLog(LogLevel::Error, "AsyncDbWriter: HandleDbOperate failed. TableId:0x%X, Table:%s, Operate:%d, Message:%s",
			failedTableID, failedSchema != nullptr ? failedSchema->tableName : "unknown", failedOperateType, e.what());
		// DisConnect 会连同队列一起丢弃并上报丢弃条数
		DisConnect();
		if (dbOperate != nullptr)
		{
			dbOperate->DeallocateRecord();
			dbOperate->Deallocate();
		}
		this_thread::sleep_for(chrono::seconds(5));
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
	m_Db->CreateTables(schemaRegistry_->GetAllSchemas(), schemaRegistry_->GetTableCount());
}
void AsyncDbWriter::DropTables(DbOperate* dbOperate)
{
	m_Db->DropTables(schemaRegistry_->GetAllSchemas(), schemaRegistry_->GetTableCount());
}
void AsyncDbWriter::TruncateTables(DbOperate* dbOperate)
{
	m_Db->TruncateTables(schemaRegistry_->GetAllSchemas(), schemaRegistry_->GetTableCount());
}
void AsyncDbWriter::InsertRecord(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (schema)
	{
		m_Db->Insert(schema, dbOperate->Record);
	}
}
void AsyncDbWriter::BatchInsertRecords(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (!schema) return;

	auto& batch = static_cast<DbOperateImpl*>(dbOperate)->GetBatchData();
	if (!batch.empty())
	{
		m_Db->BatchInsert(schema, batch.data(), static_cast<int>(batch.size()));
	}
}
void AsyncDbWriter::DeleteRecord(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (schema)
	{
		m_Db->Delete(schema, dbOperate->Record, schema->primaryKeyIndices, schema->primaryKeyCount);
		schema->DeallocateRecord(dbOperate->Record);
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
			m_Db->Delete(schema, dbOperate->Record,
			             schema->secondaryIndices[i].fieldIndices,
			             schema->secondaryIndices[i].fieldCount);
			schema->DeallocateRecord(dbOperate->Record);
			return;
		}
	}
	WriteLog(LogLevel::Error, "Incorrect TableId/IndexId for DeleteRecordByIndex. TableId:0x%X, IndexId:%d", dbOperate->TableId, dbOperate->IndexId);
	schema->DeallocateRecord(dbOperate->Record);
}
void AsyncDbWriter::UpdateRecord(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (schema)
	{
		m_Db->Update(schema, dbOperate->Record);
		schema->DeallocateRecord(dbOperate->Record);
	}
}
void AsyncDbWriter::TruncateTable(DbOperate* dbOperate)
{
	const TableSchema* schema = schemaRegistry_->GetSchema(dbOperate->TableId);
	if (schema)
	{
		m_Db->TruncateTable(schema->tableName);
	}
}
}
