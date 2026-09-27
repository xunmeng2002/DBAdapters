#pragma once

#include <DbAdapters/DbInterface/Db.h>
#include <Spark/Types.h>

#include <string>

namespace DbAdapters
{
    // 低层入口: 调用方自己给模块基名 (不含平台前后缀与调试后缀 —— 那两样由实现统一拼).
    // 留它是为了能构造"模块根本不存在"这一情形, 正常调用请用下面按后端种类的那条.
    // 装载失败抛 std::runtime_error, 文案含尝试过的路径与平台错误原文.
    Db* LoadDatabaseBackend(const std::string& backendModuleBaseName, const std::string& connectionTarget, const std::string& userName, const std::string& password);

    // 常路入口: 按后端种类装载. 种类取自 Spark 的 DbTypeType —— 它就是引擎配置里 DbType 那个
    // 字面量的来源 (值同 "0".."3"), 故本库不再自备一套枚举, 免得同一件事出现第三种词汇.
    // 模块基名与调试后缀都由本库自己拼 —— 消费方不需要知道 DbAdapters 装出来的模块叫什么、
    // 调试构装的后缀是什么.
    Db* LoadDatabaseBackend(DbTypeType dbType, const std::string& connectionTarget, const std::string& userName, const std::string& password);
}
