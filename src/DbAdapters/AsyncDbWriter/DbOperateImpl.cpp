#include "DbOperateImpl.h"

#include <Spark/TemplateLib/TemplateLib.h>

using namespace Spark;

namespace DbAdapters
{
DbOperateImpl* DbOperateImpl::Allocate()
{
	return ObjectPool<DbOperateImpl>::GetInstance().Allocate();
}
void DbOperateImpl::Deallocate()
{
	ObjectPool<DbOperateImpl>::GetInstance().Deallocate(this);
}
}
