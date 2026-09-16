#include "DBOperateImpl.h"
#include <Spark/TemplateLib/TemplateLib.h>
#include <cstring>

using namespace Spark;

namespace DbAdapters
{
DbOperate* DbOperate::Allocate()
{
	return ObjectPool<DBOperateImpl>::GetInstance().Allocate();
}
void DBOperateImpl::Deallocate()
{
	batch_data_.clear();
	ObjectPool<DBOperateImpl>::GetInstance().Deallocate(this);
}
void DBOperateImpl::DeallocateRecord()
{
	if (Operate == DbOperateType::Insert || Operate == DbOperateType::BatchInsert || Operate == DbOperateType::Truncate)
	{
		return;
	}
	if (schema_registry_)
	{
		const TableSchema* schema = schema_registry_->GetSchema(TableId);
		if (schema && schema->DeallocateRecord && Record)
		{
			schema->DeallocateRecord(Record);
		}
	}
	Record = nullptr;
}
}
