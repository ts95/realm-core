#ifndef __TDB_COLUMN_STRING_ENUM__
#define __TDB_COLUMN_STRING_ENUM__

#include "column_string.hpp"

namespace tightdb {

class ColumnStringEnum : public Column {
public:
    ColumnStringEnum(size_t ref_keys, size_t ref_values, ArrayParent* parent=NULL, size_t pndx=0,
                     Allocator& alloc=GetDefaultAllocator());
    ~ColumnStringEnum();
    void Destroy();

    size_t Size() const;
    bool is_empty() const;

    const char* Get(size_t ndx) const;
    bool add(const char* value);
    bool Set(size_t ndx, const char* value);
    bool Insert(size_t ndx, const char* value);
    void Delete(size_t ndx);
    void Clear();

    size_t find_first_int(const char* value, size_t start=0, size_t end=-1) const;
    void find_all_int(Array& res, const char* value, size_t start=0, size_t end=-1) const;

    size_t find_first_int(size_t key_index, size_t start=0, size_t end=-1) const;
    void find_all_int(Array& res, size_t key_index, size_t start=0, size_t end=-1) const;

    void UpdateParentNdx(int diff);
    void  UpdateFromParent();

#ifdef _DEBUG
    bool Compare(const ColumnStringEnum& c) const;
    void verify() const;
    void ToDot(std::ostream& out, const char* title) const;
#endif // _DEBUG

    size_t GetKeyNdx(const char* value) const;
    size_t GetKeyNdxOrAdd(const char* value);

private:

    // Member variables
    AdaptiveStringColumn m_keys;
};

}

#endif //__TDB_COLUMN_STRING_ENUM__
