#pragma once
#include <DBAdapters/DBInterface/DbOperate.h>
#include <atomic>
#include <vector>


namespace DbAdapters
{
class MdbSubscriber
{
public:
	MdbSubscriber()
		:connected_(false)
	{
	}
	virtual ~MdbSubscriber() = default;

	virtual void OnTableOp(DbOperateType op) {}
	virtual void OnRecordInsert(unsigned int tableID, void* record) {}
	virtual void OnRecordBatchInsert(unsigned int tableID, std::vector<const void*>* records) {}
	virtual void OnRecordErase(unsigned int tableID, void* record) {}
	virtual void OnRecordEraseByIndex(unsigned int tableID, unsigned int indexId, void* record) {}
	virtual void OnRecordUpdate(unsigned int tableID, void* record) {}
	virtual void OnRecordTruncate(unsigned int tableID) {}

public:
	std::atomic<bool> connected_;
};
}
