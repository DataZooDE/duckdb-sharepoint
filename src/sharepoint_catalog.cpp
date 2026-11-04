// ATTACH 'sharepoint://sites/Sales' AS sales (TYPE sharepoint) (#60): a
// SharePoint site as a DuckDB catalog. One schema, `main`; its tables are the
// site's visible lists, typed from their column definitions. A table scans
// through read_sharepoint_list's table function (filter and projection
// pushdown included). See docs/hld.md §17.
//
// The catalog API is the same in DuckDB 1.5 and 2.0 except for names
// (std::string vs Identifier) and a few pure virtuals; those are handled
// without `override` or through duckdb_compat.
#include "sharepoint_catalog.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_list_functions.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

#include <cstdint>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

// --- 1.5 / 2.0 names -----------------------------------------------------------
namespace detail {
template <class I>
auto SetSchemaName(I &info, const string &name, int) -> decltype(info.schema = name, void()) {
	info.schema = name;
}
template <class I>
auto SetSchemaName(I &info, const string &name, long)
    -> decltype(info.SetQualifiedName({{duckdb::Identifier(name)}, duckdb::Identifier()}), void()) {
	info.SetQualifiedName({{duckdb::Identifier(name)}, duckdb::Identifier()});
}
template <class E>
auto ReturnType(const E &e, int) -> decltype(e.GetReturnType()) {
	return e.GetReturnType();
}
template <class E>
auto ReturnType(const E &e, long) -> decltype((e.return_type)) {
	return e.return_type;
}
} // namespace detail

//! An expression's type: Expression::return_type before 2.0, GetReturnType() in 2.0.
const LogicalType &ReturnType(const Expression &e) {
	return detail::ReturnType(e, 0);
}

//! A CreateSchemaInfo for a top-level schema called `name`.
void SetSchemaName(CreateSchemaInfo &info, const string &name) {
	detail::SetSchemaName(info, name, 0);
}

[[noreturn]] void ReadOnly(const string &what) {
	throw NotImplementedException("sharepoint: %s is not supported on an attached SharePoint site; manage lists in "
	                              "SharePoint, write items with COPY ... (FORMAT sharepoint_list)",
	                              what);
}

class SharepointSchema;

// --- Table: one list ------------------------------------------------------------
class SharepointTable : public TableCatalogEntry {
public:
	SharepointTable(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, string list_id_p,
	                string site_id_p)
	    // The copy is taken here, as an argument: TableCatalogEntry's
	    // constructor moves info.columns out, so copying in the member
	    // initializer copied an empty list (#60, seen live).
	    : SharepointTable(catalog, schema, info, info.columns.Copy(), std::move(list_id_p), std::move(site_id_p)) {
	}

	// Pure in 2.0 (the base no longer holds the columns); a non-virtual member
	// of the base before, which holds the same columns.
	const ColumnList &GetColumns() const {
		return own_columns;
	}

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &, column_t) override {
		return nullptr;
	}

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		vector<string> names;
		vector<LogicalType> types;
		bind_data = BindListForCatalog(context, list_id, site_id, names, types);
		// The catalog's columns were read at ATTACH; a list changed since then
		// would scan into the wrong columns.
		// The first difference goes in the message: "was X, now Y".
		string difference;
		const idx_t attached = own_columns.LogicalColumnCount();
		for (idx_t i = 0; difference.empty() && i < MaxValue<idx_t>(attached, names.size()); i++) {
			string was = "(none)", now = "(none)";
			if (i < attached) {
				auto &col = own_columns.GetColumn(LogicalIndex(i));
				was = duckdb_compat::Str(col.Name()) + " " + col.Type().ToString();
			}
			if (i < names.size()) {
				now = names[i] + " " + types[i].ToString();
			}
			if (!StringUtil::CIEquals(was, now)) {
				difference = StringUtil::Format("column %llu was %s, now %s", i + 1, was, now);
			}
		}
		if (!difference.empty()) {
			throw InvalidInputException("sharepoint: the columns of list '%s' changed since the site was attached (%s); "
			                            "DETACH and ATTACH it again",
			                            duckdb_compat::Str(name), difference);
		}
		return ListScanFunction();
	}

	TableStorageInfo GetStorageInfo(ClientContext &) override {
		return TableStorageInfo();
	}

	string list_id;
	string site_id;

private:
	SharepointTable(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, ColumnList columns,
	                string list_id_p, string site_id_p)
	    : TableCatalogEntry(catalog, schema, info), list_id(std::move(list_id_p)), site_id(std::move(site_id_p)),
	      own_columns(std::move(columns)) {
	}

	ColumnList own_columns;
};

// --- Schema `main`: the site's lists ------------------------------------------------
class SharepointSchema : public SchemaCatalogEntry {
public:
	SharepointSchema(Catalog &catalog, CreateSchemaInfo &info, string site_p)
	    : SchemaCatalogEntry(catalog, info), site(std::move(site_p)) {
	}

	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
		if (type != CatalogType::TABLE_ENTRY) {
			return;
		}
		Load(context);
		for (auto &table : tables) {
			callback(*table);
		}
	}
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
		// No context, no Graph: what is loaded already.
		if (type != CatalogType::TABLE_ENTRY) {
			return;
		}
		lock_guard<mutex> guard(lock);
		for (auto &table : tables) {
			callback(*table);
		}
	}

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup) override {
		if (lookup.GetCatalogType() != CatalogType::TABLE_ENTRY || !transaction.context) {
			return nullptr;
		}
		Load(*transaction.context);
		const auto &wanted = lookup.GetEntryName();
		for (auto &table : tables) {
			if (StringUtil::CIEquals(duckdb_compat::Str(table->name), wanted)) {
				return table.get();
			}
		}
		return nullptr;
	}

	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) override {
		ReadOnly("CREATE INDEX");
	}
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction, CreateFunctionInfo &) override {
		ReadOnly("CREATE FUNCTION");
	}
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction, BoundCreateTableInfo &) override {
		ReadOnly("CREATE TABLE");
	}
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction, CreateViewInfo &) override {
		ReadOnly("CREATE VIEW");
	}
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction, CreateSequenceInfo &) override {
		ReadOnly("CREATE SEQUENCE");
	}
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction, CreateTypeInfo &) override {
		ReadOnly("CREATE TYPE");
	}
	// Pure before 2.0 only: no `override`.
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
		ReadOnly("CREATE FUNCTION");
	}
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
		ReadOnly("CREATE FUNCTION");
	}
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
		ReadOnly("CREATE FUNCTION");
	}
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction, CreateCollationInfo &) {
		ReadOnly("CREATE COLLATION");
	}
	void DropEntry(ClientContext &, DropInfo &) override {
		ReadOnly("DROP");
	}
	void Alter(CatalogTransaction, AlterInfo &) override {
		ReadOnly("ALTER");
	}

private:
	//! The lists, once per attach (one Graph call per page of lists).
	void Load(ClientContext &context) {
		lock_guard<mutex> guard(lock);
		if (loaded) {
			return;
		}
		string site_id;
		auto lists = ListCatalogLists(context, site, site_id);
		case_insensitive_set_t used;
		for (auto &list : lists) {
			string table_name = list.name.empty() ? list.id : list.name;
			for (int n = 2; used.count(table_name); n++) {
				table_name = list.name + "_" + std::to_string(n);
			}
			used.insert(table_name);
			CreateTableInfo info(*this, duckdb_compat::FunctionName(table_name));
			for (idx_t i = 0; i < list.column_names.size(); i++) {
				info.columns.AddColumn(
				    ColumnDefinition(duckdb_compat::FunctionName(list.column_names[i]), list.column_types[i]));
			}
			tables.push_back(make_uniq<SharepointTable>(catalog, *this, info, list.id, site_id));
		}
		loaded = true;
	}

	string site;
	mutex lock;
	bool loaded = false;
	vector<unique_ptr<SharepointTable>> tables;
};

// --- INSERT / UPDATE / DELETE: items through the $batch writer ----------------------
enum class ModifyKind { INSERT, UPDATE, DELETE };

struct ModifyState : public GlobalSinkState {
	unique_ptr<ListItemWriter> writer;
	bool returned = false;
};

class SharepointModify : public PhysicalOperator {
public:
	SharepointModify(PhysicalPlan &plan, ModifyKind kind_p, SharepointTable &table_p, vector<string> names_p,
	                 vector<idx_t> source_p, vector<unique_ptr<Expression>> expressions_p, idx_t row_id_p,
	                 idx_t estimated_cardinality)
	    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, {LogicalType::BIGINT}, estimated_cardinality),
	      kind(kind_p), table(table_p), names(std::move(names_p)), source(std::move(source_p)),
	      expressions(std::move(expressions_p)), row_id(row_id_p) {
	}

	string GetName() const override {
		return kind == ModifyKind::INSERT ? "SHAREPOINT_INSERT"
		       : kind == ModifyKind::UPDATE ? "SHAREPOINT_UPDATE"
		                                    : "SHAREPOINT_DELETE";
	}

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		auto state = make_uniq<ModifyState>();
		state->writer =
		    make_uniq<ListItemWriter>(context, table.list_id, table.site_id, names, kind == ModifyKind::DELETE);
		return std::move(state);
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &state = input.global_state.Cast<ModifyState>();
		DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
			DataChunk values;
			if (kind == ModifyKind::UPDATE) {
				// The new values, as PhysicalUpdate computes them; the row id is the
				// child's last column.
				vector<LogicalType> types;
				for (auto &e : expressions) {
					types.push_back(ReturnType(*e));
				}
				values.Initialize(Allocator::Get(context.client), types);
				ExpressionExecutor executor(context.client, expressions);
				executor.Execute(chunk, values);
			}
			for (idx_t r = 0; r < chunk.size(); r++) {
				if (kind == ModifyKind::DELETE) {
					state.writer->Delete(chunk.GetValue(row_id, r));
					continue;
				}
				vector<Value> row;
				if (kind == ModifyKind::UPDATE) {
					for (idx_t c = 0; c < values.ColumnCount(); c++) {
						row.push_back(values.GetValue(c, r));
					}
					state.writer->Write(row, chunk.GetValue(chunk.ColumnCount() - 1, r));
				} else {
					for (auto idx : source) {
						row.push_back(chunk.GetValue(idx, r));
					}
					state.writer->Write(row, Value()); // INSERT always creates
				}
			}
		});
		return SinkResultType::NEED_MORE_INPUT;
	}

	SinkFinalizeType Finalize(Pipeline &, Event &, ClientContext &, OperatorSinkFinalizeInput &input) const override {
		auto &state = input.global_state.Cast<ModifyState>();
		DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, { state.writer->Flush(); });
		return SinkFinalizeType::READY;
	}

	SourceResultType GetDataInternal(ExecutionContext &, DataChunk &chunk, OperatorSourceInput &) const override {
		auto &state = sink_state->Cast<ModifyState>();
		if (!state.returned) {
			chunk.SetCardinality(1);
			chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(state.writer->Count())));
			state.returned = true;
		}
		return SourceResultType::FINISHED;
	}

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	bool IsSource() const override {
		return true;
	}

private:
	ModifyKind kind;
	SharepointTable &table;
	vector<string> names;              // the list columns written, in `source` / expression order
	vector<idx_t> source;              // INSERT: the child column of each name
	vector<unique_ptr<Expression>> expressions; // UPDATE: the new values
	idx_t row_id;                      // DELETE: the child column holding the row id
};

// --- Catalog ----------------------------------------------------------------------
class SharepointCatalog : public Catalog {
public:
	SharepointCatalog(AttachedDatabase &db, string path_p, string site_p)
	    : Catalog(db), path(std::move(path_p)), site(std::move(site_p)) {
	}

	void Initialize(bool) override {
		CreateSchemaInfo info;
		SetSchemaName(info, "main");
		main_schema = make_uniq<SharepointSchema>(*this, info, site);
	}
	string GetCatalogType() override {
		return "sharepoint";
	}
	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction, CreateSchemaInfo &) override {
		ReadOnly("CREATE SCHEMA");
	}
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction, const EntryLookupInfo &lookup,
	                                              OnEntryNotFound if_not_found) override {
		if (StringUtil::CIEquals(lookup.GetEntryName(), "main")) {
			return main_schema.get();
		}
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			return nullptr;
		}
		throw CatalogException("sharepoint: an attached SharePoint site has one schema, main, not '%s'",
		                       lookup.GetEntryName());
	}
	void ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) override {
		callback(*main_schema);
	}
	PhysicalOperator &PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
	                                    PhysicalOperator &) override {
		ReadOnly("CREATE TABLE AS");
	}
	PhysicalOperator &PlanInsert(ClientContext &, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override {
		if (op.return_chunk) {
			ReadOnly("INSERT ... RETURNING");
		}
		if (!plan) {
			ReadOnly("INSERT DEFAULT VALUES");
		}
		auto &table = op.table.Cast<SharepointTable>();
		// The columns the INSERT gives, and where each is in the child's chunk:
		// the others are left out, so SharePoint applies its own defaults.
		vector<string> names;
		vector<idx_t> source;
		const auto &columns = table.GetColumns();
		for (idx_t i = 0; i < columns.LogicalColumnCount(); i++) {
			idx_t at = i;
			if (!op.column_index_map.empty()) {
				at = op.column_index_map[PhysicalIndex(i)];
				if (at == DConstants::INVALID_INDEX) {
					continue;
				}
			}
			names.push_back(duckdb_compat::Str(columns.GetColumn(LogicalIndex(i)).Name()));
			source.push_back(at);
		}
		auto &insert = planner.Make<SharepointModify>(ModifyKind::INSERT, table, std::move(names), std::move(source),
		                                              vector<unique_ptr<Expression>>(), 0, op.estimated_cardinality);
		insert.children.push_back(*plan);
		return insert;
	}
	PhysicalOperator &PlanDelete(ClientContext &, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override {
		if (op.return_chunk) {
			ReadOnly("DELETE ... RETURNING");
		}
		auto &table = op.table.Cast<SharepointTable>();
		auto &ref = op.expressions[0]->Cast<BoundReferenceExpression>();
		auto &del = planner.Make<SharepointModify>(ModifyKind::DELETE, table, vector<string>(), vector<idx_t>(),
		                                           vector<unique_ptr<Expression>>(),
		                                           duckdb_compat::ReferenceIndex(ref), op.estimated_cardinality);
		del.children.push_back(plan);
		return del;
	}
	PhysicalOperator &PlanUpdate(ClientContext &, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override {
		if (op.return_chunk) {
			ReadOnly("UPDATE ... RETURNING");
		}
		auto &table = op.table.Cast<SharepointTable>();
		vector<string> names;
		for (auto &column : op.columns) {
			names.push_back(duckdb_compat::Str(table.GetColumns().GetColumn(LogicalIndex(column.index)).Name()));
		}
		for (auto &e : op.expressions) {
			if (e->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
				ReadOnly("UPDATE ... SET x = DEFAULT");
			}
		}
		auto &update = planner.Make<SharepointModify>(ModifyKind::UPDATE, table, std::move(names), vector<idx_t>(),
		                                              std::move(op.expressions), 0, op.estimated_cardinality);
		update.children.push_back(plan);
		return update;
	}
	DatabaseSize GetDatabaseSize(ClientContext &) override {
		return DatabaseSize();
	}
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return path;
	}
	void DropSchema(ClientContext &, DropInfo &) override {
		ReadOnly("DROP SCHEMA");
	}

private:
	string path;
	string site;
	unique_ptr<SharepointSchema> main_schema;
};

// --- Transactions: nothing to commit (reads go straight to Graph) ----------------
class SharepointTransaction : public Transaction {
public:
	SharepointTransaction(TransactionManager &manager, ClientContext &context) : Transaction(manager, context) {
	}
};

class SharepointTransactionManager : public TransactionManager {
public:
	explicit SharepointTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}
	Transaction &StartTransaction(ClientContext &context) override {
		auto transaction = make_uniq<SharepointTransaction>(*this, context);
		auto &result = *transaction;
		lock_guard<mutex> guard(lock);
		transactions[result] = std::move(transaction);
		return result;
	}
	ErrorData CommitTransaction(ClientContext &, Transaction &transaction) override {
		lock_guard<mutex> guard(lock);
		transactions.erase(transaction);
		return ErrorData();
	}
	void RollbackTransaction(Transaction &transaction) override {
		lock_guard<mutex> guard(lock);
		transactions.erase(transaction);
	}
	void Checkpoint(ClientContext &, bool) override {
	}

private:
	mutex lock;
	reference_map_t<Transaction, unique_ptr<Transaction>> transactions;
};

//! "sharepoint://sites/Sales" -> "sites/Sales"; "sharepoint://" -> "" (the
//! secret's own site). A Graph site id or "host:/sites/x" works too.
string SiteOfPath(const string &path) {
	string site = path;
	if (StringUtil::StartsWith(site, SHAREPOINT_SCHEME)) {
		site = site.substr(string(SHAREPOINT_SCHEME).size());
	}
	while (!site.empty() && site.back() == '/') {
		site.pop_back();
	}
	return site;
}

unique_ptr<Catalog> SharepointAttach(optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db,
                                     const string &, AttachInfo &info, AttachOptions &) {
	unique_ptr<Catalog> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		result = make_uniq<SharepointCatalog>(db, info.path, SiteOfPath(info.path));
	});
	return result;
}

unique_ptr<TransactionManager> SharepointCreateTransactionManager(optional_ptr<StorageExtensionInfo>,
                                                                 AttachedDatabase &db, Catalog &) {
	return make_uniq<SharepointTransactionManager>(db);
}

class SharepointStorageExtension : public StorageExtension {
public:
	SharepointStorageExtension() {
		attach = SharepointAttach;
		create_transaction_manager = SharepointCreateTransactionManager;
	}
};

} // namespace

void RegisterSharepointCatalog(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "sharepoint", make_shared_ptr<SharepointStorageExtension>());
}

} // namespace sharepoint
} // namespace duckdb
