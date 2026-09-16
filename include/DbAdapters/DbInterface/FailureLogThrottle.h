#pragma once
#include <Spark/Core/Logger/Logger.h>

namespace DbAdapters
{
    // 逐记录写操作（Insert/Update/Delete）失败时会连续重复上万次（BatchInsert 内每条记录一次），
    // 因此失败上报必须节流：首次上报完整详情，其后每 reportInterval 次上报一次并附带累计失败次数，
    // 保证长时间故障期间故障量仍可见，又不会把日志刷满。
    class FailureLogThrottle
    {
    public:
        static constexpr int kDefaultReportInterval = 1000;

        explicit FailureLogThrottle(int reportInterval = kDefaultReportInterval)
            : reportInterval_(reportInterval > 0 ? reportInterval : kDefaultReportInterval)
        {
        }
        FailureLogThrottle(const FailureLogThrottle&) = delete;
        FailureLogThrottle& operator=(const FailureLogThrottle&) = delete;

        // 返回值：0 表示本次节流不上报；>0 表示应上报，其值为累计失败次数
        int RegisterFailure()
        {
            ++failureCount_;
            if (failureCount_ != 1 && failureCount_ % reportInterval_ != 0)
            {
                return 0;
            }
            return failureCount_;
        }
        int FailureCount() const { return failureCount_; }

    private:
        int reportInterval_;
        int failureCount_ = 0;
    };

    // 上报一次语句失败：含后端名、操作名、表名与后端错误文本，累计失败次数由本函数追加。
    // errorDetail 为后端原生错误描述，如 sqlite3_errmsg、duckdb_result_error
    inline void LogOperationFailure(FailureLogThrottle& throttle, const char* wrapperName, const char* operationName,
                                    const char* tableName, const char* errorDetail)
    {
        const int failureCount = throttle.RegisterFailure();
        if (failureCount == 0)
        {
            return;
        }
        WriteLog(Spark::Core::LogLevel::Error,
            "%s: %s failed. Table:%s, Error:%s, FailureCount:%d",
            wrapperName, operationName, tableName, errorDetail, failureCount);
    }
}
