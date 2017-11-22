/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#ifndef REALM_TABLE_VIEW_HPP
#define REALM_TABLE_VIEW_HPP

#include <realm/sort_descriptor.hpp>
#include <realm/table.hpp>
#include <realm/util/features.h>
#include <realm/obj_list.hpp>

namespace realm {

// Views, tables and synchronization between them:
//
// Views are built through queries against either tables or another view.
// Views may be restricted to only hold entries provided by another view.
// this other view is called the "restricting view".
// Views may be sorted in ascending or descending order of values in one ore more columns.
//
// Views remember the query from which it was originally built.
// Views remember the table from which it was originally built.
// Views remember a restricting view if one was used when it was originally built.
// Views remember the sorting criteria (columns and direction)
//
// A view may be operated in one of two distinct modes: *reflective* and *imperative*.
// Sometimes the term "reactive" is used instead of "reflective" with the same meaning.
//
// Reflective views:
// - A reflective view *always* *reflect* the result of running the query.
//   If the underlying tables or tableviews change, the reflective view changes as well.
//   A reflective view may need to rerun the query it was generated from, a potentially
//   costly operation which happens on demand.
// - It does not matter whether changes are explicitly done within the transaction, or
//   occur implicitly as part of advance_read() or promote_to_write().
//
// Imperative views:
// - An imperative view only *initially* holds the result of the query. An imperative
//   view *never* reruns the query. To force the view to match it's query (by rerunning it),
//   the view must be operated in reflective mode.
//   An imperative view can be modified explicitly. References can be added, removed or
//   changed.
//
// - In imperative mode, the references in the view tracks movement of the referenced data:
//   If you delete an entry which is referenced from a view, said reference is detached,
//   not removed.
// - It does not matter whether the delete is done in-line (as part of the current transaction),
//   or if it is done implicitly as part of advance_read() or promote_to_write().
//
// The choice between reflective and imperative views might eventually be represented by a
// switch on the tableview, but isn't yet. For now, clients (bindings) must call sync_if_needed()
// to get reflective behavior.
//
// Use cases:
//
// 1. Presenting data
// The first use case (and primary motivator behind the reflective view) is to just track
// and present the state of the database. In this case, the view is operated in reflective
// mode, it is not modified within the transaction, and it is not used to modify data in
// other parts of the database.
//
// 2. Handover
// The second use case is "handover." The implicit rerun of the query in our first use case
// may be too costly to be acceptable on the main thread. Instead you want to run the query
// on a worker thread, but display it on the main thread. To achieve this, you need two
// SharedGroups locked on to the same version of the database. If you have that, you can
// *handover* a view from one thread/SharedGroup to the other.
//
// Handover is a two-step procedure. First, the accessors are *exported* from one SharedGroup,
// called the sourcing group, then it is *imported* into another SharedGroup, called the
// receiving group. The thread associated with the sourcing SharedGroup will be
// responsible for the export operation, while the thread associated with the receiving
// SharedGroup will do the import operation.
//
// 3. Iterating a view and changing data
// The third use case (and a motivator behind the imperative view) is when you want
// to make changes to the database in accordance with a query result. Imagine you want to
// find all employees with a salary below a limit and raise their salaries to the limit (pseudocode):
//
//    promote_to_write();
//    view = table.where().less_than(salary_column,limit).find_all();
//    for (size_t i = 0; i < view.size(); ++i) {
//        view.set_int(salary_column, i, limit);
//        // add this to get reflective mode: view.sync_if_needed();
//    }
//    commit_and_continue_as_read();
//
// This is idiomatic imperative code and it works if the view is operated in imperative mode.
//
// If the view is operated in reflective mode, the behaviour surprises most people: When the
// first salary is changed, the entry no longer fullfills the query, so it is dropped from the
// view implicitly. view[0] is removed, view[1] moves to view[0] and so forth. But the next
// loop iteration has i=1 and refers to view[1], thus skipping view[0]. The end result is that
// every other employee get a raise, while the others don't.
//
// 4. Iterating intermixed with implicit updates
// This leads us to use case 4, which is similar to use case 3, but uses promote_to_write()
// intermixed with iterating a view. This is actually quite important to some, who do not want
// to end up with a large write transaction.
//
//    view = table.where().less_than(salary_column,limit).find_all();
//    for (size_t i = 0; i < view.size(); ++i) {
//        promote_to_write();
//        view.set_int(salary_column, i, limit);
//        commit_and_continue_as_write();
//    }
//
// Anything can happen at the call to promote_to_write(). The key question then becomes: how
// do we support a safe way of realising the original goal (raising salaries) ?
//
// using the imperative operating mode:
//
//    view = table.where().less_than(salary_column,limit).find_all();
//    for (size_t i = 0; i < view.size(); ++i) {
//        promote_to_write();
//        // add r.sync_if_needed(); to get reflective mode
//        if (r.is_row_attached(i)) {
//            Row r = view[i];
//            r.set_int(salary_column, limit);
//        }
//        commit_and_continue_as_write();
//    }
//
// This is safe, and we just aim for providing low level safety: is_row_attached() can tell
// if the reference is valid, and the references in the view continue to point to the
// same object at all times, also following implicit updates. The rest is up to the
// application logic.
//
// It is important to see, that there is no guarantee that all relevant employees get
// their raise in cases whith concurrent updates. At every call to promote_to_write() new
// employees may be added to the underlying table, but as the view is in imperative mode,
// these new employees are not added to the view. Also at promote_to_write() an existing
// employee could recieve a (different, larger) raise which would then be overwritten and lost.
// However, these are problems that you should expect, since the activity is spread over multiple
// transactions.


/// Common base class for TableView and ConstTableView.

class TableViewBase : public ObjList {
public:
    // - not in use / implemented yet:   ... explicit calls to sync_if_needed() must be used
    //                                       to get 'reflective' mode.
    //    enum mode { mode_Reflective, mode_Imperative };
    //    void set_operating_mode(mode);
    //    mode get_operating_mode();
    bool is_empty() const noexcept;

    // Tells if the table that this TableView points at still exists or has been deleted.
    bool is_attached() const noexcept;

    bool is_row_attached(size_t row_ndx) const noexcept;
    size_t num_attached_rows() const noexcept;

    // Get the query used to create this TableView
    // The query will have a null source table if this tv was not created from
    // a query
    const Query& get_query() const noexcept;

    // Column information
    const ColumnBase& get_column_base(size_t index) const;

    size_t get_column_count() const noexcept;
    StringData get_column_name(size_t column_ndx) const noexcept;
    size_t get_column_index(StringData name) const;
    DataType get_column_type(size_t column_ndx) const noexcept;

    // Searching
    template<typename T>
    size_t find_first(size_t column_ndx, T value) const;

    size_t find_first_int(size_t column_ndx, int64_t value) const;
    size_t find_first_bool(size_t column_ndx, bool value) const;
    size_t find_first_olddatetime(size_t column_ndx, OldDateTime value) const;
    size_t find_first_float(size_t column_ndx, float value) const;
    size_t find_first_double(size_t column_ndx, double value) const;
    size_t find_first_string(size_t column_ndx, StringData value) const;
    size_t find_first_binary(size_t column_ndx, BinaryData value) const;
    size_t find_first_timestamp(size_t column_ndx, Timestamp value) const;

    template <Action action, typename T, typename R>
    R aggregate(size_t column_ndx, size_t* result_count = nullptr, Key* return_key = nullptr) const;
    template <typename T>
    size_t aggregate_count(size_t column_ndx, T count_target) const;

    int64_t sum_int(size_t column_ndx) const;
    int64_t maximum_int(size_t column_ndx, Key* return_key = nullptr) const;
    int64_t minimum_int(size_t column_ndx, Key* return_key = nullptr) const;
    double average_int(size_t column_ndx, size_t* value_count = nullptr) const;
    size_t count_int(size_t column_ndx, int64_t target) const;

    double sum_float(size_t column_ndx) const;
    float maximum_float(size_t column_ndx, Key* return_key = nullptr) const;
    float minimum_float(size_t column_ndx, Key* return_key = nullptr) const;
    double average_float(size_t column_ndx, size_t* value_count = nullptr) const;
    size_t count_float(size_t column_ndx, float target) const;

    double sum_double(size_t column_ndx) const;
    double maximum_double(size_t column_ndx, Key* return_key = nullptr) const;
    double minimum_double(size_t column_ndx, Key* return_key = nullptr) const;
    double average_double(size_t column_ndx, size_t* value_count = nullptr) const;
    size_t count_double(size_t column_ndx, double target) const;

    Timestamp minimum_timestamp(size_t column_ndx, Key* return_key = nullptr) const;
    Timestamp maximum_timestamp(size_t column_ndx, Key* return_key = nullptr) const;
    size_t count_timestamp(size_t column_ndx, Timestamp target) const;

    // Simple pivot aggregate method. Experimental! Please do not
    // document method publicly.
    void aggregate(size_t group_by_column, size_t aggr_column, Table::AggrType op, Table& result) const;

    /// Search this view for the specified key. If found, the index of that row
    /// within this view is returned, otherwise `realm::not_found` is returned.
    size_t find_by_source_ndx(Key key) const noexcept;

    // Conversion
    void to_json(std::ostream&) const;
    void to_string(std::ostream&, size_t limit = 500) const;
    void row_to_string(size_t row_ndx, std::ostream&) const;

    // Determine if the view is 'in sync' with the underlying table
    // as well as other views used to generate the view. Note that updates
    // through views maintains synchronization between view and table.
    // It doesnt by itself maintain other views as well. So if a view
    // is generated from another view (not a table), updates may cause
    // that view to be outdated, AND as the generated view depends upon
    // it, it too will become outdated.
    bool is_in_sync() const override;

    // Tells if this TableView depends on a LinkList or row that has been deleted.
    bool depends_on_deleted_object() const;

    // Synchronize a view to match a table or tableview from which it
    // has been derived. Synchronization is achieved by rerunning the
    // query used to generate the view. If derived from another view, that
    // view will be synchronized as well.
    //
    // "live" or "reactive" views are implemented by calling sync_if_needed
    // before any of the other access-methods whenever the view may have become
    // outdated.
    //
    // This will make the TableView empty and in sync with the highest possible table version
    // if the TableView depends on an object (LinkView or row) that has been deleted.
    uint_fast64_t sync_if_needed() const override;

    // Sort m_key_values according to one column
    void sort(size_t column, bool ascending = true);

    // Sort m_key_values according to multiple columns
    void sort(SortDescriptor order);

    // Remove rows that are duplicated with respect to the column set passed as argument.
    // distinct() will preserve the original order of the row pointers, also if the order is a result of sort()
    // If two rows are indentical (for the given set of distinct-columns), then the last row is removed.
    // You can call sync_if_needed() to update the distinct view, just like you can for a sorted view.
    // Each time you call distinct() it will compound on the previous calls
    void distinct(size_t column);
    void distinct(DistinctDescriptor columns);

    // Replace the order of sort and distinct operations, bypassing manually
    // calling sort and distinct. This is a convenience method for bindings.
    void apply_descriptor_ordering(DescriptorOrdering new_ordering);

    // Returns whether the rows are guaranteed to be in table order.
    // This is true only of unsorted TableViews created from either:
    // - Table::find_all()
    // - Query::find_all() when the query is not restricted to a view.
    bool is_in_table_order() const;

    virtual std::unique_ptr<TableViewBase> clone() const = 0;

protected:
    // This TableView can be "born" from 4 different sources:
    // - LinkView
    // - Query::find_all()
    // - Table::get_distinct_view()
    // - Table::get_backlink_view()
    // Return the version of the source it was created from.
    uint64_t outside_version() const;

    void do_sync();

    // The source column index that this view contain backlinks for.
    size_t m_source_column_ndx = npos;
    // The target object that rows in this view link to.
    ConstObj m_linked_obj;

    // If this TableView was created from a LinkList, then this reference points to it. Otherwise it's 0
    mutable ConstLinkListPtr m_linklist_source;

    // m_distinct_column_source != npos if this view was created from distinct values in a column of m_table.
    size_t m_distinct_column_source = npos;

    // Stores the ordering criteria of applied sort and distinct operations.
    DescriptorOrdering m_descriptor_ordering;

    // A valid query holds a reference to its table which must match our m_table.
    // hence we can use a query with a null table reference to indicate that the view
    // was NOT generated by a query, but follows a table directly.
    Query m_query;
    // parameters for findall, needed to rerun the query
    size_t m_start;
    size_t m_end;
    size_t m_limit;

    mutable util::Optional<uint_fast64_t> m_last_seen_version;

    size_t m_num_detached_refs = 0;
    /// Construct null view (no memory allocated).
    TableViewBase() = default;

    /// Construct empty view, ready for addition of row indices.
    TableViewBase(Table* parent);
    TableViewBase(Table* parent, Query& query, size_t start, size_t end, size_t limit);
    TableViewBase(Table* parent, size_t column, const ConstObj& obj);
    TableViewBase(Table* parent, ConstLinkListPtr link_list);

    enum DistinctViewTag { DistinctView };
    TableViewBase(DistinctViewTag, Table* parent, size_t column_ndx);

    /// Copy constructor.
    TableViewBase(const TableViewBase&);

    /// Move constructor.
    TableViewBase(TableViewBase&&) noexcept;

    TableViewBase& operator=(const TableViewBase&);
    TableViewBase& operator=(TableViewBase&&) noexcept;

    template <class R, class V>
    static R find_all_integer(V*, size_t, int64_t);

    template <class R, class V>
    static R find_all_float(V*, size_t, float);

    template <class R, class V>
    static R find_all_double(V*, size_t, double);

    template <class R, class V>
    static R find_all_string(V*, size_t, StringData);

    using HandoverPatch = TableViewHandoverPatch;

    // handover machinery entry points based on dynamic type. These methods:
    // a) forward their calls to the static type entry points.
    // b) new/delete patch data structures.
    virtual std::unique_ptr<TableViewBase> clone_for_handover(std::unique_ptr<HandoverPatch>& patch,
                                                              ConstSourcePayload mode) const = 0;

    virtual std::unique_ptr<TableViewBase> clone_for_handover(std::unique_ptr<HandoverPatch>& patch,
                                                              MutableSourcePayload mode) = 0;

    void apply_and_consume_patch(std::unique_ptr<HandoverPatch>& patch, Group& group)
    {
        apply_patch(*patch, group);
        patch.reset();
    }
    // handover machinery entry points based on static type
    void apply_patch(HandoverPatch& patch, Group& group);
    TableViewBase(const TableViewBase& source, HandoverPatch& patch, ConstSourcePayload mode);
    TableViewBase(TableViewBase& source, HandoverPatch& patch, MutableSourcePayload mode);

private:
    void detach() const noexcept; // may have to remove const
    size_t find_first_integer(size_t column_ndx, int64_t value) const;
    template <class oper>
    Timestamp minmax_timestamp(size_t column_ndx, Key* return_key) const;

    friend class Table;
    friend class Query;
    friend class SharedGroup;
};


inline void TableViewBase::detach() const noexcept // may have to remove const
{
    m_table = TableRef();
}


class ConstTableView;


enum class RemoveMode { ordered, unordered };


/// A TableView gives read and write access to the parent table.
///
/// A 'const TableView' cannot be changed (e.g. sorted), nor can the
/// parent table be modified through it.
///
/// A TableView is both copyable and movable.
class TableView : public TableViewBase {
public:
    using TableViewBase::TableViewBase;

    TableView() = default;

    // Rows
    Obj get(size_t row_ndx) noexcept;
    Obj front() noexcept;
    Obj back() noexcept;
    Obj operator[](size_t row_ndx) noexcept;

    // Links
    TableRef get_link_target(size_t column_ndx) noexcept;
    ConstTableRef get_link_target(size_t column_ndx) const noexcept;
    void nullify_link(size_t column_ndx, size_t row_ndx);

    /// \defgroup table_view_removes
    //@{
    /// \brief Remove the specified row (or rows) from the underlying table.
    ///
    /// remove() removes the specified row from the underlying table,
    /// remove_last() removes the last row in the table view from the underlying
    /// table, and clear removes all the rows in the table view from the
    /// underlying table.
    ///
    /// When rows are removed from the underlying table, they will by necessity
    /// also be removed from the table view.
    ///
    /// The order of the remaining rows in the the table view will be maintained
    /// regardless of the value passed for \a underlying_mode.
    ///
    /// \param row_ndx The index within this table view of the row to be
    /// removed.
    ///
    /// \param underlying_mode If set to RemoveMode::ordered (the default), the
    /// rows will be removed from the underlying table in a way that maintains
    /// the order of the remaining rows in the underlying table. If set to
    /// RemoveMode::unordered, the order of the remaining rows in the underlying
    /// table will not in general be maintaind, but the operation will generally
    /// be much faster. In any case, the order of remaining rows in the table
    /// view will not be affected.
    void remove(size_t row_ndx, RemoveMode underlying_mode = RemoveMode::ordered);
    void remove_last(RemoveMode underlying_mode = RemoveMode::ordered);
    void clear(RemoveMode underlying_mode = RemoveMode::ordered);
    //@}

    // Searching (Int and String)
    TableView find_all_int(size_t column_ndx, int64_t value);
    ConstTableView find_all_int(size_t column_ndx, int64_t value) const;
    TableView find_all_bool(size_t column_ndx, bool value);
    ConstTableView find_all_bool(size_t column_ndx, bool value) const;
    TableView find_all_olddatetime(size_t column_ndx, OldDateTime value);
    ConstTableView find_all_olddatetime(size_t column_ndx, OldDateTime value) const;
    TableView find_all_float(size_t column_ndx, float value);
    ConstTableView find_all_float(size_t column_ndx, float value) const;
    TableView find_all_double(size_t column_ndx, double value);
    ConstTableView find_all_double(size_t column_ndx, double value) const;
    TableView find_all_string(size_t column_ndx, StringData value);
    ConstTableView find_all_string(size_t column_ndx, StringData value) const;
    // FIXME: Need: TableView find_all_binary(size_t column_ndx, BinaryData value);
    // FIXME: Need: ConstTableView find_all_binary(size_t column_ndx, BinaryData value) const;

    Table& get_parent() noexcept;
    const Table& get_parent() const noexcept;

    std::unique_ptr<TableViewBase> clone() const override
    {
        return std::unique_ptr<TableViewBase>(new TableView(*this));
    }

    std::unique_ptr<TableViewBase> clone_for_handover(std::unique_ptr<HandoverPatch>& patch,
                                                      ConstSourcePayload mode) const override
    {
        patch.reset(new HandoverPatch);
        std::unique_ptr<TableViewBase> retval(new TableView(*this, *patch, mode));
        return retval;
    }

    std::unique_ptr<TableViewBase> clone_for_handover(std::unique_ptr<HandoverPatch>& patch,
                                                      MutableSourcePayload mode) override
    {
        patch.reset(new HandoverPatch);
        std::unique_ptr<TableViewBase> retval(new TableView(*this, *patch, mode));
        return retval;
    }

private:
    TableView(Table& parent);
    TableView(Table& parent, Query& query, size_t start, size_t end, size_t limit);
    TableView(Table& parent, ConstLinkListPtr);

    TableView(DistinctViewTag, Table& parent, size_t column_ndx);

    TableView find_all_integer(size_t column_ndx, int64_t value);
    ConstTableView find_all_integer(size_t column_ndx, int64_t value) const;

    friend class ConstTableView;
    friend class Table;
    friend class Query;
    friend class TableViewBase;
    friend class LinkView;
    friend class LinkList;
};


/// A ConstTableView gives read access to the parent table, but no
/// write access. The view itself, though, can be changed, for
/// example, it can be sorted.
///
/// Note that methods are declared 'const' if, and only if they leave
/// the view unmodified, and this is irrespective of whether they
/// modify the parent table.
///
/// A ConstTableView has both copy and move semantics. See TableView
/// for more on this.
class ConstTableView : public TableViewBase {
public:
    using TableViewBase::TableViewBase;

    ConstTableView() = default;

    ConstTableView(const TableView&);
    ConstTableView(TableView&&);
    ConstTableView& operator=(const TableView&);
    ConstTableView& operator=(TableView&&);

    // Links
    ConstTableRef get_link_target(size_t column_ndx) const noexcept;

    // Searching (Int and String)
    ConstTableView find_all_int(size_t column_ndx, int64_t value) const;
    ConstTableView find_all_bool(size_t column_ndx, bool value) const;
    ConstTableView find_all_olddatetime(size_t column_ndx, OldDateTime value) const;
    ConstTableView find_all_float(size_t column_ndx, float value) const;
    ConstTableView find_all_double(size_t column_ndx, double value) const;
    ConstTableView find_all_string(size_t column_ndx, StringData value) const;

    const Table& get_parent() const noexcept;

    std::unique_ptr<TableViewBase> clone() const override
    {
        return std::unique_ptr<TableViewBase>(new ConstTableView(*this));
    }

    std::unique_ptr<TableViewBase> clone_for_handover(std::unique_ptr<HandoverPatch>& patch,
                                                      ConstSourcePayload mode) const override
    {
        patch.reset(new HandoverPatch);
        std::unique_ptr<TableViewBase> retval(new ConstTableView(*this, *patch, mode));
        return retval;
    }

    std::unique_ptr<TableViewBase> clone_for_handover(std::unique_ptr<HandoverPatch>& patch,
                                                      MutableSourcePayload mode) override
    {
        patch.reset(new HandoverPatch);
        std::unique_ptr<TableViewBase> retval(new ConstTableView(*this, *patch, mode));
        return retval;
    }

private:
    ConstTableView(const Table& parent);

    ConstTableView find_all_integer(size_t column_ndx, int64_t value) const;

    friend class TableView;
    friend class Table;
    friend class Query;
    friend class TableViewBase;
};


// ================================================================================================
// TableViewBase Implementation:

inline const Query& TableViewBase::get_query() const noexcept
{
    return m_query;
}

inline bool TableViewBase::is_empty() const noexcept
{
    return m_key_values.is_empty();
}

inline bool TableViewBase::is_attached() const noexcept
{
    return bool(m_table);
}

inline bool TableViewBase::is_row_attached(size_t row_ndx) const noexcept
{
    return m_table->is_valid(Key(m_key_values.get(row_ndx)));
}

inline size_t TableViewBase::num_attached_rows() const noexcept
{
    return m_key_values.size() - m_num_detached_refs;
}

inline size_t TableViewBase::find_by_source_ndx(Key key) const noexcept
{
    return m_key_values.find_first(key.value);
}


inline TableViewBase::TableViewBase(Table* parent)
    : ObjList(parent) // Throws
    , m_last_seen_version(m_table ? util::make_optional(m_table->m_version) : util::none)
{
}

inline TableViewBase::TableViewBase(Table* parent, Query& query, size_t start, size_t end, size_t limit)
    : ObjList(parent)
    , m_query(query)
    , m_start(start)
    , m_end(end)
    , m_limit(limit)
    , m_last_seen_version(outside_version())
{
}

inline TableViewBase::TableViewBase(Table* src_table, size_t src_col_ndx, const ConstObj& obj)
    : ObjList(src_table) // Throws
    , m_source_column_ndx(src_col_ndx)
    , m_linked_obj(obj)
    , m_last_seen_version(m_table ? util::make_optional(m_table->m_version) : util::none)
{
}

inline TableViewBase::TableViewBase(DistinctViewTag, Table* parent, size_t column_ndx)
    : ObjList(parent) // Throws
    , m_distinct_column_source(column_ndx)
    , m_last_seen_version(m_table ? util::make_optional(m_table->m_version) : util::none)
{
    REALM_ASSERT(m_distinct_column_source != npos);
}

inline TableViewBase::TableViewBase(Table* parent, ConstLinkListPtr link_list)
    : ObjList(parent) // Throws
    , m_linklist_source(std::move(link_list))
    , m_last_seen_version(m_table ? util::make_optional(m_table->m_version) : util::none)
{
    REALM_ASSERT(m_linklist_source);
}

inline TableViewBase::TableViewBase(const TableViewBase& tv)
    : ObjList(tv)
    , m_source_column_ndx(tv.m_source_column_ndx)
    , m_linked_obj(tv.m_linked_obj)
    , m_linklist_source(tv.m_linklist_source->clone())
    , m_distinct_column_source(tv.m_distinct_column_source)
    , m_descriptor_ordering(std::move(tv.m_descriptor_ordering))
    , m_query(tv.m_query)
    , m_start(tv.m_start)
    , m_end(tv.m_end)
    , m_limit(tv.m_limit)
    , m_last_seen_version(tv.m_last_seen_version)
    , m_num_detached_refs(tv.m_num_detached_refs)
{
}

inline TableViewBase::TableViewBase(TableViewBase&& tv) noexcept
    : ObjList(std::move(tv))
    , m_source_column_ndx(tv.m_source_column_ndx)
    , m_linked_obj(tv.m_linked_obj)
    , m_linklist_source(std::move(tv.m_linklist_source))
    , m_distinct_column_source(tv.m_distinct_column_source)
    , m_descriptor_ordering(std::move(tv.m_descriptor_ordering))
    , m_query(std::move(tv.m_query))
    , m_start(tv.m_start)
    , m_end(tv.m_end)
    , m_limit(tv.m_limit)
    ,
    // if we are created from a table view which is outdated, take care to use the outdated
    // version number so that we can later trigger a sync if needed.
    m_last_seen_version(tv.m_last_seen_version)
    , m_num_detached_refs(tv.m_num_detached_refs)
{
}

inline TableViewBase& TableViewBase::operator=(TableViewBase&& tv) noexcept
{
    m_table = std::move(tv.m_table);

    m_key_values.move_assign(tv.m_key_values);
    m_query = std::move(tv.m_query);
    m_num_detached_refs = tv.m_num_detached_refs;
    m_last_seen_version = tv.m_last_seen_version;
    m_start = tv.m_start;
    m_end = tv.m_end;
    m_limit = tv.m_limit;
    m_source_column_ndx = tv.m_source_column_ndx;
    m_linked_obj = tv.m_linked_obj;
    m_linklist_source = std::move(tv.m_linklist_source);
    m_descriptor_ordering = std::move(tv.m_descriptor_ordering);
    m_distinct_column_source = tv.m_distinct_column_source;

    return *this;
}

inline TableViewBase& TableViewBase::operator=(const TableViewBase& tv)
{
    if (this == &tv)
        return *this;

    Allocator& alloc = m_key_values.get_alloc();
    MemRef mem = tv.m_key_values.get_root_array()->clone_deep(alloc); // Throws
    _impl::DeepArrayRefDestroyGuard ref_guard(mem.get_ref(), alloc);
    m_key_values.destroy();
    m_key_values.get_root_array()->init_from_mem(mem);
    ref_guard.release();

    m_query = tv.m_query;
    m_num_detached_refs = tv.m_num_detached_refs;
    m_last_seen_version = tv.m_last_seen_version;
    m_start = tv.m_start;
    m_end = tv.m_end;
    m_limit = tv.m_limit;
    m_source_column_ndx = tv.m_source_column_ndx;
    m_linked_obj = tv.m_linked_obj;
    m_linklist_source = tv.m_linklist_source ? tv.m_linklist_source->clone() : LinkListPtr{};
    m_descriptor_ordering = tv.m_descriptor_ordering;
    m_distinct_column_source = tv.m_distinct_column_source;

    return *this;
}

#define REALM_ASSERT_COLUMN(column_ndx)                                                                              \
    REALM_ASSERT(m_table);                                                                                           \
    REALM_ASSERT(column_ndx < m_table->get_column_count())

#define REALM_ASSERT_ROW(row_ndx)                                                                                    \
    REALM_ASSERT(m_table);                                                                                           \
    REALM_ASSERT(row_ndx < m_key_values.size())

#define REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, column_type)                                                        \
    REALM_ASSERT_COLUMN(column_ndx);                                                                                 \
    REALM_DIAG_PUSH();                                                                                               \
    REALM_DIAG_IGNORE_TAUTOLOGICAL_COMPARE();                                                                        \
    REALM_ASSERT(m_table->get_column_type(column_ndx) == column_type ||                                              \
                 (m_table->get_column_type(column_ndx) == type_OldDateTime && column_type == type_Int));             \
    REALM_DIAG_POP()

#define REALM_ASSERT_INDEX(column_ndx, row_ndx)                                                                      \
    REALM_ASSERT_COLUMN(column_ndx);                                                                                 \
    REALM_ASSERT(row_ndx < m_key_values.size())

#define REALM_ASSERT_INDEX_AND_TYPE(column_ndx, row_ndx, column_type)                                                \
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, column_type);                                                           \
    REALM_ASSERT(row_ndx < m_key_values.size())

#define REALM_ASSERT_INDEX_AND_TYPE_TABLE_OR_MIXED(column_ndx, row_ndx)                                              \
    REALM_ASSERT_COLUMN(column_ndx);                                                                                 \
    REALM_DIAG_PUSH();                                                                                               \
    REALM_DIAG_IGNORE_TAUTOLOGICAL_COMPARE();                                                                        \
    REALM_ASSERT(m_table->get_column_type(column_ndx) == type_Table ||                                               \
                 (m_table->get_column_type(column_ndx) == type_Mixed));                                              \
    REALM_DIAG_POP();                                                                                                \
    REALM_ASSERT(row_ndx < m_key_values.size())

// Column information

inline const ColumnBase& TableViewBase::get_column_base(size_t index) const
{
    return m_table->get_column_base(index);
}

inline size_t TableViewBase::get_column_count() const noexcept
{
    REALM_ASSERT(m_table);
    return m_table->get_column_count();
}

inline StringData TableViewBase::get_column_name(size_t column_ndx) const noexcept
{
    REALM_ASSERT(m_table);
    return m_table->get_column_name(column_ndx);
}

inline size_t TableViewBase::get_column_index(StringData name) const
{
    REALM_ASSERT(m_table);
    return m_table->get_column_index(name);
}

inline DataType TableViewBase::get_column_type(size_t column_ndx) const noexcept
{
    REALM_ASSERT(m_table);
    return m_table->get_column_type(column_ndx);
}


inline TableRef TableView::get_link_target(size_t column_ndx) noexcept
{
    return m_table->get_link_target(column_ndx);
}

inline ConstTableRef TableView::get_link_target(size_t column_ndx) const noexcept
{
    return m_table->get_link_target(column_ndx);
}

inline ConstTableRef ConstTableView::get_link_target(size_t column_ndx) const noexcept
{
    return m_table->get_link_target(column_ndx);
}

// Searching


inline size_t TableViewBase::find_first_int(size_t column_ndx, int64_t value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_Int);
    return find_first_integer(column_ndx, value);
}

inline size_t TableViewBase::find_first_bool(size_t column_ndx, bool value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_Bool);
    return find_first_integer(column_ndx, value ? 1 : 0);
}

inline size_t TableViewBase::find_first_olddatetime(size_t column_ndx, OldDateTime value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_OldDateTime);
    return find_first_integer(column_ndx, int64_t(value.get_olddatetime()));
}

inline size_t TableViewBase::find_first_integer(size_t column_ndx, int64_t value) const
{
    return find_first<int64_t>(column_ndx, value);
}

inline size_t TableViewBase::find_first_float(size_t column_ndx, float value) const
{
    return find_first<float>(column_ndx, value);
}

inline size_t TableViewBase::find_first_double(size_t column_ndx, double value) const
{
    return find_first<double>(column_ndx, value);
}

inline size_t TableViewBase::find_first_string(size_t column_ndx, StringData value) const
{
    return find_first<StringData>(column_ndx, value);
}

inline size_t TableViewBase::find_first_binary(size_t column_ndx, BinaryData value) const
{
    return find_first<BinaryData>(column_ndx, value);
}

inline size_t TableViewBase::find_first_timestamp(size_t column_ndx, Timestamp value) const
{
    return find_first<Timestamp>(column_ndx, value);
}


template <class R, class V>
R TableViewBase::find_all_integer(V* view, size_t column_ndx, int64_t value)
{
    typedef typename std::remove_const<V>::type TNonConst;
    return view->m_table->where(const_cast<TNonConst*>(view)).equal(column_ndx, value).find_all();
}

template <class R, class V>
R TableViewBase::find_all_float(V* view, size_t column_ndx, float value)
{
    typedef typename std::remove_const<V>::type TNonConst;
    return view->m_table->where(const_cast<TNonConst*>(view)).equal(column_ndx, value).find_all();
}

template <class R, class V>
R TableViewBase::find_all_double(V* view, size_t column_ndx, double value)
{
    typedef typename std::remove_const<V>::type TNonConst;
    return view->m_table->where(const_cast<TNonConst*>(view)).equal(column_ndx, value).find_all();
}

template <class R, class V>
R TableViewBase::find_all_string(V* view, size_t column_ndx, StringData value)
{
    typedef typename std::remove_const<V>::type TNonConst;
    return view->m_table->where(const_cast<TNonConst*>(view)).equal(column_ndx, value).find_all();
}


//-------------------------- TableView, ConstTableView implementation:

inline ConstTableView::ConstTableView(const TableView& tv)
    : TableViewBase(tv)
{
}

inline ConstTableView::ConstTableView(TableView&& tv)
    : TableViewBase(std::move(tv))
{
}

inline void TableView::remove_last(RemoveMode underlying_mode)
{
    if (!is_empty())
        remove(size() - 1, underlying_mode);
}

inline Table& TableView::get_parent() noexcept
{
    return *m_table;
}

inline const Table& TableView::get_parent() const noexcept
{
    return *m_table;
}

inline const Table& ConstTableView::get_parent() const noexcept
{
    return *m_table;
}

inline TableView::TableView(Table& parent)
    : TableViewBase(&parent)
{
}

inline TableView::TableView(Table& parent, Query& query, size_t start, size_t end, size_t limit)
    : TableViewBase(&parent, query, start, end, limit)
{
}

inline TableView::TableView(Table& parent, ConstLinkListPtr link_list)
    : TableViewBase(&parent, std::move(link_list))
{
}

inline TableView::TableView(TableViewBase::DistinctViewTag, Table& parent, size_t column_ndx)
    : TableViewBase(TableViewBase::DistinctView, &parent, column_ndx)
{
}

inline ConstTableView::ConstTableView(const Table& parent)
    : TableViewBase(const_cast<Table*>(&parent))
{
}

inline ConstTableView& ConstTableView::operator=(const TableView& tv)
{
    TableViewBase::operator=(tv);
    return *this;
}

inline ConstTableView& ConstTableView::operator=(TableView&& tv)
{
    TableViewBase::operator=(std::move(tv));
    return *this;
}


// - string
inline TableView TableView::find_all_string(size_t column_ndx, StringData value)
{
    return TableViewBase::find_all_string<TableView>(this, column_ndx, value);
}

inline ConstTableView TableView::find_all_string(size_t column_ndx, StringData value) const
{
    return TableViewBase::find_all_string<ConstTableView>(this, column_ndx, value);
}

inline ConstTableView ConstTableView::find_all_string(size_t column_ndx, StringData value) const
{
    return TableViewBase::find_all_string<ConstTableView>(this, column_ndx, value);
}

// - float
inline TableView TableView::find_all_float(size_t column_ndx, float value)
{
    return TableViewBase::find_all_float<TableView>(this, column_ndx, value);
}

inline ConstTableView TableView::find_all_float(size_t column_ndx, float value) const
{
    return TableViewBase::find_all_float<ConstTableView>(this, column_ndx, value);
}

inline ConstTableView ConstTableView::find_all_float(size_t column_ndx, float value) const
{
    return TableViewBase::find_all_float<ConstTableView>(this, column_ndx, value);
}


// - double
inline TableView TableView::find_all_double(size_t column_ndx, double value)
{
    return TableViewBase::find_all_double<TableView>(this, column_ndx, value);
}

inline ConstTableView TableView::find_all_double(size_t column_ndx, double value) const
{
    return TableViewBase::find_all_double<ConstTableView>(this, column_ndx, value);
}

inline ConstTableView ConstTableView::find_all_double(size_t column_ndx, double value) const
{
    return TableViewBase::find_all_double<ConstTableView>(this, column_ndx, value);
}


// -- 3 variants of the 3 find_all_{int, bool, date} all based on integer

inline TableView TableView::find_all_integer(size_t column_ndx, int64_t value)
{
    return TableViewBase::find_all_integer<TableView>(this, column_ndx, value);
}

inline ConstTableView TableView::find_all_integer(size_t column_ndx, int64_t value) const
{
    return TableViewBase::find_all_integer<ConstTableView>(this, column_ndx, value);
}

inline ConstTableView ConstTableView::find_all_integer(size_t column_ndx, int64_t value) const
{
    return TableViewBase::find_all_integer<ConstTableView>(this, column_ndx, value);
}


inline TableView TableView::find_all_int(size_t column_ndx, int64_t value)
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_Int);
    return find_all_integer(column_ndx, value);
}

inline TableView TableView::find_all_bool(size_t column_ndx, bool value)
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_Bool);
    return find_all_integer(column_ndx, value ? 1 : 0);
}

inline TableView TableView::find_all_olddatetime(size_t column_ndx, OldDateTime value)
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_OldDateTime);
    return find_all_integer(column_ndx, int64_t(value.get_olddatetime()));
}


inline ConstTableView TableView::find_all_int(size_t column_ndx, int64_t value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_Int);
    return find_all_integer(column_ndx, value);
}

inline ConstTableView TableView::find_all_bool(size_t column_ndx, bool value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_Bool);
    return find_all_integer(column_ndx, value ? 1 : 0);
}

inline ConstTableView TableView::find_all_olddatetime(size_t column_ndx, OldDateTime value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_OldDateTime);
    return find_all_integer(column_ndx, int64_t(value.get_olddatetime()));
}


inline ConstTableView ConstTableView::find_all_int(size_t column_ndx, int64_t value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_Int);
    return find_all_integer(column_ndx, value);
}

inline ConstTableView ConstTableView::find_all_bool(size_t column_ndx, bool value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_Bool);
    return find_all_integer(column_ndx, value ? 1 : 0);
}

inline ConstTableView ConstTableView::find_all_olddatetime(size_t column_ndx, OldDateTime value) const
{
    REALM_ASSERT_COLUMN_AND_TYPE(column_ndx, type_OldDateTime);
    return find_all_integer(column_ndx, int64_t(value.get_olddatetime()));
}


// Rows


inline Obj TableView::get(size_t row_ndx) noexcept
{
    REALM_ASSERT_ROW(row_ndx);
    Key key(m_key_values.get(row_ndx));
    REALM_ASSERT(key != realm::null_key);
    return m_table->get_object(key);
}


inline Obj TableView::front() noexcept
{
    return get(0);
}

inline Obj TableView::back() noexcept
{
    size_t last_row_ndx = size() - 1;
    return get(last_row_ndx);
}

inline Obj TableView::operator[](size_t row_ndx) noexcept
{
    return get(row_ndx);
}

} // namespace realm

#endif // REALM_TABLE_VIEW_HPP
