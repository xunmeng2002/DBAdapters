#pragma once
#include <cstddef>

namespace DbAdapters
{
//FieldType 描述的是"对内存 record 的带类型视图"，不是 Db 列宽：四个 Wrapper 都按
//(const char*)record + offset 定位，再按此类型定长读写，两个方向都是 sizeof(FieldType 对应类型) 字节。
//因此新增字段类型时必须保证描述符类型与 MdbStructs.h 里成员的声明类型宽度一致。
enum class FieldType : unsigned char
{
    Int32,
    Int64,
    Double,
    Char,
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    UInt32,
    UInt64,
};

struct FieldDescriptor
{
    const char*     name;
    FieldType       type;
    std::size_t     offset;
    std::size_t     arraySize;
};

struct RecordFactory
{
    void* (*Allocate)();
    void  (*PushBack)(void* records, void* record);
};

struct IndexDefinition
{
    unsigned int    indexId;
    const int*      fieldIndices;
    int             fieldCount;
};

struct TableSchema
{
    const char*             tableName;
    const FieldDescriptor*  fields;
    int                     fieldCount;
    const int*              primaryKeyIndices;
    int                     primaryKeyCount;
    const IndexDefinition*  secondaryIndices;
    int                     secondaryIndexCount;
};
}
