#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dbadapters
{
//FieldType 描述的是"对内存 record 的带类型视图"，不是 DB 列宽：四个 Wrapper 都按
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
    unsigned int    indexID;
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
    void                    (*DeallocateRecord)(void*);
    const IndexDefinition*  secondaryIndices;
    int                     secondaryIndexCount;
};

//仅供四个 Wrapper 内部使用，不是公共 API：把 DB 侧读到的有符号整数写进 record 成员。
//源值超出目标类型值域时按目标类型饱和写入并返回 false（调用方据此计数并汇总告警），不会越界写。
//64 位无符号目标请用 TryWriteUInt。
template <typename TDest>
bool TryWriteInt(long long source, char* dest)
{
    static_assert(std::is_integral_v<TDest>, "TDest must be an integral type");
    static_assert(!std::is_unsigned_v<TDest> || sizeof(TDest) < sizeof(long long),
                  "16/32 位以下的无符号目标用 TryWriteInt，64 位无符号目标请用 TryWriteUInt");

    const long long sourceLowest  = static_cast<long long>(std::numeric_limits<TDest>::lowest());
    const long long sourceHighest = static_cast<long long>(std::numeric_limits<TDest>::max());
    if (source < sourceLowest)
    {
        *reinterpret_cast<TDest*>(dest) = static_cast<TDest>(sourceLowest);
        return false;
    }
    if (source > sourceHighest)
    {
        *reinterpret_cast<TDest*>(dest) = static_cast<TDest>(sourceHighest);
        return false;
    }
    *reinterpret_cast<TDest*>(dest) = static_cast<TDest>(source);
    return true;
}

//仅供四个 Wrapper 内部使用，不是公共 API：TryWriteInt 的无符号镜像。
//只有窄于 64 位的目标可能饱和；UInt64 目标的值域与源值域同宽，永不饱和。
template <typename TDest>
bool TryWriteUInt(unsigned long long source, char* dest)
{
    static_assert(std::is_unsigned_v<TDest>, "TDest must be an unsigned type");

    const unsigned long long sourceHighest =
        static_cast<unsigned long long>(std::numeric_limits<TDest>::max());
    if (source > sourceHighest)
    {
        *reinterpret_cast<TDest*>(dest) = static_cast<TDest>(sourceHighest);
        return false;
    }
    *reinterpret_cast<TDest*>(dest) = static_cast<TDest>(source);
    return true;
}

//仅供四个 Wrapper 内部使用，不是公共 API：超过 long long 值域的无符号源按 long long 最高值饱和。
inline long long SaturatingToInt64(unsigned long long source)
{
    const unsigned long long int64Highest =
        static_cast<unsigned long long>(std::numeric_limits<long long>::max());
    return source > int64Highest ? std::numeric_limits<long long>::max()
                                 : static_cast<long long>(source);
}

//仅供四个 Wrapper 内部使用，不是公共 API：负的有符号源转无符号目标时置 0 并把 clamped 置真。
inline unsigned long long SaturatingToUInt64(long long source, bool& clamped)
{
    if (source < 0)
    {
        clamped = true;
        return 0ULL;
    }
    return static_cast<unsigned long long>(source);
}

//仅供四个 Wrapper 内部使用，不是公共 API：把有符号源收窄写入 dest，返回 false 表示按目标类型饱和。
//非整数目标不走本函数。SQLite 没有无符号列类型，其读取路径只有这一个方向。
inline bool TryWriteIntegerFromSigned(long long source, FieldType targetType, char* dest)
{
    switch (targetType)
    {
    case FieldType::Int8:   return TryWriteInt<int8_t>(source, dest);
    case FieldType::UInt8:  return TryWriteInt<uint8_t>(source, dest);
    case FieldType::Int16:  return TryWriteInt<int16_t>(source, dest);
    case FieldType::UInt16: return TryWriteInt<uint16_t>(source, dest);
    case FieldType::Int32:  return TryWriteInt<int32_t>(source, dest);
    case FieldType::UInt32: return TryWriteInt<uint32_t>(source, dest);
    case FieldType::Int64:  return TryWriteInt<int64_t>(source, dest);
    case FieldType::UInt64:
    {
        bool clampedToZero = false;
        *reinterpret_cast<uint64_t*>(dest) = SaturatingToUInt64(source, clampedToZero);
        return !clampedToZero;
    }
    default:                return true;
    }
}

//仅供四个 Wrapper 内部使用，不是公共 API：把无符号源收窄写入 dest，返回 false 表示按目标类型饱和。
//非整数目标不走本函数。有符号目标能容纳的窄类型走 TryWriteInt，靠 SaturatingToInt64 先收到 long long。
inline bool TryWriteIntegerFromUnsigned(unsigned long long source, FieldType targetType, char* dest)
{
    switch (targetType)
    {
    case FieldType::Int8:   return TryWriteInt<int8_t>(SaturatingToInt64(source), dest);
    case FieldType::UInt8:  return TryWriteUInt<uint8_t>(source, dest);
    case FieldType::Int16:  return TryWriteInt<int16_t>(SaturatingToInt64(source), dest);
    case FieldType::UInt16: return TryWriteUInt<uint16_t>(source, dest);
    case FieldType::Int32:  return TryWriteInt<int32_t>(SaturatingToInt64(source), dest);
    case FieldType::UInt32: return TryWriteUInt<uint32_t>(source, dest);
    case FieldType::Int64:  return TryWriteInt<int64_t>(SaturatingToInt64(source), dest);
    case FieldType::UInt64: return TryWriteUInt<uint64_t>(source, dest);
    default:                return true;
    }
}
}
