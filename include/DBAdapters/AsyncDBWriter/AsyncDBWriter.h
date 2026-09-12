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

namespace dbadapters
{
class ASYNCDBWRITER_EXPORTS AsyncDBWriter : public spark::core::ThreadBase, public MdbSubscriber
{
public:
	AsyncDBWriter(DB* db, SchemaRegistry* schemaRegistry);
	~AsyncDBWriter();
	void Subscribe(DBSubscriber* dbSubscriber);
	DB* GetDB();
	bool Connect();
	void DisConnect();

	virtual void OnTableOp(DBOperateType op) override;
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
	DBOperate* GetDBOperate();

private:
	DBOperate* AllocateDBOperate();
	void AddDBOperate(DBOperate* dbOperate);
	int PendingOperateCount();

	void CreateTables(DBOperate* dbOperate);
	void DropTables(DBOperate* dbOperate);
	void TruncateTables(DBOperate* dbOperate);
	void InsertRecord(DBOperate* dbOperate);
	void DeleteRecord(DBOperate* dbOperate);
	void DeleteRecordByIndex(DBOperate* dbOperate);
	void UpdateRecord(DBOperate* dbOperate);
	void BatchInsertRecords(DBOperate* dbOperate);
	void TruncateTable(DBOperate* dbOperate);

private:
	// 连接失败按固定重试次数节流上报：每轮 Run 都会重试一次连接，不节流会按 m_TimeOut 频率刷屏
	static constexpr int kConnectFailureReportInterval = 100;

	DB* m_DB;
	SchemaRegistry* m_SchemaRegistry;
	DBSubscriber* m_DBSubscriber;
	std::list<DBOperate*> m_DBOperates;
	std::mutex m_Mutex;
	std::condition_variable m_ConditionVariable;
	FailureLogThrottle m_ConnectFailureLogThrottle{ kConnectFailureReportInterval };
};
}
