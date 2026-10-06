#pragma once
#include <DbAdapters/AsyncDbWriter/AsyncDbWriterExport.h>
#include <DbAdapters/DbInterface/MdbSubscriber.h>
#include <DbAdapters/DbInterface/DbSubscriber.h>
#include <DbAdapters/DbInterface/Db.h>
#include <DbAdapters/DbInterface/FailureLogThrottle.h>
#include <DbAdapters/DbInterface/RecordHandle.h>
#include <DbAdapters/DbInterface/SchemaRegistry.h>
#include <Spark/Core/Core.h>
#include <list>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace DbAdapters
{
class DbOperate;

class ASYNCDBWRITER_EXPORTS AsyncDbWriter : public Spark::Core::ThreadBase, public MdbSubscriber
{
public:
	AsyncDbWriter(Db* db, SchemaRegistry* schemaRegistry);
	~AsyncDbWriter();
	void Subscribe(DbSubscriber* dbSubscriber);
	Db* GetDb();
	bool Connect();
	void DisConnect();

	virtual void OnTableOp(DbOperateType op) override;
	virtual void OnRecordInsert(unsigned int tableID, RecordHandle record) override;
	virtual void OnRecordBatchInsert(unsigned int tableID, std::vector<RecordHandle> records) override;
	virtual void OnRecordErase(unsigned int tableID, RecordHandle record) override;
	virtual void OnRecordEraseByIndex(unsigned int tableID, unsigned int indexId, RecordHandle record) override;
	virtual void OnRecordUpdate(unsigned int tableID, RecordHandle record) override;
	virtual void OnRecordTruncate(unsigned int tableID) override;


protected:
	virtual void Run() override;
	virtual void ThreadExit() override;
	void CheckConnect();
	void CheckDbOperate();
	void HandleDbOperate();
	DbOperate* GetDbOperate();

private:
	void AddDbOperate(DbOperate* dbOperate);
	DbOperate* CreateDbOperate(DbOperateType operate, unsigned int tableId, RecordHandle record = RecordHandle(), unsigned int indexId = 0);
	void EnqueueDbOperate(DbOperateType operate, unsigned int tableId, RecordHandle record = RecordHandle(), unsigned int indexId = 0);
	int PendingOperateCount();
	void DropPendingOperates(const char* triggerReason);

	void ExecuteDbOperate(DbOperate* dbOperate);
	void CreateTables(DbOperate* dbOperate);
	void DropTables(DbOperate* dbOperate);
	void TruncateTables(DbOperate* dbOperate);
	void InsertRecord(DbOperate* dbOperate);
	void DeleteRecord(DbOperate* dbOperate);
	void DeleteRecordByIndex(DbOperate* dbOperate);
	void UpdateRecord(DbOperate* dbOperate);
	void BatchInsertRecords(DbOperate* dbOperate);
	void TruncateTable(DbOperate* dbOperate);

	// 连接失败按固定重试次数节流上报：每轮 Run 都会重试一次连接，不节流会按 timeOut_ 频率刷屏
	static constexpr int connectFailureReportInterval_ = 100;
	Db* db_;
	SchemaRegistry* schemaRegistry_;
	DbSubscriber* dbSubscriber_;
	std::list<DbOperate*> dbOperates_;
	std::mutex mutex_;
	std::condition_variable conditionVariable_;
	FailureLogThrottle connectFailureLogThrottle_{ connectFailureReportInterval_ };
};
}
