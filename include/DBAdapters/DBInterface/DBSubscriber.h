#pragma once


namespace dbadapters
{
class DBSubscriber
{
public:
	virtual void OnDBConnected() = 0;
	virtual void OnDBDisConnected() = 0;
};
}
