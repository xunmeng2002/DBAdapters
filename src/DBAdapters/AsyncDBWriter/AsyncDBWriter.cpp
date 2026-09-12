#include <DBAdapters/AsyncDBWriter/AsyncDBWriter.h>
#include "DBOperateImpl.h"
#include <Spark/Core/Logger/Logger.h>
#include <cstring>
#include <vector>

using namespace std;
using namespace spark::core;

namespace dbadapters
{
AsyncDBWriter::AsyncDBWriter(DB* db, SchemaRegistry* schemaRegistry)
	:ThreadBase("AsyncDBWriter"), m_DB(db), m_SchemaRegistry(schemaRegistry), m_DBSubscriber(nullptr)
{
}
AsyncDBWriter::~AsyncDBWriter()
{
	if (m_DB != nullptr)
	{
		delete m_DB;
		m_DB = nullptr;
	}
}
void AsyncDBWriter::Subscribe(DBSubscriber* dbSubscriber)
{
	m_DBSubscriber = dbSubscriber;
}
DB* AsyncDBWriter::GetDB()
{
	return m_DB;
}
bool AsyncDBWriter::Connect()
{
	try
	{
		if (m_DB->Connect())
		{
			m_Connected = true;
			WriteLog(LogLevel::Info, "AsyncDBWriter: DB connected.");
			if (m_DBSubscriber != nullptr)
			{
				m_DBSubscriber->OnDBConnected();
			}
			return true;
		}
	}
	catch (const std::exception& e)
	{
		// Connect 内的异常若逃出 Run() 会穿过 ThreadBase::ThreadFunc（无 catch）直达 std::terminate，故在此转为连接失败
		WriteLog(LogLevel::Error, "AsyncDBWriter: Connect throw. Message:%s", e.what());
		return false;
	}
	return false;
}
void AsyncDBWriter::DisConnect()
{
	m_Connected = false;
	if (m_DBSubscriber != nullptr)
	{
		m_DBSubscriber->OnDBDisConnected();
	}
	m_DB->DisConnect();
	lock_guard<mutex> guard(m_Mutex);
	const size_t pendingCount = m_DBOperates.size();
	for (auto item : m_DBOperates)
	{
		item->DeallocateRecord();
		item->Deallocate();
	}
	m_DBOperates.clear();
	if (pendingCount > 0)
	{
		// 这里会连同队列一起丢弃，不记录的话丢数据只能从"库里的行数比预期少"反推
		WriteLog(LogLevel::Warning, "AsyncDBWriter: DisConnect discarded %d pending operations.", (int)pendingCount);
	}
}

// ---- Generic MdbSubscriber overrides ----

void AsyncDBWriter::OnTableOp(DBOperateType op)
{
	DBOperate* dbOperate = AllocateDBOperate();
	dbOperate->Operate = op;
	dbOperate->TableID = 0;
	dbOperate->Record = nullptr;
	AddDBOperate(dbOperate);
}

void AsyncDBWriter::OnRecordInsert(unsigned int tableID, void* record)
{
	DBOperate* dbOperate = AllocateDBOperate();
	dbOperate->Operate = DBOperateType::Insert;
	dbOperate->TableID = tableID;
	dbOperate->Record = record;
	AddDBOperate(dbOperate);
}

void AsyncDBWriter::OnRecordBatchInsert(unsigned int tableID, std::vector<const void*>* records)
{
	DBOperate* dbOperate = AllocateDBOperate();
	dbOperate->Operate = DBOperateType::BatchInsert;
	dbOperate->TableID = tableID;
	dbOperate->Record = nullptr;

	auto& batch = static_cast<DBOperateImpl*>(dbOperate)->GetBatchData();
	batch.swap(*records);
	delete records;
	AddDBOperate(dbOperate);
}

void AsyncDBWriter::OnRecordErase(unsigned int tableID, void* record)
{
	DBOperate* dbOperate = AllocateDBOperate();
	dbOperate->Operate = DBOperateType::Delete;
	dbOperate->TableID = tableID;
	dbOperate->Record = record;
	AddDBOperate(dbOperate);
}

void AsyncDBWriter::OnRecordEraseByIndex(unsigned int tableID, unsigned int indexID, void* record)
{
	DBOperate* dbOperate = AllocateDBOperate();
	dbOperate->Operate = DBOperateType::DeleteByIndex;
	dbOperate->TableID = tableID;
	dbOperate->IndexID = indexID;
	dbOperate->Record = record;
	AddDBOperate(dbOperate);
}

void AsyncDBWriter::OnRecordUpdate(unsigned int tableID, void* record)
{
	DBOperate* dbOperate = AllocateDBOperate();
	dbOperate->Operate = DBOperateType::Update;
	dbOperate->TableID = tableID;
	dbOperate->Record = record;
	AddDBOperate(dbOperate);
}

void AsyncDBWriter::OnRecordTruncate(unsigned int tableID)
{
	DBOperate* dbOperate = AllocateDBOperate();
	dbOperate->Operate = DBOperateType::Truncate;
	dbOperate->TableID = tableID;
	dbOperate->Record = nullptr;
	AddDBOperate(dbOperate);
}


void AsyncDBWriter::Run()
{
	CheckConnect();
	CheckDBOperate();
	HandleDBOperate();
}
void AsyncDBWriter::CheckConnect()
{
	if (m_Connected)
	{
		return;
	}
	if (Connect())
	{
		return;
	}
	// 未连接期间 HandleDBOperate 每轮都整队丢弃（见其入口分支），此处按固定尝试次数节流上报积压规模
	const int connectFailureCount = m_ConnectFailureLogThrottle.RegisterFailure();
	if (connectFailureCount > 0)
	{
		WriteLog(LogLevel::Error, "AsyncDBWriter: DB connect failed, %d operations pending and not written. ConnectFailureCount:%d",
			PendingOperateCount(), connectFailureCount);
	}
}
void AsyncDBWriter::CheckDBOperate()
{
	unique_lock<mutex> guard(m_Mutex);
	m_ConditionVariable.wait_for(guard, m_TimeOut, [&] {return !m_DBOperates.empty(); });
}
void AsyncDBWriter::ThreadExit()
{
	ThreadBase::ThreadExit();
	const int pendingCount = PendingOperateCount();
	if (pendingCount > 0)
	{
		// 队列非空即退出 = 这些操作从未提交给后端，是"跑完没有落库"这件事在日志里唯一的痕迹
		WriteLog(LogLevel::Error, "AsyncDBWriter: Exit with %d operations never written, database was not available.", pendingCount);
	}
}
void AsyncDBWriter::HandleDBOperate()
{
	if (!m_Connected)
	{
		// 未连接时整队保留（不抽干、不释放），积压规模与原因由 CheckConnect 同轮上报
		return;
	}
	DBOperate* dbOperate = nullptr;
	try
	{
		while ((dbOperate = GetDBOperate()) != nullptr)
		{
			switch (dbOperate->Operate)
			{
			case DBOperateType::CreateTables:		CreateTables(dbOperate); break;
			case DBOperateType::DropTables:			DropTables(dbOperate); break;
			case DBOperateType::TruncateTables:		TruncateTables(dbOperate); break;
			case DBOperateType::Insert:				InsertRecord(dbOperate); break;
			case DBOperateType::Delete:				DeleteRecord(dbOperate); break;
			case DBOperateType::DeleteByIndex:		DeleteRecordByIndex(dbOperate); break;
			case DBOperateType::Update:				UpdateRecord(dbOperate); break;
			case DBOperateType::BatchInsert:		BatchInsertRecords(dbOperate); break;
			case DBOperateType::Truncate:			TruncateTable(dbOperate); break;
			default:
				WriteLog(LogLevel::Warning, "Unknown DBOperateType:%d", dbOperate->Operate);
				break;
			}
			dbOperate->Deallocate();
		}
	}
	catch(const exception& e)
	{
		const unsigned int failedTableID = dbOperate != nullptr ? dbOperate->TableID : 0;
		const int failedOperateType = dbOperate != nullptr ? static_cast<int>(dbOperate->Operate) : -1;
		const TableSchema* failedSchema = m_SchemaRegistry != nullptr ? m_SchemaRegistry->GetSchema(failedTableID) : nullptr;
		WriteLog(LogLevel::Error, "AsyncDBWriter: HandleDBOperate failed. TableID:0x%X, Table:%s, Operate:%d, Message:%s",
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
DBOperate* AsyncDBWriter::GetDBOperate()
{
	lock_guard<mutex> guard(m_Mutex);
	if (m_DBOperates.empty())
	{
		return nullptr;
	}
	auto item = m_DBOperates.front();
	m_DBOperates.pop_front();
	return item;
}
int AsyncDBWriter::PendingOperateCount()
{
	lock_guard<mutex> guard(m_Mutex);
	return static_cast<int>(m_DBOperates.size());
}


void AsyncDBWriter::AddDBOperate(DBOperate* dbOperate)
{
	{
		lock_guard<mutex> guard(m_Mutex);
		m_DBOperates.push_back(dbOperate);
	}
	m_ConditionVariable.notify_one();
}

DBOperate* AsyncDBWriter::AllocateDBOperate()
{
	DBOperate* op = DBOperate::Allocate();
	static_cast<DBOperateImpl*>(op)->SetSchemaRegistry(m_SchemaRegistry);
	return op;
}


void AsyncDBWriter::CreateTables(DBOperate* dbOperate)
{
	m_DB->CreateTables(m_SchemaRegistry->GetAllSchemas(), m_SchemaRegistry->GetTableCount());
}
void AsyncDBWriter::DropTables(DBOperate* dbOperate)
{
	m_DB->DropTables(m_SchemaRegistry->GetAllSchemas(), m_SchemaRegistry->GetTableCount());
}
void AsyncDBWriter::TruncateTables(DBOperate* dbOperate)
{
	m_DB->TruncateTables(m_SchemaRegistry->GetAllSchemas(), m_SchemaRegistry->GetTableCount());
}
void AsyncDBWriter::InsertRecord(DBOperate* dbOperate)
{
	const TableSchema* schema = m_SchemaRegistry->GetSchema(dbOperate->TableID);
	if (schema)
	{
		m_DB->Insert(schema, dbOperate->Record);
	}
}
void AsyncDBWriter::BatchInsertRecords(DBOperate* dbOperate)
{
	const TableSchema* schema = m_SchemaRegistry->GetSchema(dbOperate->TableID);
	if (!schema) return;

	auto& batch = static_cast<DBOperateImpl*>(dbOperate)->GetBatchData();
	if (!batch.empty())
	{
		m_DB->BatchInsert(schema, batch.data(), static_cast<int>(batch.size()));
	}
}
void AsyncDBWriter::DeleteRecord(DBOperate* dbOperate)
{
	const TableSchema* schema = m_SchemaRegistry->GetSchema(dbOperate->TableID);
	if (schema)
	{
		m_DB->Delete(schema, dbOperate->Record, schema->primaryKeyIndices, schema->primaryKeyCount);
		schema->DeallocateRecord(dbOperate->Record);
	}
}

void AsyncDBWriter::DeleteRecordByIndex(DBOperate* dbOperate)
{
	const TableSchema* schema = m_SchemaRegistry->GetSchema(dbOperate->TableID);
	if (!schema) return;

	for (int i = 0; i < schema->secondaryIndexCount; ++i)
	{
		if (schema->secondaryIndices[i].indexID == dbOperate->IndexID)
		{
			m_DB->Delete(schema, dbOperate->Record,
			             schema->secondaryIndices[i].fieldIndices,
			             schema->secondaryIndices[i].fieldCount);
			schema->DeallocateRecord(dbOperate->Record);
			return;
		}
	}
	WriteLog(LogLevel::Error, "Incorrect TableID/IndexID for DeleteRecordByIndex. TableID:0x%X, IndexID:%d", dbOperate->TableID, dbOperate->IndexID);
	schema->DeallocateRecord(dbOperate->Record);
}
void AsyncDBWriter::UpdateRecord(DBOperate* dbOperate)
{
	const TableSchema* schema = m_SchemaRegistry->GetSchema(dbOperate->TableID);
	if (schema)
	{
		m_DB->Update(schema, dbOperate->Record);
		schema->DeallocateRecord(dbOperate->Record);
	}
}
void AsyncDBWriter::TruncateTable(DBOperate* dbOperate)
{
	const TableSchema* schema = m_SchemaRegistry->GetSchema(dbOperate->TableID);
	if (schema)
	{
		m_DB->TruncateTable(schema->tableName);
	}
}
}
