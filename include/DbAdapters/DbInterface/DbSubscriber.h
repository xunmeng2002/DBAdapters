#pragma once


namespace DbAdapters
{
class DbSubscriber
{
public:
	virtual void OnDbConnected() = 0;
	virtual void OnDbDisConnected() = 0;
};
}
