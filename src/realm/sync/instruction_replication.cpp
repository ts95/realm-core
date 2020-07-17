#include <realm/sync/instruction_replication.hpp>
#include <realm/db.hpp>
#include <realm/sync/transform.hpp> // TransformError
#include <realm/sync/object.hpp>
#include <realm/list.hpp>

namespace realm {
namespace sync {

SyncReplication::SyncReplication(const std::string& realm_path)
    : TrivialReplication(realm_path)
{
}

void SyncReplication::initialize(DB& sg)
{
    REALM_ASSERT(!m_sg);
    m_sg = &sg;
}

void SyncReplication::reset()
{
    m_encoder.reset();

    m_last_table = nullptr;
    m_last_object = ObjKey();
    m_last_field = ColKey();
    m_last_class_name = InternString::npos;
    m_last_primary_key = Instruction::PrimaryKey();
    m_last_field_name = InternString::npos;
}

void SyncReplication::do_initiate_transact(Group& group, version_type current_version, bool history_updated)
{
    TrivialReplication::do_initiate_transact(group, current_version, history_updated);
    m_transaction = dynamic_cast<Transaction*>(&group); // FIXME: Is this safe?
    reset();
}

Instruction::Payload SyncReplication::as_payload(Mixed value)
{
    if (value.is_null()) {
        return Instruction::Payload{};
    }

    switch (value.get_type()) {
        case type_Int: {
            return Instruction::Payload{value.get<int64_t>()};
        }
        case type_Bool: {
            return Instruction::Payload{value.get<bool>()};
        }
        case type_Float: {
            return Instruction::Payload{value.get<float>()};
        }
        case type_Double: {
            return Instruction::Payload{value.get<double>()};
        }
        case type_String: {
            auto str = value.get<StringData>();
            auto range = m_encoder.add_string_range(str);
            return Instruction::Payload{range};
        }
        case type_Binary: {
            auto binary = value.get<BinaryData>();
            auto range = m_encoder.add_string_range(StringData{binary.data(), binary.size()});
            const bool is_binary = true;
            return Instruction::Payload{range, is_binary};
        }
        case type_Timestamp: {
            return Instruction::Payload{value.get<Timestamp>()};
        }
        case type_Decimal: {
            return Instruction::Payload{value.get<Decimal128>()};
        }
        case type_ObjectId: {
            return Instruction::Payload{value.get<ObjectId>()};
        }
        case type_TypedLink:
            [[fallthrough]];
        case type_Link: {
            REALM_TERMINATE("as_payload() needs table/collection for links");
            break;
        }
        case type_Mixed:
            [[fallthrough]];
        case type_OldTable:
            [[fallthrough]];
        case type_OldDateTime:
            [[fallthrough]];
        case type_LinkList: {
            REALM_TERMINATE("Invalid payload type");
            break;
        }
    }
    return Instruction::Payload{};
}

Instruction::Payload SyncReplication::as_payload(const CollectionBase& collection, Mixed value)
{
    return as_payload(*collection.get_table(), collection.get_col_key(), value);
}

Instruction::Payload SyncReplication::as_payload(const Table& table, ColKey col_key, Mixed value)
{
    if (value.is_null()) {
        // FIXME: `Mixed::get_type()` asserts on null.
        return Instruction::Payload{};
    }

    if (value.get_type() == type_Link) {
        ConstTableRef target_table = table.get_link_target(col_key);
        if (target_table->is_embedded()) {
            // FIXME: Include target table name to support Mixed of Embedded Objects.
            return Instruction::Payload::ObjectValue{};
        }

        Instruction::Payload::Link link;
        link.target_table = emit_class_name(*target_table);
        link.target = primary_key_for_object(*target_table, value.get<ObjKey>());
        return Instruction::Payload{link};
    }
    else if (value.get_type() == type_TypedLink) {
        auto obj_link = value.get<ObjLink>();
        ConstTableRef target_table = m_transaction->get_table(obj_link.get_table_key());
        REALM_ASSERT(target_table);

        if (target_table->is_embedded()) {
            REALM_TERMINATE("Dynamically typed embedded objects not supported yet.");
        }

        Instruction::Payload::Link link;
        link.target_table = emit_class_name(*target_table);
        link.target = primary_key_for_object(*target_table, obj_link.get_obj_key());
        return Instruction::Payload{link};
    }
    else {
        return as_payload(value);
    }
}

InternString SyncReplication::emit_class_name(StringData table_name)
{
    return m_encoder.intern_string(table_name_to_class_name(table_name));
}

InternString SyncReplication::emit_class_name(const Table& table)
{
    return emit_class_name(table.get_name());
}

Instruction::Payload::Type SyncReplication::get_payload_type(DataType type) const
{
    using Type = Instruction::Payload::Type;
    switch (type) {
        case type_Int:
            return Type::Int;
        case type_Bool:
            return Type::Bool;
        case type_String:
            return Type::String;
        case type_Binary:
            return Type::Binary;
        case type_Timestamp:
            return Type::Timestamp;
        case type_Float:
            return Type::Float;
        case type_Double:
            return Type::Double;
        case type_Decimal:
            return Type::Decimal;
        case type_Link:
            return Type::Link;
        case type_LinkList:
            return Type::Link;
        case type_TypedLink:
            return Type::Link;
        case type_ObjectId:
            return Type::ObjectId;
        case type_Mixed:
            return Type::Null;
        case type_OldTable:
            [[fallthrough]];
        case type_OldDateTime:
            unsupported_instruction();
    }
    return Type::Int; // Make compiler happy
}

void SyncReplication::add_class(TableKey tk, StringData name, bool is_embedded)
{
    TrivialReplication::add_class(tk, name, is_embedded);

    bool is_class = name.begins_with("class_");

    if (is_class && !m_short_circuit) {
        Instruction::AddTable instr;
        instr.table = emit_class_name(name);
        if (is_embedded) {
            instr.type = Instruction::AddTable::EmbeddedTable{};
        }
        else {
            auto field = m_encoder.intern_string(""); // FIXME: Should this be "_id"?
            const bool is_nullable = false;
            instr.type = Instruction::AddTable::PrimaryKeySpec{
                field,
                Instruction::Payload::Type::GlobalKey,
                is_nullable,
            };
        }
        emit(instr);
    }
}

void SyncReplication::add_class_with_primary_key(TableKey tk, StringData name, DataType pk_type, StringData pk_field,
                                                 bool nullable)
{
    TrivialReplication::add_class_with_primary_key(tk, name, pk_type, pk_field, nullable);

    bool is_class = name.begins_with("class_");

    if (is_class && !m_short_circuit) {
        Instruction::AddTable instr;
        instr.table = emit_class_name(name);
        auto field = m_encoder.intern_string(pk_field);
        auto spec = Instruction::AddTable::PrimaryKeySpec{field, get_payload_type(pk_type), nullable};
        if (!is_valid_key_type(spec.type)) {
            unsupported_instruction();
        }
        instr.type = std::move(spec);
        emit(instr);
    }
}

void SyncReplication::create_object(const Table* table, GlobalKey oid)
{
    if (table->is_embedded()) {
        unsupported_instruction(); // FIXME: TODO
    }

    TrivialReplication::create_object(table, oid);
    if (select_table(*table)) {
        if (table->get_primary_key_column()) {
            // Trying to create object without a primary key in a table that
            // has a primary key column.
            unsupported_instruction();
        }
        Instruction::CreateObject instr;
        instr.table = m_last_class_name;
        instr.object = oid;
        emit(instr);
    }
}

Instruction::PrimaryKey SyncReplication::as_primary_key(Mixed value)
{
    if (value.is_null()) {
        return mpark::monostate{};
    }
    else if (value.get_type() == type_Int) {
        return value.get<int64_t>();
    }
    else if (value.get_type() == type_String) {
        return m_encoder.intern_string(value.get<StringData>());
    }
    else if (value.get_type() == type_ObjectId) {
        return value.get<ObjectId>();
    }
    else {
        // Unsupported primary key type.
        unsupported_instruction();
    }
}

void SyncReplication::create_object_with_primary_key(const Table* table, GlobalKey oid, Mixed value)
{
    if (table->is_embedded()) {
        // Trying to create an object with a primary key in an embedded table.
        unsupported_instruction();
    }

    TrivialReplication::create_object_with_primary_key(table, oid, value);
    if (select_table(*table)) {
        auto col = table->get_primary_key_column();
        if (col && ((value.is_null() && col.is_nullable()) || DataType(col.get_type()) == value.get_type())) {
            Instruction::CreateObject instr;
            instr.table = m_last_class_name;
            instr.object = as_primary_key(value);
            emit(instr);
        }
        else {
            // Trying to create object with primary key in table without a
            // primary key column, or with wrong primary key type.
            unsupported_instruction();
        }
    }
}


void SyncReplication::prepare_erase_table(StringData table_name)
{
    REALM_ASSERT(table_name.begins_with("class_"));
    REALM_ASSERT(m_table_being_erased.empty());
    m_table_being_erased = std::string(table_name);
}

void SyncReplication::erase_group_level_table(TableKey table_key, size_t num_tables)
{
    TrivialReplication::erase_group_level_table(table_key, num_tables);

    StringData table_name = m_transaction->get_table_name(table_key);

    bool is_class = table_name.begins_with("class_");

    if (is_class) {
        REALM_ASSERT(table_name == m_table_being_erased);
        m_table_being_erased.clear();

        if (!m_short_circuit) {
            Instruction::EraseTable instr;
            instr.table = emit_class_name(table_name);
            emit(instr);
        }
    }

    m_last_table = nullptr;
}

void SyncReplication::rename_group_level_table(TableKey, StringData)
{
    unsupported_instruction();
}

void SyncReplication::insert_column(const Table* table, ColKey col_key, DataType type, StringData name,
                                    Table* target_table)
{
    TrivialReplication::insert_column(table, col_key, type, name, target_table);

    if (select_table(*table)) {
        Instruction::AddColumn instr;
        instr.table = m_last_class_name;
        instr.field = m_encoder.intern_string(name);
        instr.nullable = col_key.is_nullable();
        instr.type = get_payload_type(type);
        instr.collection_type = Instruction::AddColumn::CollectionType::Single;
        if (col_key.is_list()) {
            instr.collection_type = Instruction::AddColumn::CollectionType::List;
        }
        if (col_key.is_dictionary()) {
            instr.collection_type = Instruction::AddColumn::CollectionType::Dictionary;
            auto value_type = table->get_dictionary_value_type(col_key);
            instr.value_type = get_payload_type(value_type);
        }
        else {
            instr.value_type = Instruction::Payload::Type::Null;
        }

        // Mixed columns are always nullable.
        REALM_ASSERT(instr.type != Instruction::Payload::Type::Null || instr.nullable);

        if (instr.type == Instruction::Payload::Type::Link && target_table) {
            instr.link_target_table = emit_class_name(*target_table);
        }
        else {
            instr.link_target_table = m_encoder.intern_string("");
        }
        emit(instr);
    }
}

void SyncReplication::erase_column(const Table* table, ColKey col_ndx)
{
    TrivialReplication::erase_column(table, col_ndx);

    if (select_table(*table)) {
        if (table->get_name() == m_table_being_erased) {
            // Ignore any EraseColumn instructions generated by Core as part of
            // EraseTable.
            return;
        }
        // Not allowed to remove PK/OID columns!
        REALM_ASSERT(col_ndx != table->get_primary_key_column());
        Instruction::EraseColumn instr;
        instr.table = m_last_class_name;
        instr.field = m_encoder.intern_string(table->get_column_name(col_ndx));
        emit(instr);
    }
}

void SyncReplication::rename_column(const Table*, ColKey, StringData)
{
    unsupported_instruction();
}

void SyncReplication::list_set(const CollectionBase& list, size_t ndx, Mixed value)
{
    TrivialReplication::list_set(list, ndx, value);

    if (!value.is_null() && value.get_type() == type_Link && value.get<ObjKey>().is_unresolved()) {
        // If link is unresolved, it should not be communicated.
        return;
    }

    if (select_collection(list)) {
        Instruction::Update instr;
        populate_path_instr(instr, list, uint32_t(ndx));
        REALM_ASSERT(instr.is_array_update());
        instr.value = as_payload(list, value);
        instr.prior_size = uint32_t(list.size());
        emit(instr);
    }
}

void SyncReplication::list_insert(const CollectionBase& list, size_t ndx, Mixed value)
{
    TrivialReplication::list_insert(list, ndx, value);

    if (select_collection(list)) {
        auto sz = uint32_t(list.size());
        Instruction::ArrayInsert instr;
        populate_path_instr(instr, list, uint32_t(ndx));
        instr.value = as_payload(list, value);
        instr.prior_size = sz;
        emit(instr);
    }
}

void SyncReplication::add_int(const Table* table, ColKey col, ObjKey ndx, int_fast64_t value)
{
    TrivialReplication::add_int(table, col, ndx, value);

    if (select_table(*table)) {
        REALM_ASSERT(col != table->get_primary_key_column());

        Instruction::AddInteger instr;
        populate_path_instr(instr, *table, ndx, col);
        instr.value = value;
        emit(instr);
    }
}

void SyncReplication::set(const Table* table, ColKey col, ObjKey key, Mixed value, _impl::Instruction variant)
{
    TrivialReplication::set(table, col, key, value, variant);

    if (!value.is_null() && value.get_type() == type_Link && value.get<ObjKey>().is_unresolved()) {
        // If link is unresolved, it should not be communicated.
        return;
    }

    if (select_table(*table)) {
        Instruction::Update instr;
        populate_path_instr(instr, *table, key, col);
        instr.value = as_payload(*table, col, value);
        instr.is_default = (variant == _impl::instr_SetDefault);
        emit(instr);
    }
}


void SyncReplication::remove_object(const Table* table, ObjKey row_ndx)
{
    TrivialReplication::remove_object(table, row_ndx);
    if (table->is_embedded())
        return;
    REALM_ASSERT(!row_ndx.is_unresolved());

    // FIXME: This probably belongs in a function similar to sync::create_object().
    if (table->get_name().begins_with("class_")) {
    }

    if (select_table(*table)) {
        Instruction::EraseObject instr;
        instr.table = m_last_class_name;
        instr.object = primary_key_for_object(*table, row_ndx);
        emit(instr);
    }
}


void SyncReplication::list_move(const CollectionBase& view, size_t from_ndx, size_t to_ndx)
{
    TrivialReplication::list_move(view, from_ndx, to_ndx);
    if (select_collection(view)) {
        Instruction::ArrayMove instr;
        populate_path_instr(instr, view, uint32_t(from_ndx));
        instr.ndx_2 = uint32_t(to_ndx);
        emit(instr);
    }
}

void SyncReplication::list_erase(const CollectionBase& view, size_t ndx)
{
    size_t prior_size = view.size();
    TrivialReplication::list_erase(view, ndx);
    if (select_collection(view)) {
        Instruction::ArrayErase instr;
        populate_path_instr(instr, view, uint32_t(ndx));
        instr.prior_size = uint32_t(prior_size);
        emit(instr);
    }
}

void SyncReplication::list_clear(const CollectionBase& view)
{
    size_t prior_size = view.size();
    TrivialReplication::list_clear(view);
    if (select_collection(view)) {
        Instruction::ArrayClear instr;
        populate_path_instr(instr, view);
        instr.prior_size = uint32_t(prior_size);
        emit(instr);
    }
}


void SyncReplication::dictionary_insert(const CollectionBase& dict, Mixed key, Mixed val)
{
    TrivialReplication::dictionary_insert(dict, key, val);

    if (select_collection(dict)) {
        Instruction::DictionaryInsert instr;
        REALM_ASSERT(key.get_type() == type_String);
        populate_path_instr(instr, dict);
        StringData key_value = key.get_string();
        InternString interned_key_value = m_encoder.intern_string(key_value);
        instr.path.m_path.push_back(interned_key_value);
        instr.value = as_payload(dict, val);
        emit(instr);
    }
}


void SyncReplication::dictionary_erase(const CollectionBase& dict, Mixed key)
{
    TrivialReplication::dictionary_erase(dict, key);

    if (select_collection(dict)) {
        Instruction::DictionaryErase instr;
        REALM_ASSERT(key.get_type() == type_String);
        populate_path_instr(instr, dict);
        StringData key_value = key.get_string();
        InternString interned_key_value = m_encoder.intern_string(key_value);
        instr.path.m_path.push_back(interned_key_value);
        emit(instr);
    }
}


void SyncReplication::nullify_link(const Table* table, ColKey col_ndx, ObjKey ndx)
{
    TrivialReplication::nullify_link(table, col_ndx, ndx);

    if (select_table(*table)) {
        Instruction::Update instr;
        populate_path_instr(instr, *table, ndx, col_ndx);
        REALM_ASSERT(!instr.is_array_update());
        instr.value = Instruction::Payload{realm::util::none};
        instr.is_default = false;
        emit(instr);
    }
}

void SyncReplication::link_list_nullify(const Lst<ObjKey>& view, size_t ndx)
{
    size_t prior_size = view.size();
    TrivialReplication::link_list_nullify(view, ndx);
    if (select_collection(view)) {
        Instruction::ArrayErase instr;
        populate_path_instr(instr, view, uint32_t(ndx));
        instr.prior_size = uint32_t(prior_size);
        emit(instr);
    }
}

void SyncReplication::unsupported_instruction() const
{
    throw realm::sync::TransformError{"Unsupported instruction"};
}

bool SyncReplication::select_table(const Table& table)
{
    if (is_short_circuited()) {
        return false;
    }

    if (&table == m_last_table) {
        return true;
    }
    else {
        StringData name = table.get_name();
        if (name.begins_with("class_")) {
            m_last_class_name = emit_class_name(table);
            m_last_table = &table;
            m_last_field = ColKey{};
            m_last_object = ObjKey{};
            m_last_primary_key.reset();
            return true;
        }
        return false;
    }
}

bool SyncReplication::select_collection(const CollectionBase& view)
{
    return select_table(*view.get_table());
}

Instruction::PrimaryKey SyncReplication::primary_key_for_object(const Table& table, ObjKey key)
{
    bool should_emit = select_table(table);
    REALM_ASSERT(should_emit);

    ColKey pk_col = table.get_primary_key_column();
    const Obj obj = table.get_object(key);
    if (pk_col) {
        DataType pk_type = table.get_column_type(pk_col);
        if (obj.is_null(pk_col)) {
            return mpark::monostate{};
        }

        if (pk_type == type_Int) {
            return obj.get<int64_t>(pk_col);
        }

        if (pk_type == type_String) {
            StringData str = obj.get<StringData>(pk_col);
            auto interned = m_encoder.intern_string(str);
            return interned;
        }

        if (pk_type == type_ObjectId) {
            ObjectId id = obj.get<ObjectId>(pk_col);
            return id;
        }

        unsupported_instruction(); // Unsupported PK type
    }

    GlobalKey global_key = table.get_object_id(key);
    return global_key;
}

void SyncReplication::populate_path_instr(Instruction::PathInstruction& instr, const Table& table, ObjKey key,
                                          ColKey field)
{
    REALM_ASSERT(key);
    REALM_ASSERT(field);

    if (table.is_embedded()) {
        // For embedded objects, Obj::traverse_path() yields the top object
        // first, then objects in the path in order.
        auto obj = table.get_object(key);
        auto path_sizer = [&](size_t size) {
            REALM_ASSERT(size != 0);
            // Reserve 2 elements per path component, because link list entries
            // have both a field and an index.
            instr.path.m_path.reserve(size * 2);
        };

        auto visitor = [&](const Obj& path_obj, ColKey next_field, size_t index) {
            auto element_table = path_obj.get_table();
            if (element_table->is_embedded()) {
                StringData field_name = element_table->get_column_name(next_field);
                InternString interned_field_name = m_encoder.intern_string(field_name);
                instr.path.push_back(interned_field_name);
            }
            else {
                // This is the top object, populate it the normal way.
                populate_path_instr(instr, *element_table, path_obj.get_key(), next_field);
            }

            if (next_field.is_list()) {
                instr.path.push_back(uint32_t(index));
            }
        };

        obj.traverse_path(visitor, path_sizer);

        // The field in the embedded object is the last path component.
        StringData field_in_embedded = table.get_column_name(field);
        InternString interned_field_in_embedded = m_encoder.intern_string(field_in_embedded);
        instr.path.push_back(interned_field_in_embedded);
        return;
    }

    bool should_emit = select_table(table);
    REALM_ASSERT(should_emit);

    instr.table = m_last_class_name;

    if (m_last_object == key) {
        instr.object = *m_last_primary_key;
    }
    else {
        instr.object = primary_key_for_object(table, key);
        m_last_object = key;
        m_last_primary_key = instr.object;
    }

    if (m_last_field == field) {
        instr.field = m_last_field_name;
    }
    else {
        instr.field = m_encoder.intern_string(table.get_column_name(field));
        m_last_field = field;
        m_last_field_name = instr.field;
    }
}

void SyncReplication::populate_path_instr(Instruction::PathInstruction& instr, const CollectionBase& list)
{
    ConstTableRef source_table = list.get_table();
    ObjKey source_obj = list.get_key();
    ColKey source_field = list.get_col_key();
    populate_path_instr(instr, *source_table, source_obj, source_field);
}

void SyncReplication::populate_path_instr(Instruction::PathInstruction& instr, const CollectionBase& list,
                                          uint32_t ndx)
{
    populate_path_instr(instr, list);
    instr.path.m_path.push_back(ndx);
}

} // namespace sync
} // namespace realm
