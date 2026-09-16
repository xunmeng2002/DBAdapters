#include "DbOperateImpl.h"
#include <Spark/TemplateLib/TemplateLib.h>
#include <cstring>

using namespace Spark;

namespace DbAdapters
{
DbOperate* DbOperate::Allocate()
{
	return ObjectPool<DbOperateImpl>::GetInstance().Allocate();
}
void DbOperateImpl::Deallocate()
{
	batch_data_.clear();
	ObjectPool<DbOperateImpl>::GetInstance().Deallocate(this);
}
void DbOperateImpl::DeallocateRecord()
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
