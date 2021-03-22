#pragma once

#include "include/pq/copy_tuples_session.hpp"
#include "include/operation_types.hpp"

#include <memory>
#include <include/operation_types.hpp>

extern "C" {
  struct HeapTupleData;
} // extern "C"

namespace ForkExtension::PostgresPQ {

    class CopyToReversibleTuplesTable : public CopyTuplesSession {
    public:
        explicit CopyToReversibleTuplesTable( std::shared_ptr< pg_conn > _connection );
        ~CopyToReversibleTuplesTable();

        void push_delete(const std::string& _table_name, const HeapTupleData& _deleted_tuple, const TupleDesc& _tuple_desc );
        void push_insert(const std::string& _table_name, const HeapTupleData& _inserted_tuple, const TupleDesc& _tuple_desc );
        void push_update(const std::string& _table_name, const HeapTupleData& _old_tuple, const HeapTupleData& _new_tuple, const TupleDesc& _tuple_desc );

    private:
        void push_tuple_header();
        void push_id_field();
        void push_table_name( const std::string& _table_name );
        void push_operation( OperationType _operation);

    private:
        class TupleHeader;
    };

} // namespace ForkExtension::PostgresPQ
