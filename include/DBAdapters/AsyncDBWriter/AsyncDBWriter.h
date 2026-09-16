#pragma once
#include <DBAdapters/AsyncDBWriter/AsyncDBWriterExport.h>
#include <DBAdapters/DBInterface/MdbSubscriber.h>
#include <DBAdapters/DBInterface/DBSubscriber.h>
#include <DBAdapters/DBInterface/DB.h>
#include <DBAdapters/DBInterface/FailureLogThrottle.h>
#include <DBAdapters/DBInterface/SchemaRegistry.h>
#include <Spark/TemplateLib/TemplateLib.h>
#include <Spark/Core/Core.h>
#include <list>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace DbAdapters
{
class ASYNCDBWRITER_EXPORTS AsyncDBWriter : public Spark::Core::ThreadBase, public MdbSubscriber
{
public:
	AsyncDBWriter(DB* db, SchemaRegistry* schemaRegistry);
	~AsyncDBWriter();
	void Subscribe(DBSubscriber* dbSubscriber);
	DB* GetDB();
	bool Connect();
	void DisConnect();

	virtual void OnTableOp(DbOperateType op) override;
	virtual void OnRecordInsert(unsigned int tableID, void* record) override;
	virtual void OnRecordBatchInsert(unsigned int tableID, std::vector<const void*>* records) override;
	virtual void OnRecordErase(unsigned int tableID, void* record) override;
	virtual void OnRecordEraseByIndex(unsigned int tableID, unsigned int indexID, void* record) override;
	virtual void OnRecordUpdate(unsigned int tableID, void* record) override;
	virtual void OnRecordTruncate(unsigned int tableID) override;


protected:
	virtual void Run() override;
	virtual void ThreadExit() override;
	void CheckConnect();
	void CheckDBOperate();
	void HandleDBOperate();
	DbOperate* GetDBOperate();

private:
	DbOperate* AllocateDBOperate();
	void AddDBOperate(DbOperate* dbOperate);
	int PendingOperateCount();

	void CreateTables(DbOperate* dbOperate);
	void DropTables(DbOperate* dbOperate);
	void TruncateTables(DbOperate* dbOperate);
	void InsertRecord(DbOperate* dbOperate);
	void DeleteRecord(DbOperate* dbOperate);
	void DeleteRecordByIndex(DbOperate* dbOperate);
	void UpdateRecord(DbOperate* dbOperate);
	void BatchInsertRecords(DbOperate* dbOperate);
	void TruncateTable(DbOperate* dbOperate);

private:
	// 连接失败按固定重试次数节流上报：每轮 Run 都会重试一次连接，不节流会按 timeOut_ 频率刷屏
	static constexpr int connectFailureReportInterval_ = 100;

	DB* m_DB;
	SchemaRegistry* schemaRegistry_;
	DBSubscriber* dbSubscriber_;
	std::list<DbOperate*> dbOperates_;
	std::mutex mutex_;
	std::condition_variable conditionVariable_;
	FailureLogThrottle connectFailureLogThrottle_{ connectFailureReportInterval_ };
};
}
