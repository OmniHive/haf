#include <hive/plugins/sql_serializer/sql_serializer_plugin.hpp>
#include <hive/plugins/sql_serializer/data_processor.hpp>

#include <hive/chain/util/impacted.hpp>

#include <hive/protocol/config.hpp>
#include <hive/protocol/operations.hpp>

#include <hive/utilities/plugin_utilities.hpp>

#include <fc/io/json.hpp>
#include <fc/io/sstream.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/utf8.hpp>

#include <boost/filesystem.hpp>
#include <condition_variable>

#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <pqxx/pqxx>


namespace hive
{
  namespace plugins
  {
    namespace sql_serializer
    {

    inline std::string get_operation_name(const hive::protocol::operation& op)
    {
      PSQL::name_gathering_visitor v;
      return op.visit(v);
    }

    
    struct account_info
    {
      account_info(int id, unsigned int operation_count) : _id(id), _operation_count(operation_count) {}

      /// Account id
      int _id; 
      unsigned int _operation_count;
    };

    typedef std::map<std::string, account_info> account_cache_t;

    typedef std::map<std::string, int> permlink_cache_t;

    typedef std::vector<PSQL::processing_objects::account_data_t> account_data_container_t;
    typedef std::vector<PSQL::processing_objects::permlink_data_t> permlink_data_container_t;

    typedef std::vector<PSQL::processing_objects::process_block_t> block_data_container_t;
    typedef std::vector<PSQL::processing_objects::process_transaction_t> transaction_data_container_t;
    typedef std::vector<PSQL::processing_objects::process_transaction_multisig_t> transaction_multisig_data_container_t;
    typedef std::vector<PSQL::processing_objects::process_operation_t> operation_data_container_t;
    typedef std::vector<PSQL::processing_objects::account_operation_data_t> account_operation_data_container_t;


      using num_t = std::atomic<uint64_t>;
      using duration_t = fc::microseconds;
      using stat_time_t = std::atomic<duration_t>;

      inline void operator+=( stat_time_t& atom, const duration_t& dur )
      {
        atom.store( atom.load() + dur );
      }

      struct stat_t
      {
        stat_time_t processing_time{ duration_t{0} };
        num_t count{0};
      };

      struct ext_stat_t : public stat_t
      {
        stat_time_t flush_time{ duration_t{0} };

        void reset()
        {
          processing_time.store( duration_t{0} );
          flush_time.store( duration_t{0} );
          count.store(0);
        }
      };

      struct stats_group
      {
        stat_time_t sending_cache_time{ duration_t{0} };
        uint64_t workers_count{0};
        uint64_t all_created_workers{0};

        ext_stat_t blocks{};
        ext_stat_t transactions{};
        ext_stat_t operations{};

        void reset()
        {
          blocks.reset();
          transactions.reset();
          operations.reset();

          sending_cache_time.store(duration_t{0});
          workers_count = 0;
          all_created_workers = 0;
        }
      };

      inline std::ostream& operator<<(std::ostream& os, const stat_t& obj)
      {
        return os << obj.processing_time.load().count() << " us | count: " << obj.count.load();
      }

      inline std::ostream& operator<<(std::ostream& os, const ext_stat_t& obj)
      {
        return os << "flush time: " << obj.flush_time.load().count() << " us | processing time: " << obj.processing_time.load().count() << " us | count: " << obj.count.load();
      }

      inline std::ostream& operator<<(std::ostream& os, const stats_group& obj)
      {
        #define __shortcut( name ) << #name ": " << obj.name << std::endl
        return os
          << "threads created since last info: " << obj.all_created_workers << std::endl
          << "currently working threads: " << obj.workers_count << std::endl
          << "sending accounts and permlinks took: " << obj.sending_cache_time.load().count() << " us"<< std::endl
          __shortcut(blocks)
          __shortcut(transactions)
          __shortcut(operations)
          ;
      }

      using namespace hive::plugins::sql_serializer::PSQL;

      constexpr size_t default_reservation_size{ 16'000u };
      constexpr size_t max_tuples_count{ 1'000 };
      constexpr size_t max_data_length{ 16*1024*1024 }; 

      namespace detail
      {

      using data_processing_status = data_processor::data_processing_status;
      using data_chunk_ptr = data_processor::data_chunk_ptr;

      struct cached_data_t
      {
        account_cache_t _account_cache;
        int             _next_account_id;

        permlink_cache_t _permlink_cache;
        int             _next_permlink_id;

        account_data_container_t accounts;
        permlink_data_container_t permlinks;

        block_data_container_t blocks;
        transaction_data_container_t transactions;
        transaction_multisig_data_container_t transactions_multisig;
        operation_data_container_t operations;
        account_operation_data_container_t account_operations;

        size_t total_size;

        explicit cached_data_t(const size_t reservation_size) : _next_account_id{ 0 }, _next_permlink_id{ 0 }, total_size{ 0ul }
        {
          accounts.reserve(reservation_size);
          permlinks.reserve(reservation_size);
          blocks.reserve(reservation_size);
          transactions.reserve(reservation_size);
          transactions_multisig.reserve(reservation_size);
          operations.reserve(reservation_size);
          account_operations.reserve(reservation_size);
        }

        ~cached_data_t()
        {
          ilog(
          "accounts: ${a} permlinks: ${p} blocks: ${b} trx: ${t} operations: ${o} account_operation_data: ${aod} total size: ${ts}...",
          ("a", accounts.size() )
          ("p", permlinks.size() )
          ("b", blocks.size() )
          ("t", transactions.size() )
          ("o", operations.size() )
          ("aod", account_operations.size() )
          ("ts", total_size )
          );
        }

      };
      using cached_containter_t = std::unique_ptr<cached_data_t>;

      /**
       * @brief Common implementation of data writer to be used for all SQL entities.
       * 
       * @tparam DataContainer temporary container providing a data chunk.
       * @tparam TupleConverter a functor to convert volatile representation (held in the DataContainer) into SQL representation
       *                        TupleConverter must match to function interface:
       *                        std::string(pqxx::work& tx, typename DataContainer::const_reference)
       * 
      */
      template <class DataContainer, class TupleConverter, const char* const TABLE_NAME, const char* const COLUMN_LIST>
      class container_data_writer
      {
      public:
        container_data_writer(std::string psqlUrl, const cached_data_t& mainCache, std::string description) : _mainCache(mainCache)

        {
          _processor = std::make_unique<data_processor>(psqlUrl, description, flush_data);
        }

        void trigger(DataContainer&& data, bool wait_for_data_completion)
        {
          if(data.empty() == false)
          {
            _processor->trigger(std::make_unique<chunk>(_mainCache, std::move(data)));
            if(wait_for_data_completion)
              _processor->complete_data_processing();
          }

          FC_ASSERT(data.empty());
        }

        void register_transaction_controler(transaction_controller_ptr tx_controller)
        {
          FC_ASSERT(_prev_tx_controller == nullptr);
          _prev_tx_controller = _processor->register_transaction_controler(std::move(tx_controller));
          FC_ASSERT(_prev_tx_controller != nullptr);
        }

        void restore_transaction_controller()
        {
          FC_ASSERT(_prev_tx_controller != nullptr);
          _processor->register_transaction_controler(std::move(_prev_tx_controller));

        }

        void complete_data_processing()
        {
          _processor->complete_data_processing();
        }

        void join()
        {
          _processor->join();
        }

      private:
        static data_processing_status flush_data(const data_chunk_ptr& dataPtr, transaction& tx)
        {
          const chunk* holder = static_cast<const chunk*>(dataPtr.get());
          data_processing_status processingStatus;

          TupleConverter conv(holder->_mainCache);

          const DataContainer& data = holder->_data;

          FC_ASSERT(data.empty() == false);

          std::string query = "INSERT INTO ";
          query += TABLE_NAME;
          query += '(';
          query += COLUMN_LIST;
          query += ") VALUES\n";

          auto dataI = data.cbegin();
          query += '(' + conv(*dataI) + ")\n";

          for(++dataI; dataI != data.cend(); ++dataI)
          {
            query += ",(" + conv(*dataI) + ")\n";
          }

          query += ';';

          tx.exec(query);

          processingStatus.first += data.size();
          processingStatus.second = true;

          return processingStatus;

        }

        private:
          class chunk : public data_processor::data_chunk
          {
          public:
            chunk(const cached_data_t& mainCache, DataContainer&& data) : _data(std::move(data)), _mainCache(mainCache) {}
            virtual ~chunk() {}

            virtual std::string generate_code(size_t* processedItem) const override { return std::string(); }

            DataContainer _data;
            const cached_data_t& _mainCache;
          };

      private:
        const cached_data_t&            _mainCache;
        std::unique_ptr<data_processor> _processor;
        transaction_controller_ptr      _prev_tx_controller;
      };

      template <typename TableDescriptor>
      using table_data_writer = container_data_writer<typename TableDescriptor::container_t, typename TableDescriptor::data2sql_tuple,
        TableDescriptor::TABLE, TableDescriptor::COLS>;

          struct data2_sql_tuple_base
          {
          explicit data2_sql_tuple_base(const cached_data_t& mainCache) : _mainCache(mainCache) {}

          protected:
              std::string escape(const std::string& source) const
              {
                return escape_sql(source);
              }

              std::string escape_raw(const fc::ripemd160& hash) const
              {
                return '\'' + fc::to_hex(hash.data(), hash.data_size()) + '\'';
              }

              std::string escape_raw(const fc::optional<signature_type>& sign) const
              {
                if( sign.valid() )
                  return '\'' + fc::to_hex(reinterpret_cast<const char*>( sign->begin() ), sign->size()) + '\'';
                else
                  return "NULL";
              }

              const cached_data_t& _mainCache;
          private:

              fc::string escape_sql(const std::string &text) const
              {
                if(text.empty()) return "E''";

                std::wstring utf32;
                utf32.reserve( text.size() );
                fc::decodeUtf8( text, &utf32 );

                std::string ret;
                ret.reserve( 6 * text.size() );

                ret = "E'";

                for (auto it = utf32.begin(); it != utf32.end(); it++)
                {

                  const wchar_t& c{*it};
                  const int code = static_cast<int>(c);

                  if( code == 0 ) ret += " ";
                  if(code > 0 && code <= 0x7F && std::isprint(code)) // if printable ASCII
                  {
                    switch(c)
                    {
                      case L'\r': ret += "\\015"; break;
                      case L'\n': ret += "\\012"; break;
                      case L'\v': ret += "\\013"; break;
                      case L'\f': ret += "\\014"; break;
                      case L'\\': ret += "\\134"; break;
                      case L'\'': ret += "\\047"; break;
                      case L'%':  ret += "\\045"; break;
                      case L'_':  ret += "\\137"; break;
                      case L':':  ret += "\\072"; break;
                      default:    ret += static_cast<char>(code); break;
                    }
                  }
                  else
                  {
                    fc::string u_code{}; 
                    u_code.reserve(8);

                    const int i_c = int(c);
                    const char * c_str = reinterpret_cast<const char*>(&i_c);
                    for( int _s = ( i_c > 0xff ) + ( i_c > 0xffff ) + ( i_c > 0xffffff ); _s >= 0; _s-- ) 
                      u_code += fc::to_hex( c_str + _s, 1 );

                    if(i_c > 0xffff)
                    {
                      ret += "\\U";
                      if(u_code.size() < 8) ret.insert( ret.end(), 8 - u_code.size(), '0' );
                    }
                    else 
                    {
                      ret += "\\u";
                      if(u_code.size() < 4) ret.insert( ret.end(), 4 - u_code.size(), '0' );
                    }
                    ret += u_code;
                  }
                }

                ret += '\'';
                return ret;
              }
          };

      struct hive_accounts
      {
        typedef account_data_container_t container_t;

        static const char TABLE[];
        static const char COLS[];

        struct data2sql_tuple : public data2_sql_tuple_base
        {
          using data2_sql_tuple_base::data2_sql_tuple_base;

          std::string operator()(typename container_t::const_reference data)
          {
            return std::to_string(data.id) + ',' + escape(data.name);
          }
        };
      };

      const char hive_accounts::TABLE[] = "hive_accounts";
      const char hive_accounts::COLS[] = "id, name";

      struct hive_permlink_data
      {
        typedef permlink_data_container_t container_t;

        static const char TABLE[];
        static const char COLS[];

        struct data2sql_tuple : public data2_sql_tuple_base
        {
          using data2_sql_tuple_base::data2_sql_tuple_base;

          std::string operator()(typename container_t::const_reference data) const
          {
            return std::to_string(data.id) + ',' + escape(data.permlink);
          }
        };
      };

      const char hive_permlink_data::TABLE[] = "hive_permlink_data";
      const char hive_permlink_data::COLS[] = "id, permlink";

      struct hive_blocks
      {
        typedef block_data_container_t container_t;

        static const char TABLE[];
        static const char COLS[];

        struct data2sql_tuple : public data2_sql_tuple_base
        {
          using data2_sql_tuple_base::data2_sql_tuple_base;

          std::string operator()(typename container_t::const_reference data) const
          {
            return std::to_string(data.block_number) + "," + escape_raw(data.hash) + "," +
              escape_raw(data.prev_hash) + ", '" + data.created_at.to_iso_string() + '\'';
          }
        };
      };

      const char hive_blocks::TABLE[] = "hive_blocks";
      const char hive_blocks::COLS[] = "num, hash, prev, created_at";

      struct hive_transactions
      {
        typedef transaction_data_container_t container_t;

        static const char TABLE[];
        static const char COLS[];

        struct data2sql_tuple : public data2_sql_tuple_base
        {
          using data2_sql_tuple_base::data2_sql_tuple_base;

          std::string operator()(typename container_t::const_reference data) const
          {
            return std::to_string(data.block_number) + "," + escape_raw(data.hash) + "," + std::to_string(data.trx_in_block) + "," +
                   std::to_string(data.ref_block_num) + "," + std::to_string(data.ref_block_prefix) + ",'" + data.expiration.to_iso_string() + "'," + escape_raw(data.signature);
          }
        };
      };

      const char hive_transactions::TABLE[] = "hive_transactions";
      const char hive_transactions::COLS[] = "block_num, trx_hash, trx_in_block, ref_block_num, ref_block_prefix, expiration, signature";

      struct hive_transactions_multisig
      {
        typedef transaction_multisig_data_container_t container_t;

        static const char TABLE[];
        static const char COLS[];

        struct data2sql_tuple : public data2_sql_tuple_base
        {
          using data2_sql_tuple_base::data2_sql_tuple_base;

          std::string operator()(typename container_t::const_reference data) const
          {
            return escape_raw(data.hash) + "," + escape_raw(data.signature);
          }
        };
      };

      const char hive_transactions_multisig::TABLE[] = "hive_transactions_multisig";
      const char hive_transactions_multisig::COLS[] = "trx_hash, signature";

      struct hive_operations
      {
        typedef operation_data_container_t container_t;

        static const char TABLE[];
        static const char COLS[];

        struct data2sql_tuple : public data2_sql_tuple_base
        {
          using data2_sql_tuple_base::data2_sql_tuple_base;

          std::string operator()(typename container_t::const_reference data) const
          {
            // deserialization
            fc::variant opVariant;
            fc::to_variant(data.op, opVariant);
            fc::string deserialized_op = fc::json::to_string(opVariant);

            std::vector<fc::string> permlinks;
            hive::app::operation_get_permlinks(data.op, permlinks);

            std::string permlink_id_array = "'{";
            /*bool first = true;
            for(const auto& p : permlinks)
            {
              auto permlinkI = _mainCache._permlink_cache.find(p);
              FC_ASSERT(permlinkI != _mainCache._permlink_cache.cend());
              if(first == false)
                permlink_id_array += ',';

              permlink_id_array += std::to_string(permlinkI->second);
              first = false;
            }
            */
            permlink_id_array += "}'";

            permlink_id_array = "NULL::int[]";

            return std::to_string(data.operation_id) + ',' + std::to_string(data.block_number) + ',' +
              std::to_string(data.trx_in_block) + ',' + std::to_string(data.op_in_trx) + ',' +
              std::to_string(data.op.which()) + ',' + escape(deserialized_op); // + ',' + permlink_id_array;
          }
        };
      };

      const char hive_operations::TABLE[] = "hive_operations";
      const char hive_operations::COLS[] = "id, block_num, trx_in_block, op_pos, op_type_id, body"; //, permlink_ids";

      struct hive_account_operations
      {
        typedef account_operation_data_container_t container_t;

        static const char TABLE[];
        static const char COLS[];

        struct data2sql_tuple : public data2_sql_tuple_base
        {
          using data2_sql_tuple_base::data2_sql_tuple_base;

          std::string operator()(typename container_t::const_reference data) const
          {
            return std::to_string(data.operation_id) + ',' + std::to_string(data.account_id) + ',' +
              std::to_string(data.operation_seq_no);
          }
        };
      };

      const char hive_account_operations::TABLE[] = "hive_account_operations";
      const char hive_account_operations::COLS[] = "operation_id, account_id, account_op_seq_no";

      using account_data_container_t_writer = table_data_writer<hive_accounts>;
      using permlink_data_container_t_writer = table_data_writer<hive_permlink_data>;
      using block_data_container_t_writer = table_data_writer<hive_blocks>;
      using transaction_data_container_t_writer = table_data_writer<hive_transactions>;
      using transaction_multisig_data_container_t_writer = table_data_writer<hive_transactions_multisig>;
      using operation_data_container_t_writer = table_data_writer<hive_operations>;
      using account_operation_data_container_t_writer = table_data_writer<hive_account_operations>;

      /**
       * @brief Collects new identifiers (accounts or permlinks) defined by given operation.
       * 
      */
      struct new_ids_collector
      {
        typedef void result_type;

        new_ids_collector(account_cache_t* known_accounts, int* next_account_id,
          permlink_cache_t* permlink_cache, int* next_permlink_id,
          account_data_container_t* newAccounts,
          permlink_data_container_t* newPermlinks) :
          _known_accounts(known_accounts),
          _known_permlinks(permlink_cache),
          _next_account_id(*next_account_id),
          _next_permlink_id(*next_permlink_id),
          _new_accounts(newAccounts),
          _new_permlinks(newPermlinks),
          _processed_operation(nullptr)
        {
        }

        void collect(const hive::protocol::operation& op)
        {
          _processed_operation = &op;

          _processed_operation->visit(*this);

          _processed_operation = nullptr;
        }

      private:
        template<int64_t N, typename T, typename... Ts>
        friend struct fc::impl::storage_ops;

        template< typename T >
        void operator()(const T& v) { }

        void operator()(const hive::protocol::account_create_operation& op)
        {
          on_new_account(op.new_account_name);
        }

        void operator()(const hive::protocol::account_create_with_delegation_operation& op)
        {
          on_new_account(op.new_account_name);
        }

        void operator()(const hive::protocol::pow_operation& op)
        {
          if(_known_accounts->find(op.get_worker_account()) == _known_accounts->end())
            on_new_account(op.get_worker_account());
        }
        
        void operator()(const hive::protocol::pow2_operation& op)
        {
          flat_set<hive::protocol::account_name_type> newAccounts;
          hive::protocol::operation packed_op(op);
          hive::app::operation_get_impacted_accounts(packed_op, newAccounts);

          for(const auto& account_id : newAccounts)
          {
            if(_known_accounts->find(account_id) == _known_accounts->end())
              on_new_account(account_id);
          }
        }

        void operator()(const hive::protocol::create_claimed_account_operation& op)
        {
          on_new_account(op.new_account_name);
        }

        void operator()(const hive::protocol::comment_operation& op)
        {
          auto ii = _known_permlinks->emplace(op.permlink, _next_permlink_id + 1);
          /// warning comment_operation in edit case uses the same permlink.
          if(ii.second)
          {
            ++_next_permlink_id;
            _new_permlinks->emplace_back(_next_permlink_id, op.permlink);
          }
        }

      private:
        void on_new_account(const hive::protocol::account_name_type& name)
        {
          ++_next_account_id;
          auto ii = _known_accounts->emplace(std::string(name), account_info(_next_account_id, 0));
          _new_accounts->emplace_back(_next_account_id, std::string(name));

          FC_ASSERT(ii.second, "Already found account `${a}' at processing operation: `{o}.", ("a", name)("o", get_operation_name(*_processed_operation)));
        }

      private:
        account_cache_t* _known_accounts;
        permlink_cache_t* _known_permlinks;

        int& _next_account_id;
        int& _next_permlink_id;

        account_data_container_t* _new_accounts;
        permlink_data_container_t* _new_permlinks;

        const hive::protocol::operation* _processed_operation;
      };



        struct transaction_repr_t
        {
          std::unique_ptr<pqxx::connection> _connection;
          std::unique_ptr<pqxx::work> _transaction;

          transaction_repr_t() = default;
          transaction_repr_t(pqxx::connection* _conn, pqxx::work* _trx) : _connection{_conn}, _transaction{_trx} {}

          auto get_escaping_charachter_methode() const
          {
            return [this](const char *val) -> fc::string { return std::move( this->_connection->esc(val) ); };
          }

          auto get_raw_escaping_charachter_methode() const
          {
            return [this](const char *val, const size_t s) -> fc::string { 
              pqxx::binarystring __tmp(val, s); 
              return std::move( this->_transaction->esc_raw( __tmp.str() ) ); 
            };
          }
        };
        using transaction_t = std::unique_ptr<transaction_repr_t>;

        struct postgress_connection_holder
        {
          explicit postgress_connection_holder(const fc::string &url)
              : connection_string{url} {}

          transaction_t start_transaction() const
          {
            pqxx::connection* _conn = new pqxx::connection{ this->connection_string };
            pqxx::work *_trx = new pqxx::work{*_conn};
            _trx->exec("SET CONSTRAINTS ALL DEFERRED;");

            return transaction_t{ new transaction_repr_t{ _conn, _trx } };
          }

          bool exec_transaction(transaction_t &trx, const fc::string &sql) const
          {
            if (sql == fc::string())
              return true;
            return sql_safe_execution([&trx, &sql]() { trx->_transaction->exec(sql); }, sql.c_str());
          }

          bool commit_transaction(transaction_t &trx) const
          {
            return sql_safe_execution([&]() { trx->_transaction->commit(); }, "commit");
          }

          void abort_transaction(transaction_t &trx) const
          {
            trx->_transaction->abort();
          }

          bool exec_single_in_transaction(const fc::string &sql, pqxx::result *result = nullptr) const
          {
            if (sql == fc::string())
              return true;

            return sql_safe_execution([&]() {
              pqxx::connection conn{ this->connection_string };
              pqxx::work trx{conn};
              if (result)
                *result = trx.exec(sql);
              else
                trx.exec(sql);
              trx.commit();
            }, sql.c_str());
          }

          template<typename T>
          bool get_single_value(const fc::string& query, T& _return) const
          {
            pqxx::result res;
            if(!exec_single_in_transaction( query, &res ) && res.empty() ) return false;
            _return = res.at(0).at(0).as<T>();
            return true;
          }

          template<typename T>
          T get_single_value(const fc::string& query) const
          {
            T _return;
            FC_ASSERT( get_single_value( query, _return ) );
            return _return;
          }

          uint get_max_transaction_count() const
          {
            // get maximum amount of connections defined by postgres and set half of it as max; minimum is 1
            return std::max( 1u, get_single_value<uint>("SELECT setting::int / 2 FROM pg_settings WHERE  name = 'max_connections'") );
          }

        private:
          fc::string connection_string;

          bool sql_safe_execution(const std::function<void()> &f, const char* msg = nullptr) const
          {
            try
            {
              f();
              return true;
            }
            catch (const pqxx::pqxx_exception &sql_e)
            {
              elog("Exception from postgress: ${e}", ("e", sql_e.base().what()));
            }
            catch (const std::exception &e)
            {
              elog("Exception: ${e}", ("e", e.what()));
            }
            catch (...)
            {
              elog("Unknown exception occured");
            }
            if(msg) std::cerr << "Error message: " << msg << std::endl;
            return false;
          }
        };

        class sql_serializer_plugin_impl final
        {
        public:
          sql_serializer_plugin_impl(const std::string &url, hive::chain::database& _chain_db) 
            : connection{url},
              db_url{url},
              chain_db{_chain_db}
          {

            init_data_processors();

            /** By default the data_writer class builds a concurrent (owned by given thread) transaction controller.
                Since after starting up, we don't know which mode should be used, lets switch into LIVE-SYNC (single transaction) mode.
                Once reindex-start notification will be received, working mode will be switched back to the concurrent transaction controller.
            */
            switch_to_single_transaction_controller();
          }

          ~sql_serializer_plugin_impl()
          {
            ilog("Serializer plugin is closing");

            cleanup_sequence();

            ilog("Serializer plugin has been closed");
          }

          boost::signals2::connection _on_pre_apply_operation_con;
          boost::signals2::connection _on_pre_apply_block_con;
          boost::signals2::connection _on_post_apply_block_con;
          boost::signals2::connection _on_starting_reindex;
          boost::signals2::connection _on_finished_reindex;

          std::unique_ptr<account_data_container_t_writer> _account_writer;
          std::unique_ptr<permlink_data_container_t_writer> _permlink_writer;
          std::unique_ptr<block_data_container_t_writer> _block_writer;
          std::unique_ptr<transaction_data_container_t_writer> _transaction_writer;
          std::unique_ptr<transaction_multisig_data_container_t_writer> _transaction_multisig_writer;
          std::unique_ptr<operation_data_container_t_writer> _operation_writer;
          std::unique_ptr<account_operation_data_container_t_writer> _account_operation_writer;

          postgress_connection_holder connection;
          std::string db_url;
          hive::chain::database& chain_db;
          fc::optional<fc::string> path_to_schema;

          uint32_t last_skipped_block = 0;
          uint32_t psql_block_number = 0;
          uint32_t psql_index_threshold = 0;
          uint32_t head_block_number = 0;

          uint32_t blocks_per_commit = 1;
          int64_t block_vops = 0;
          int64_t op_sequence_id = 0; 

          cached_containter_t currently_caching_data;
          stats_group current_stats;

          bool massive_sync = false;

          void log_statistics()
          {
            std::cout << current_stats;
            current_stats.reset();
          }

          void switch_db_items( bool mode, const std::string& function_name, const std::string& objects_name ) const
          {
            /// Since hive_operation_types table is actually immutable after initial fill (writing very limited amount of data), don't mess with its indexes
            /// to let working ON CONFLICT DO NOTHING clause during subsequent init tries.
            std::vector<std::string> table_names = { "hive_blocks", "hive_transactions", "hive_transactions_multisig",
                                                     "hive_permlink_data", "hive_operations", "hive_accounts", "hive_account_operations" };

            ilog("${mode} ${objects_name}...", ("objects_name", objects_name )("mode", ( mode ? "Creating" : "Dropping" ) ) );

            std::list< data_processor > processors;

            for( const auto& table_name : table_names )
            {
              std::string query = std::string("SELECT ") + function_name + "( '" + table_name + "' );";
              std::string description = "Query processor: `" + query + "'";
              processors.emplace_back( db_url, description, [query, &table_name , &objects_name, mode](const data_chunk_ptr&, transaction& tx) -> data_processing_status
                            {
                              ilog("Attempting to execute query: `${query}`...", ("query", query ) );
                              const auto start_time = fc::time_point::now();
                              tx.exec( query );
                              ilog(
                                "${mode} of ${mod_type} for `${table}` done in ${time} ms", 
                                ("mode", (mode ? "Creating" : "Saving and dropping")) ("mod_type", objects_name) ("table", table_name) ("time", (fc::time_point::now() - start_time).count() / 1000.0 ) 
                              );
                              return data_processing_status();
                            } );

              processors.back().trigger(data_processor::data_chunk_ptr());
            }

            for( auto& item : processors )
            {
              item.join();
            }

            ilog("The ${objects_name} have been ${mode}...", ("objects_name", objects_name )("mode", ( mode ? "created" : "dropped" ) ) );
          }

          void switch_db_items( bool create ) const
          {
            if( psql_block_number == 0 || ( psql_block_number + psql_index_threshold <= head_block_number ) )
            {
              ilog("Switching indexes and constraints is allowed: psql block number: `${pbn}` psql index threshold: `${pit}` head block number: `${hbn}` ...",
              ("pbn", psql_block_number)("pit", psql_index_threshold)("hbn", head_block_number));

              if(create)
              {
                switch_db_items(create, "restore_indexes_constraints", "indexes/constraints");
                switch_db_items(create, "restore_foreign_keys", "foreign keys");
              }
              else
              {
                switch_db_items(create, "save_and_drop_indexes_foreign_keys", "foreign keys");
                switch_db_items(create, "save_and_drop_indexes_constraints", "indexes/constraints");
              }
            }
            else
            {
              ilog("Switching indexes and constraints isn't allowed: psql block number: `${pbn}` psql index threshold: `${pit}` head block number: `${hbn}` ...",
              ("pbn", psql_block_number)("pit", psql_index_threshold)("hbn", head_block_number));
            }
          }

          void recreate_db()
          {
            FC_ASSERT(path_to_schema.valid());
            std::vector<fc::string> querries;
            fc::string line;

            std::ifstream file{*path_to_schema};
            while(std::getline(file, line)) querries.push_back(line);
            file.close();

            for(const fc::string& q : querries) 
              if(!connection.exec_single_in_transaction(q)) 
                wlog("Failed to execute query from ${schema_path}:\n${query}", ("schema_path", *path_to_schema)("query", q));
          }

          void init_database(bool freshDb, uint32_t max_block_number )
          {
            head_block_number = max_block_number;

            load_initial_db_data();

            if(freshDb)
            {
              connection.exec_single_in_transaction(PSQL::get_all_type_definitions());
              import_all_builtin_accounts();
            }

            switch_db_items( false/*mode*/ );
          }

          void import_all_builtin_accounts()
          {
            const auto& accounts = chain_db.get_index<hive::chain::account_index, hive::chain::by_id>();

            auto* data = currently_caching_data.get(); 

            int& next_account_id = data->_next_account_id;
            auto& known_accounts = data->_account_cache;
            auto& new_accounts = data->accounts;

            for(const auto& account : accounts)
            {
              auto ii = known_accounts.emplace(std::string(account.name), account_info(next_account_id + 1, 0));
              if(ii.second)
              {
                ++next_account_id;
                ilog("Importing builtin account: `${a}' with id: ${i}", ("a", account.name)("i", next_account_id));
                new_accounts.emplace_back(next_account_id, std::string(account.name));
              }
              else
              {
                ilog("Builtin account: `${a}' already exists.", ("a", account.name));
              }
            }
          }

          void load_initial_db_data()
          {
            ilog("Loading operation's last id and account/permlink caches...");

            auto* data = currently_caching_data.get();
            auto& account_cache = data->_account_cache;

            int next_account_id = 0;
            op_sequence_id = 0;
            psql_block_number = 0;

            data_processor block_loader(db_url, "Block loader",
              [this](const data_chunk_ptr&, transaction& tx) -> data_processing_status
              {
                data_processing_status processingStatus;
                pqxx::result data = tx.exec("SELECT hb.num AS _max_block FROM hive_blocks hb ORDER BY hb.num DESC LIMIT 1;");
                if( !data.empty() )
                {
                  FC_ASSERT( data.size() == 1 );
                  const auto& record = data[0];
                  psql_block_number = record["_max_block"].as<uint64_t>();
                }
                return data_processing_status();
              }
            );

            block_loader.trigger(data_processor::data_chunk_ptr());

            data_processor sequence_loader(db_url, "Sequence loader",
              [this](const data_chunk_ptr&, transaction& tx) -> data_processing_status
              {
                data_processing_status processingStatus;
                pqxx::result data = tx.exec("SELECT ho.id AS _max FROM hive_operations ho ORDER BY ho.id DESC LIMIT 1;");
                if( !data.empty() )
                {
                  FC_ASSERT( data.size() == 1 );
                  const auto& record = data[0];
                  op_sequence_id = record["_max"].as<int64_t>();
                }
                return data_processing_status();
              }
            );

            sequence_loader.trigger(data_processor::data_chunk_ptr());

            data_processor account_cache_loader(db_url, "Account cache loader",
              [&account_cache, &next_account_id](const data_chunk_ptr&, transaction& tx) -> data_processing_status
              {
                data_processing_status processingStatus;
                pqxx::result data = tx.exec("SELECT ai.name, ai.id, ai.operation_count FROM account_operation_count_info_view ai;");
                for(auto recordI = data.begin(); recordI != data.end(); ++recordI)
                {
                  const auto& record = *recordI;

                  const char* name = record["name"].c_str();
                  int account_id = record["id"].as<int>();
                  unsigned int operation_count = record["operation_count"].as<int>();

                  if(account_id > next_account_id)
                    next_account_id = account_id;

                  account_cache.emplace(std::string(name), account_info(account_id, operation_count));

                  ++processingStatus.first;
                }

                return processingStatus;
              }
            );

            account_cache_loader.trigger(data_processor::data_chunk_ptr());

            auto& permlink_cache = data->_permlink_cache;
            int next_permlink_id = 0;

            data_processor permlink_cache_loader(db_url, "Permlink cache loader",
              [&permlink_cache, &next_permlink_id](const data_chunk_ptr&, transaction& tx) -> data_processing_status
              {
                data_processing_status processingStatus;
                pqxx::result data = tx.exec("SELECT pd.permlink, pd.id FROM hive_permlink_data pd;");
                for(auto recordI = data.begin(); recordI != data.end(); ++recordI)
                {
                  const auto& record = *recordI;

                  const char* permlink = record["permlink"].c_str();
                  int permlink_id = record["id"].as<int>();

                  if(permlink_id > next_permlink_id)
                    next_permlink_id = permlink_id;

                  permlink_cache.emplace(std::string(permlink), permlink_id);
                  ++processingStatus.first;
                }

                return processingStatus;
              }
            );

            permlink_cache_loader.trigger(data_chunk_ptr());

            account_cache_loader.join();
            permlink_cache_loader.join();
            sequence_loader.join();
            block_loader.join();

            data->_next_account_id = next_account_id + 1;
            data->_next_permlink_id = next_permlink_id + 1;

            ilog("Loaded ${a} cached accounts and ${p} cached permlink data", ("a", account_cache.size())("p", permlink_cache.size()));
            ilog("Next account id: ${a},  next permlink id: ${p}, next operation id: ${s} psql block number: ${pbn}.",
              ("a", data->_next_account_id)("p", data->_next_permlink_id)("s", op_sequence_id + 1)("pbn", psql_block_number));
          }

          void trigger_data_flush(account_data_container_t& data);
          void trigger_data_flush(permlink_data_container_t& data);
          void trigger_data_flush(block_data_container_t& data);
          void trigger_data_flush(transaction_data_container_t& data);
          void trigger_data_flush(transaction_multisig_data_container_t& data);
          void trigger_data_flush(operation_data_container_t& data);
          void trigger_data_flush(account_operation_data_container_t& data);

          void init_data_processors();
          void join_data_processors()
          {
            _account_writer->join();
            _permlink_writer->join();
            _block_writer->join();
            _transaction_writer->join();
            _transaction_multisig_writer->join();
            _operation_writer->join();
            _account_operation_writer->join();
          }

          void switch_to_single_transaction_controller();
          void switch_to_concurrent_transaction_controller();
          void wait_for_data_processing_finish();

          void process_cached_data();
          void collect_new_ids(const hive::protocol::operation& op);
          void collect_impacted_accounts(int64_t operation_id, const hive::protocol::operation& op);

          bool skip_reversible_block(uint32_t block);

          void cleanup_sequence()
          {
            ilog("Flushing rest of data, wait a moment...");

            process_cached_data();
            join_data_processors();

            ilog("Done, cleanup complete");
          }
        };

void sql_serializer_plugin_impl::init_data_processors()
{
  const auto& mainCache = *currently_caching_data.get();
  _account_writer = std::make_unique<account_data_container_t_writer>(db_url, mainCache, "Account data writer");
  _permlink_writer = std::make_unique<permlink_data_container_t_writer>(db_url, mainCache, "Permlink data writer");
  _block_writer = std::make_unique<block_data_container_t_writer>(db_url, mainCache, "Block data writer");
  _transaction_writer = std::make_unique<transaction_data_container_t_writer>(db_url, mainCache, "Transaction data writer");
  _transaction_multisig_writer = std::make_unique<transaction_multisig_data_container_t_writer>(db_url, mainCache, "Transaction multisig data writer");
  _operation_writer = std::make_unique<operation_data_container_t_writer>(db_url, mainCache, "Operation data writer");
  _account_operation_writer = std::make_unique<account_operation_data_container_t_writer>(db_url, mainCache, "Account operation data writer");
}

void sql_serializer_plugin_impl::switch_to_single_transaction_controller()
{
  auto single_tx_controler = build_single_transaction_controller(db_url);
  
  _account_writer->register_transaction_controler(single_tx_controler);
  _permlink_writer->register_transaction_controler(single_tx_controler);
  _block_writer->register_transaction_controler(single_tx_controler);
  _transaction_writer->register_transaction_controler(single_tx_controler);
  _transaction_multisig_writer->register_transaction_controler(single_tx_controler);
  _operation_writer->register_transaction_controler(single_tx_controler);
  _account_operation_writer->register_transaction_controler(single_tx_controler);
}

void sql_serializer_plugin_impl::switch_to_concurrent_transaction_controller()
{
  _account_writer->restore_transaction_controller();
  _permlink_writer->restore_transaction_controller();
  _block_writer->restore_transaction_controller();
  _transaction_writer->restore_transaction_controller();
  _transaction_multisig_writer->restore_transaction_controller();
  _operation_writer->restore_transaction_controller();
  _account_operation_writer->restore_transaction_controller();
}

void sql_serializer_plugin_impl::wait_for_data_processing_finish()
{
  _account_writer->complete_data_processing();
  _permlink_writer->complete_data_processing();
  _block_writer->complete_data_processing();
  _transaction_writer->complete_data_processing();
  _transaction_multisig_writer->complete_data_processing();
  _operation_writer->complete_data_processing();
  _account_operation_writer->complete_data_processing();
}

void sql_serializer_plugin_impl::trigger_data_flush(account_data_container_t& data)
{
  _account_writer->trigger(std::move(data), massive_sync == false);
}

void sql_serializer_plugin_impl::trigger_data_flush(permlink_data_container_t& data)
{
  _permlink_writer->trigger(std::move(data), massive_sync == false);
}

void sql_serializer_plugin_impl::trigger_data_flush(block_data_container_t& data)
{
  _block_writer->trigger(std::move(data), massive_sync == false);
}

void sql_serializer_plugin_impl::trigger_data_flush(transaction_data_container_t& data)
{
  _transaction_writer->trigger(std::move(data), massive_sync == false);
}

void sql_serializer_plugin_impl::trigger_data_flush(transaction_multisig_data_container_t& data)
{
  _transaction_multisig_writer->trigger(std::move(data), massive_sync == false);
}

void sql_serializer_plugin_impl::trigger_data_flush(operation_data_container_t& data)
{
  _operation_writer->trigger(std::move(data), massive_sync == false);
}

void sql_serializer_plugin_impl::trigger_data_flush(account_operation_data_container_t& data)
{
  _account_operation_writer->trigger(std::move(data), massive_sync == false);
}

void sql_serializer_plugin_impl::process_cached_data()
{  
  auto* data = currently_caching_data.get();

  trigger_data_flush(data->accounts);
  trigger_data_flush(data->permlinks);
  trigger_data_flush(data->blocks);
  trigger_data_flush(data->operations);

  trigger_data_flush(data->transactions);
  trigger_data_flush(data->transactions_multisig);
  trigger_data_flush(data->account_operations);
}

void sql_serializer_plugin_impl::collect_new_ids(const hive::protocol::operation& op)
{
  auto* data = currently_caching_data.get();
  auto* account_cache = &data->_account_cache;
  auto* permlink_cache = &data->_permlink_cache;

  new_ids_collector collector(account_cache, &data->_next_account_id, permlink_cache, &data->_next_permlink_id,
    &data->accounts, &data->permlinks);
  op.visit(collector);
}

void sql_serializer_plugin_impl::collect_impacted_accounts(int64_t operation_id, const hive::protocol::operation& op)
{
  flat_set<hive::protocol::account_name_type> impacted;
  hive::app::operation_get_impacted_accounts(op, impacted);

  impacted.erase(hive::protocol::account_name_type());

  auto* data = currently_caching_data.get();
  auto* account_cache = &data->_account_cache;
  auto* account_operations = &data->account_operations;

  for(const auto& name : impacted)
  {
    auto accountI = account_cache->find(name);
    FC_ASSERT(accountI != account_cache->end(), "Missing account: `${a}' at processing operation: `${o}'", ("a", name)("o", get_operation_name(op)));

    account_info& aInfo = accountI->second;

    account_operations->emplace_back(operation_id, aInfo._id, aInfo._operation_count);

    ++aInfo._operation_count;
  }
}

bool sql_serializer_plugin_impl::skip_reversible_block(uint32_t block_no)
{
  if(block_no <= psql_block_number)
  {
    FC_ASSERT(block_no > chain_db.get_last_irreversible_block_num());

    if(last_skipped_block < block_no)
    {
      ilog("Skipping data provided by already processed reversible block: ${block_no}", ("block_no", block_no));
      last_skipped_block = block_no;
    }

    return true;
  }

  return false;
}

      } // namespace detail

      sql_serializer_plugin::sql_serializer_plugin() {}
      sql_serializer_plugin::~sql_serializer_plugin() {}

      void sql_serializer_plugin::set_program_options(options_description &cli, options_description &cfg)
      {
        cfg.add_options()("psql-url", boost::program_options::value<string>(), "postgres connection string")
                         ("psql-path-to-schema", "if set and replay starts from 0 block, executes script")
                         ("psql-index-threshold", bpo::value<uint32_t>()->default_value( 1'000'000 ), "indexes/constraints will be recreated if `psql_block_number + psql_index_threshold >= head_block_number`");
      }

      void sql_serializer_plugin::plugin_initialize(const boost::program_options::variables_map &options)
      {
        ilog("Initializing sql serializer plugin");
        FC_ASSERT(options.count("psql-url"), "`psql-url` is required argument");

        auto& db = appbase::app().get_plugin<hive::plugins::chain::chain_plugin>().db();

        my = std::make_unique<detail::sql_serializer_plugin_impl>(options["psql-url"].as<fc::string>(), db);

        // settings
        if (options.count("psql-path-to-schema"))
          my->path_to_schema = options["psql-path-to-schema"].as<fc::string>();

        my->psql_index_threshold = options["psql-index-threshold"].as<uint32_t>();

        my->currently_caching_data = std::make_unique<detail::cached_data_t>( default_reservation_size );

        // signals
        my->_on_pre_apply_operation_con = db.add_pre_apply_operation_handler([&](const operation_notification &note) { on_pre_apply_operation(note); }, *this);
        my->_on_pre_apply_block_con = db.add_pre_apply_block_handler([&](const block_notification& note) { on_pre_apply_block(note); }, *this);
        my->_on_post_apply_block_con = db.add_post_apply_block_handler([&](const block_notification &note) { on_post_apply_block(note); }, *this);
        my->_on_finished_reindex = db.add_post_reindex_handler([&](const reindex_notification &note) { on_post_reindex(note); }, *this);
        my->_on_starting_reindex = db.add_pre_reindex_handler([&](const reindex_notification &note) { on_pre_reindex(note); }, *this);
      }

      void sql_serializer_plugin::plugin_startup()
      {
        ilog("sql::plugin_startup()");
      }

      void sql_serializer_plugin::plugin_shutdown()
      {
        ilog("Flushing left data...");
        my->join_data_processors();

        if(my->_on_pre_apply_block_con.connected())
          chain::util::disconnect_signal(my->_on_pre_apply_block_con);

        if (my->_on_post_apply_block_con.connected())
          chain::util::disconnect_signal(my->_on_post_apply_block_con);
        if (my->_on_pre_apply_operation_con.connected())
          chain::util::disconnect_signal(my->_on_pre_apply_operation_con);
        if (my->_on_starting_reindex.connected())
          chain::util::disconnect_signal(my->_on_starting_reindex);
        if (my->_on_finished_reindex.connected())
          chain::util::disconnect_signal(my->_on_finished_reindex);

        ilog("Done. Connection closed");
      }

      void sql_serializer_plugin::on_pre_apply_block(const block_notification& note)
      {
        ilog("Entering a resync data init for block: ${b}...", ("b", note.block_num));

        /// Let's init our database before applying first block (resync case)...
        my->init_database(note.block_num == 1, note.block_num);

        /// And disconnect to avoid subsequent inits
        if(my->_on_pre_apply_block_con.connected())
          chain::util::disconnect_signal(my->_on_pre_apply_block_con);
        ilog("Leaving a resync data init...");
      }

      void sql_serializer_plugin::on_pre_apply_operation(const operation_notification &note)
      {
        if(my->chain_db.is_producing())
        {
          ilog("Skipping operation processing coming from incoming transaction - waiting for already produced incoming block...");
          return;
        }

        if(my->skip_reversible_block(note.block))
          return;

        const bool is_virtual = hive::protocol::is_virtual_operation(note.op);

        detail::cached_containter_t &cdtf = my->currently_caching_data; // alias
        
        ++my->op_sequence_id;

        if(is_virtual == false)
          my->collect_new_ids(note.op);

        cdtf->operations.emplace_back(
            my->op_sequence_id,
            note.block,
            note.trx_in_block,
            is_virtual && note.trx_in_block < 0 ? my->block_vops++ : note.op_in_trx,
            note.op
          );

        my->collect_impacted_accounts(my->op_sequence_id, note.op);

      }

      void sql_serializer_plugin::on_post_apply_block(const block_notification &note)
      {
        FC_ASSERT(my->chain_db.is_producing() == false);

        if(my->skip_reversible_block(note.block_num))
          return;

        handle_transactions( note.block.transactions, note.block_num );

        my->currently_caching_data->total_size += note.block_id.data_size() + sizeof(note.block_num);
        my->currently_caching_data->blocks.emplace_back(
            note.block_id,
            note.block_num,
            note.block.timestamp,
            note.prev_block_id);
        my->block_vops = 0;

        if( note.block_num % my->blocks_per_commit == 0 )
        {
          my->process_cached_data();
        }

        if( note.block_num % 100'000 == 0 )
        {
          my->log_statistics();
        }
      }

      void sql_serializer_plugin::handle_transactions(const vector<hive::protocol::signed_transaction>& transactions, const int64_t block_num )
      {
        uint trx_in_block = 0;

        for( auto& trx : transactions )
        {
          auto hash = trx.id();
          size_t sig_size = trx.signatures.size();

          my->currently_caching_data->total_size += sizeof(hash) + sizeof(block_num) + sizeof(trx_in_block) +
                                                    sizeof(trx.ref_block_num) + sizeof(trx.ref_block_prefix) + sizeof(trx.expiration) + sizeof(trx.signatures[0]);

          my->currently_caching_data->transactions.emplace_back(
            hash,
            block_num,
            trx_in_block,
            trx.ref_block_num,
            trx.ref_block_prefix,
            trx.expiration,
            ( sig_size == 0 ) ? fc::optional<signature_type>() : fc::optional<signature_type>( trx.signatures[0] )
          );

          if( sig_size > 1 )
          {
            auto itr = trx.signatures.begin() + 1;
            while( itr != trx.signatures.end() )
            {
              my->currently_caching_data->transactions_multisig.emplace_back(
                hash,
                *itr
              );
              ++itr;
            }
          }

          trx_in_block++;
        }
      }

      void sql_serializer_plugin::on_pre_reindex(const reindex_notification &note)
      {
        ilog("Entering a reindex init...");
        /// Let's init our database before applying first block...
        my->init_database(note.force_replay, note.max_block_number);

        /// Disconnect pre-apply-block handler to avoid another initialization (for resync case).
        if(my->_on_pre_apply_block_con.connected())
          chain::util::disconnect_signal(my->_on_pre_apply_block_con);

        my->blocks_per_commit = 1'000;
        my->massive_sync = true;

        my->switch_to_concurrent_transaction_controller();

        ilog("Leaving a reindex init...");
      }

      void sql_serializer_plugin::on_post_reindex(const reindex_notification& note)
      {
        ilog("finishing from post reindex");
        
        my->process_cached_data();
        my->wait_for_data_processing_finish();

        if( note.last_block_number >= note.max_block_number )
          my->switch_db_items( true/*mode*/ );

        my->switch_to_single_transaction_controller();

        my->blocks_per_commit = 1;
        my->massive_sync = false;
      }
      
    } // namespace sql_serializer
  }    // namespace plugins
} // namespace hive
