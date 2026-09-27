#pragma once

// 每个适配器模块导出的那个 C 入口的契约. 设计说明见 docs/backend-runtime-loading.md.

#include <DbAdapters/DbInterface/Db.h>

#include <cstdio>
#include <exception>
#include <string>

namespace DbAdapters
{
    // 四个适配器模块各自导出同一个符号名, 改名即破坏装载器.
    // 下划线是 cpp-style.md §1 函数名 PascalCase 的明示例外: 该符号是 C ABI 的入口, 沿用 <命名空间>_<函数>
    // 的 C 库惯例 (如 sqlite3_open), 以便在 nm / dumpbin 的输出里一眼看出归属.
    inline constexpr const char* DbBackendCreateSymbolName = "DbAdapters_CreateBackend";

    using BackendCreateFunction = Db* (*)(
        const char* connectionTarget,
        const char* userName,
        const char* password,
        char* failureText,
        int failureTextCapacity);

    // std::string 从空指针构造是未定义行为, 而调用方传进来的连接参数可能为空指针.
    inline std::string ConnectionTargetOrEmpty(const char* connectionTarget)
    {
        return connectionTarget != nullptr ? std::string(connectionTarget) : std::string();
    }

    inline void WriteBackendFailure(char* failureText, int failureTextCapacity, const char* failureReason)
    {
        if (failureText == nullptr || failureTextCapacity <= 0)
        {
            return;
        }
        std::snprintf(failureText, static_cast<size_t>(failureTextCapacity), "%s",
            failureReason != nullptr ? failureReason : "");
    }

    // 让 C++ 异常穿过 extern "C" 边界是未定义行为, 故四个适配器的 C 入口一律经此收口.
    // 参数用可调用对象而非可变参数包, 是为了让连接参数到 std::string 的转换也落在 try 之内.
    template <typename BackendConstructor>
    Db* CreateBackendOrReportFailure(char* failureText, int failureTextCapacity, BackendConstructor constructBackend)
    {
        try
        {
            return constructBackend();
        }
        catch (const std::exception& failure)
        {
            WriteBackendFailure(failureText, failureTextCapacity, failure.what());
        }
        catch (...)
        {
            WriteBackendFailure(failureText, failureTextCapacity, "非 std::exception 的异常");
        }
        return nullptr;
    }
}
