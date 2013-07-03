/*-------------------------------------------------------------------------
 *
 * ifx_fdw.c
 *		  foreign-data wrapper for IBM INFORMIX databases
 *
 * Copyright (c) 2012, credativ GmbH
 *
 * IDENTIFICATION
 *		  informix_fdw/ifx_fdw.c
 *
 *-------------------------------------------------------------------------
 */


#include "ifx_fdw.h"
#include "ifx_node_utils.h"
#include "ifx_conncache.h"

#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "parser/parsetree.h"
#endif

#include "access/xact.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

/*
 * Object options using this wrapper module
 */
struct IfxFdwOption
{
	const char *optname;
	Oid         optcontext;
};

/*
 * Global per-backend transaction counter.
 */
extern unsigned int ifxXactInProgress;

/*
 * Valid options for informix_fdw.
 */
static struct IfxFdwOption ifx_valid_options[] =
{
	{ "informixserver",   ForeignServerRelationId },
	{ "informixdir",      ForeignServerRelationId },
	{ "user",             UserMappingRelationId },
	{ "password",         UserMappingRelationId },
	{ "database",         ForeignTableRelationId },
	{ "query",            ForeignTableRelationId },
	{ "table",            ForeignTableRelationId },
	{ "gl_datetime",      ForeignTableRelationId },
	{ "gl_date",          ForeignTableRelationId },
	{ "client_locale",    ForeignTableRelationId },
	{ "db_locale",        ForeignTableRelationId },
	{ "disable_predicate_pushdown", ForeignTableRelationId },
	{ "enable_blobs",               ForeignTableRelationId },
	{ NULL,                         ForeignTableRelationId }
};

/*
 * Data structure for intercall data
 * used by ifxGetConnections().
 */
struct ifx_sp_call_data
{
	HASH_SEQ_STATUS *hash_status;
	TupleDesc        tupdesc;
};

/*
 * informix_fdw handler and validator function
 */
extern Datum ifx_fdw_handler(PG_FUNCTION_ARGS);
extern Datum ifx_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ifx_fdw_handler);
PG_FUNCTION_INFO_V1(ifx_fdw_validator);
PG_FUNCTION_INFO_V1(ifxGetConnections);
PG_FUNCTION_INFO_V1(ifxCloseConnection);

/*******************************************************************************
 * FDW internal macros
 */

#if PG_VERSION_NUM < 90200
#define PG_SCANSTATE_PRIVATE_P(a) \
(List *) (FdwPlan *)(((ForeignScan *)(a)->ss.ps.plan)->fdwplan)->fdw_private
#else
#define PG_SCANSTATE_PRIVATE_P(a) \
(List *) ((ForeignScan *)(a)->ss.ps.plan)->fdw_private
#endif

/*******************************************************************************
 * FDW helper functions.
 */
static void ifxSetupFdwScan(IfxConnectionInfo    **coninfo,
							IfxFdwExecutionState **state,
							List    **plan_values,
							Oid       foreignTableOid,
							IfxForeignScanMode mode);

static IfxCachedConnection * ifxSetupConnection(IfxConnectionInfo **coninfo,
												Oid foreignTableOid,
												IfxForeignScanMode mode,
												bool error_ok);

static IfxFdwExecutionState *makeIfxFdwExecutionState(int refid);

static StringInfoData *
ifxFdwOptionsToStringBuf(Oid context);

static bool
ifxIsValidOption(const char *option, Oid context);

static void
ifxGetOptions(Oid foreigntableOid, IfxConnectionInfo *coninfo);

static StringInfoData *
ifxGetDatabaseString(IfxConnectionInfo *coninfo);

static StringInfoData *
ifxGenerateConnName(IfxConnectionInfo *coninfo);
static char *
ifxGenStatementName(IfxConnectionInfo *coninfo, int stmt_id);
static char *
ifxGenDescrName(IfxConnectionInfo *coninfo, int descr_id);

static void
ifxGetOptionDups(IfxConnectionInfo *coninfo, DefElem *def);

static void ifxConnInfoSetDefaults(IfxConnectionInfo *coninfo);

static IfxConnectionInfo *ifxMakeConnectionInfo(Oid foreignTableOid);

static char *ifxGenCursorName(IfxConnectionInfo *coninfo, int curid);

static void ifxPgColumnData(Oid foreignTableOid, IfxFdwExecutionState *festate);

static IfxSqlStateClass
ifxCatchExceptions(IfxStatementInfo *state, unsigned short stackentry);

static inline void ifxPopCallstack(IfxStatementInfo *info,
								   unsigned short stackentry);
static inline void ifxPushCallstack(IfxStatementInfo *info,
									unsigned short stackentry);

static void ifxColumnValueByAttNum(IfxFdwExecutionState *state, int attnum,
								   bool *isnull);


static void ifxPrepareCursorForScan(IfxStatementInfo *info,
									IfxConnectionInfo *coninfo);

static char *ifxFilterQuals(PlannerInfo *planInfo,
							RelOptInfo *baserel,
							List **excl_restrictInfo,
							Oid foreignTableOid);

static void ifxPrepareParamsForScan(IfxFdwExecutionState *state,
									IfxConnectionInfo *coninfo);

static IfxSqlStateClass
ifxFetchTuple(IfxFdwExecutionState *state);

static void
ifxGetValuesFromTuple(IfxFdwExecutionState *state,
					  TupleTableSlot *tupleSlot);

#if PG_VERSION_NUM >= 90300

static void ifxPrepareModifyQuery(IfxStatementInfo  *info,
								  IfxConnectionInfo *coninfo,
								  CmdType            operation);

static void ifxPrepareParamsForModify(IfxFdwExecutionState *state,
									  IfxConnectionInfo    *coninfo,
									  ModifyTable          *plan,
									  Oid                   foreignTableOid);

static void ifxColumnValuesToSqlda(IfxFdwExecutionState *state,
								   TupleTableSlot *slot,
								   int attnum);
static IfxFdwExecutionState *ifxCopyExecutionState(IfxFdwExecutionState *state);

#endif

static void ifx_fdw_xact_callback(XactEvent event, void *arg);
static void ifx_fdw_xact_callback_internal(IfxCachedConnection *cached,
										   XactEvent event);
static int ifxXactFinalize(IfxCachedConnection *cached,
						   IfxXactAction action,
						   bool connection_error_ok);

/*******************************************************************************
 * FDW callback routines.
 */

/*
 * Modifyable FDW API (Starting with PostgreSQL 9.3).
 */
#if PG_VERSION_NUM >= 90300

static void
ifxAddForeignUpdateTargets(Query *parsetree,
						   RangeTblEntry *target_rte,
						   Relation target_relation);
static List *
ifxPlanForeignModify(PlannerInfo *root,
					 ModifyTable *plan,
					 Index resultRelation,
					 int subplan_index);
static void
ifxBeginForeignModify(ModifyTableState *mstate,
					  ResultRelInfo *rinfo,
					  List *fdw_private,
					  int subplan_index,
					  int eflags);
static TupleTableSlot *
ifxExecForeignInsert(EState *estate,
					 ResultRelInfo *rinfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot);
static TupleTableSlot *
ifxExecForeignDelete(EState *estate,
					 ResultRelInfo *rinfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot);
static TupleTableSlot *
ifxExecForeignUpdate(EState *estate,
					 ResultRelInfo *rinfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot);

#endif

#if PG_VERSION_NUM >= 90200

static void ifxGetForeignRelSize(PlannerInfo *root,
								 RelOptInfo *baserel,
								 Oid foreignTableId);
static void ifxGetForeignPaths(PlannerInfo *root,
							   RelOptInfo *baserel,
							   Oid foreignTableId);
static ForeignScan *ifxGetForeignPlan(PlannerInfo *root,
									  RelOptInfo *baserel,
									  Oid foreignTableId,
									  ForeignPath *best_path,
									  List *tlist,
									  List *scan_clauses);

static int
ifxAcquireSampleRows(Relation relation, int elevel, HeapTuple *rows,
					 int targrows, double *totalrows, double *totaldeadrows);

static bool
ifxAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func,
					   BlockNumber *totalpages);

#else

static FdwPlan *ifxPlanForeignScan(Oid foreignTableOid,
								   PlannerInfo *planInfo,
								   RelOptInfo *baserel);

#endif

static void
ifxExplainForeignScan(ForeignScanState *node, ExplainState *es);

static void
ifxBeginForeignScan(ForeignScanState *node, int eflags);

static TupleTableSlot *ifxIterateForeignScan(ForeignScanState *node);

static void ifxReScanForeignScan(ForeignScanState *state);

static void ifxEndForeignScan(ForeignScanState *node);

static void ifxPrepareScan(IfxConnectionInfo *coninfo,
						   IfxFdwExecutionState *state);

/*******************************************************************************
 * SQL status and helper functions.
 */

Datum
ifxGetConnections(PG_FUNCTION_ARGS);
Datum
ifxCloseConnection(PG_FUNCTION_ARGS);

/*******************************************************************************
 * Implementation starts here
 */

#if PG_VERSION_NUM >= 90300

/*
 * Copies the specified IfxFdwExecutionState structure into
 * a new palloc'ed one, but without any stateful information.
 * This makes the returned pointer suitable to be used for
 * an additional scan state.

 * The refid of the origin state and its connection identifier will be
 * kept, but no statement or query information will be copied.
 */
static IfxFdwExecutionState *ifxCopyExecutionState(IfxFdwExecutionState *state)
{
	IfxFdwExecutionState *copy;

	Assert(state != NULL);

	/*
	 * Make a dummy execution state first, but keep the
	 * refid from the origin.
	 */
	copy = makeIfxFdwExecutionState(state->stmt_info.refid);

	/*
	 * Copy connection string...
	 */
	memcpy(copy->stmt_info.conname, state->stmt_info.conname, IFX_CONNAME_LEN + 1);

	/*
	 * ...and we're done.
	 */
	return copy;
}

/*
 * ifxColumnValueToSqlda()
 *
 * Does all the legwork to store the specified attribute
 * within the current Informix SQLDA structure.
 *
 * NOTE: attnum is the index into the internal state for
 *       the requested attribute. Thus, attnum == pg_attribute.attnum - 1!
 */
static void ifxColumnValuesToSqlda(IfxFdwExecutionState *state,
								   TupleTableSlot *slot,
								   int attnum)
{
	Assert(state != NULL && attnum >= 0);
	Assert(state->stmt_info.data != NULL);

	/*
	 * Call data conversion routine depending on the PostgreSQL
	 * builtin source type.
	 */
	switch(PG_ATTRTYPE_P(state, attnum))
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		{
			setIfxInteger(state, slot, attnum);
			break;
		}
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			break;
		default:
		{
			ifxRewindCallstack(&state->stmt_info);
			elog(ERROR, "informix_fdw: type \"%d\" is not supported for conversion",
				 state->stmt_info.ifxAttrDefs[attnum].type);
			break;
		}
	}
}

/*
 * Lookup the specified attribute number, obtain a column
 * identifier.
 *
 * Code borrowed from contrib/postgres_fdw.c
 */
char *dispatchColumnIdentifier(int varno, int varattno, PlannerInfo *root)
{
	char          *ident = NULL;
	RangeTblEntry *rte;
	List          *col_options;
	ListCell      *cell;

	/*
	 * Take take for special varnos!
	 */
	Assert(!IS_SPECIAL_VARNO(varno));

	rte = planner_rt_fetch(varno, root);

	/*
	 * Check out if this varattno has a special
	 * column_name value attached.
	 *
	 * TODO: SELECT statements currently don't honor ifx_column_name settings,
	 *       this issue will be adressed in the very near future!
	 */
	col_options = GetForeignColumnOptions(rte->relid, varattno);
	foreach(cell, col_options)
	{
		DefElem *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "ifx_column_name") == 0)
		{
			ident = defGetString(def);
			break; /* we're done */
		}
	}

	/*
	 * Rely on the local column identifier if no ifx_column_name
	 * was found.
	 */
	if (ident == NULL)
		ident = get_relid_attribute_name(rte->relid, varattno);

	return ident;
}

/*
 * ifxAddForeignUpdateTargets
 *
 * Injects a "rowid" column into the target list for
 * the remote table.
 *
 * NOTE:
 *
 * Informix doesn't always provide a "rowid" column for all
 * table types. Fragmented tables doesn't have a "rowid" per
 * default, so any attempts to update them will fail. If
 * fragmented tables are used in DML statements in foreign tables,
 * a explicit "rowid" column must be added.
 */
static void
ifxAddForeignUpdateTargets(Query *parsetree,
						   RangeTblEntry *target_rte,
						   Relation target_relation)
{
	Var         *var;
	TargetEntry *tle;

	var = makeVar(parsetree->resultRelation,
				  SelfItemPointerAttributeNumber,
				  TIDOID,
				  -1,
				  InvalidOid,
				  0);

	tle = makeTargetEntry((Expr *) var,
						  list_length(parsetree->targetList) + 1,
						  pstrdup("rowid"),
						  true);

	/* Finally add it to the target list */
	parsetree->targetList = lappend(parsetree->targetList, tle);
}

/*
 * ifxPlanForeignModify
 *
 * Plans a DML statement on a Informix foreign table.
 */
static List *
ifxPlanForeignModify(PlannerInfo *root,
					 ModifyTable *plan,
					 Index resultRelation,
					 int subplan_index)
{
	List          *result = NIL;
	CmdType        operation;
	RangeTblEntry *rte;
	Relation       rel;
	IfxFdwExecutionState *state;
	IfxConnectionInfo    *coninfo;
	IfxCachedConnection  *cached_handle;
	ForeignTable         *foreignTable;
	bool                  is_table;
	IfxSqlStateClass      errclass;
	ListCell             *elem;
	List                 *plan_values;
	ForeignScan          *foreignScan = NULL;
	char                 *cursor_name;

	elog(DEBUG3, "informix_fdw: plan foreign modify");

	/*
	 * Preliminary checks...we don't support updating foreign tables
	 * based on a SELECT.
	 */
	rte = planner_rt_fetch(resultRelation, root);
	foreignTable = GetForeignTable(rte->relid);
	operation = plan->operation;
	is_table = false;
	state    = NULL;

	foreach(elem, foreignTable->options)
	{
		DefElem *option = (DefElem *) lfirst(elem);
		if (strcmp(option->defname, "table") == 0)
			is_table = true;
	}

	if (!is_table)
	{
		ereport(ERROR, (errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
						errmsg("cannot modify foreign table \"%s\" which is based on a query",
							   get_rel_name(rte->relid))));
	}

	/*
	 * In case we have an UPDATE or DELETE action, retrieve the foreign scan state
	 * data belonging to the ForeignScan, initiated by the earlier scan node.
	 *
	 * We get this by referencing the corresponding RelOptInfo carried by
	 * the root PlannerInfo structure. This carries the execution state of the
	 * formerly created foreign scan, allowing us to access its current state.
	 *
	 * We need the cursor name later, to generate the WHERE CURRENT OF ... query.
	 */
	if ((operation == CMD_UPDATE)
		|| (operation == CMD_DELETE))
	{
		if ((resultRelation < root->simple_rel_array_size)
			&& (root->simple_rel_array[resultRelation] != NULL))
		{
			RelOptInfo *relInfo = root->simple_rel_array[resultRelation];
			IfxCachedConnection *cached;
			IfxFdwExecutionState *scan_state;

			/*
			 * Extract the state of the foreign scan.
			 */
			scan_state = (IfxFdwExecutionState *)
				((IfxFdwPlanState *)relInfo->fdw_private)->state;

			/*
			 * Don't reuse the connection info from the scan state,
			 * it will carry state information not usable for us.
			 */
			coninfo = ifxMakeConnectionInfo(rte->relid);

			/*
			 * Make the connection from the associated foreign scan current.
			 * Note: we use IFX_PLAN_SCAN to get a new refid used to
			 *       generate a new statement identifier.
			 */
			cached = ifxSetupConnection(&coninfo, rte->relid,
										IFX_PLAN_SCAN, true);

			/*
			 * Extract the scan state and copy it over into a new empty one,
			 * suitable to be used by this modify action.
			 */
			state = ifxCopyExecutionState(scan_state);

			/*
			 * The copied execution state kept the refid from the
			 * scan state obtained within the foreign scan. We need
			 * to prepare our own statement for the modify action, but
			 * the connection cache already will have generated one for us.
			 * Assign this to the copied execution state.
			 */
			state->stmt_info.refid = cached->con.usage;

			/*
			 * Since ifxCopyExecutionState() won't preserve stateful
			 * information, we need to do an extra step to copy
			 * the cursor name.
			 */
			state->stmt_info.cursor_name = pstrdup(scan_state->stmt_info.cursor_name);
		}
	}
	else
	{
		/*
		 * For an INSERT action, setup the foreign datasource from scratch
		 * (since no foreign scan is involved). We call ifxSetupFdwScan(),
		 * even if this is preparing a modify action on the informix table.
		 * This does all the legwork to initialize the database connection
		 * and associated handles. Note that we also establish a special INSERT
		 * cursor here feeded with the new values during ifxExecForeignInsert().
		 */
		ifxSetupFdwScan(&coninfo, &state, &plan_values, rte->relid, IFX_PLAN_SCAN);
	}

	/* Sanity check, should not happen */
	Assert((state != NULL) && (coninfo != NULL));

	/*
	 * Prepare params (retrieve affacted columns et al).
	 */
	ifxPrepareParamsForModify(state, coninfo, plan, rte->relid);

	/*
	 * Generate the query.
	 */
	switch (operation)
	{
		case CMD_INSERT:
			ifxGenerateInsertSql(state, coninfo, root, resultRelation);
			break;
		case CMD_DELETE:
			ifxGenerateDeleteSql(state, coninfo);
			break;
		case CMD_UPDATE:
			break;
		default:
			break;
	}

	/*
	 * Prepare and describe the statement.
	 */
	ifxPrepareModifyQuery(&state->stmt_info, coninfo, operation);

	/*
	 * Serialize all required plan data for use in executor later.
	 */
	result = ifxSerializePlanData(coninfo, state, root);

	return result;
}

/*
 * ifxPrepareModifyQuery()
 *
 * Prepares and describes the generated modify statement. Will
 * initialize the passed IfxStatementInfo structure with a valid
 * SQLDA structure.
 */
static void ifxPrepareModifyQuery(IfxStatementInfo *info,
								  IfxConnectionInfo *coninfo,
								  CmdType operation)
{
	/*
	 * Unique statement identifier.
	 */
	info->stmt_name = ifxGenStatementName(coninfo, info->refid);

	/*
	 * Prepare the query.
	 */
	elog(DEBUG1, "prepare query \"%s\"", info->query);
	ifxPrepareQuery(info->query,
					info->stmt_name);
	ifxCatchExceptions(info, IFX_STACK_PREPARE);

	/*
	 * In case of an INSERT command, we use an INSERT cursor.
	 */
	if ((operation == CMD_INSERT)
		|| (operation == CMD_UPDATE))
	{
		/*
		 * ...don't forget the cursor name.
		 */
		info->cursor_name = ifxGenCursorName(coninfo, info->refid);

		elog(DEBUG1, "declare cursor \"%s\" for statement \"%s\"",
			 info->cursor_name,
			 info->stmt_name);
		ifxDeclareCursorForPrepared(info->stmt_name, info->cursor_name,
									IFX_DEFAULT_CURSOR);
		ifxCatchExceptions(info, IFX_STACK_DECLARE);
	}
}

static void
ifxBeginForeignModify(ModifyTableState *mstate,
					  ResultRelInfo *rinfo,
					  List *fdw_private,
					  int subplan_index,
					  int eflags)
{
	IfxConnectionInfo    *coninfo;
	IfxFdwExecutionState *state;
	IfxCachedConnection  *cached_handle;
	Oid                   foreignTableOid;

	elog(DEBUG3, "informix_fdw: begin modify");
	foreignTableOid = RelationGetRelid(rinfo->ri_RelationDesc);

	/*
	 * Activate cached connection.
	 */
	cached_handle = ifxSetupConnection(&coninfo,
									   foreignTableOid,
									   IFX_BEGIN_SCAN,
									   true);

	/*
	 * Initialize an unassociated execution state handle (with refid -1).
	 */
	state = makeIfxFdwExecutionState(-1);

	/* Record current state structure */
	rinfo->ri_FdwState = state;

	/*
	 * Deserialize plan data.
	 */
	ifxDeserializeFdwData(state, fdw_private);

	/* EXPLAIN without ANALYZE... */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
	{
		elog(DEBUG1, "informix_fdw: explain only");
		return;
	}

	/*
	 * An INSERT action need to do much more preparing work
	 * than UPDATE/DELETE: Since no foreign scan is involved, the
	 * insert modify action need to prepare its own INSERT cursor and
	 * all other required stuff.
	 *
	 * UPDATE is a little smarter here. We rely on the cursor created
	 * during the foreign scan planning phase, but also need to prepare
	 * the UPDATE statement to bind column values later during execution.
	 * So there isn't any need to declare an UPDATE cursor additionally,
	 * but the SQLDA structure needs to be initialized nevertheless.
	 *
	 * DELETE doesn't need any special actions here, all we need for
	 * it is done in the planning phase (PREPARE).
	 */
	if (mstate->operation != CMD_DELETE)
	{
		/*
		 * Get column list for local table definition.
		 *
		 * XXX: Modify on a foreign Informix table relies on equally
		 *      named column identifiers.
		 */
		ifxPgColumnData(foreignTableOid, state);

		/*
		 * Describe the prepared statement into a SQLDA structure.
		 *
		 * This will return a valid SQLDA handle within our current
		 * IfxStatementInfo handle.
		 */
		elog(DEBUG1, "describe statement \"%s\"", state->stmt_info.stmt_name);
		ifxDescribeAllocatorByName(&state->stmt_info);
		ifxCatchExceptions(&state->stmt_info, IFX_STACK_ALLOCATE | IFX_STACK_DESCRIBE);

		/*
		 * Save number of prepared column attributes.
		 */
		state->stmt_info.ifxAttrCount = ifxDescriptorColumnCount(&state->stmt_info);
		elog(DEBUG1, "get descriptor column count %d",
			 state->stmt_info.ifxAttrCount);

		/*
		 * In case of an INSERT statement, open the associated
		 * cursor...
		 */
		elog(DEBUG1, "open cursor \"%s\"",
			 state->stmt_info.cursor_name);
		ifxOpenCursorForPrepared(&state->stmt_info);
		ifxCatchExceptions(&state->stmt_info, IFX_STACK_OPEN);

		state->stmt_info.ifxAttrDefs = palloc(state->stmt_info.ifxAttrCount
											  * sizeof(IfxAttrDef));

		/*
		 * Populate target column info array.
		 */
		if ((state->stmt_info.row_size = ifxGetColumnAttributes(&state->stmt_info)) == 0)
		{
			/* oops, no memory to allocate? Something surely went wrong,
			 * so abort */
			ifxRewindCallstack(&state->stmt_info);
			ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
							errmsg("could not initialize informix column properties")));
		}

		/*
		 * NOTE:
		 *
		 * ifxGetColumnAttributes() obtained all information about the
		 * returned column and stored them within the informix SQLDA and
		 * sqlvar structs. However, we don't want to allocate memory underneath
		 * our current memory context, thus we allocate the required memory structure
		 * on top here. ifxSetupDataBufferAligned() will assign the allocated
		 * memory area to the SQLDA structure and will maintain the data offsets
		 * properly aligned.
		 */
		state->stmt_info.data = (char *) palloc0(state->stmt_info.row_size);
		state->stmt_info.indicator = (short *) palloc0(sizeof(short)
												   * state->stmt_info.ifxAttrCount);

		/*
		 * Assign sqlvar pointers to the allocated memory area.
		 */
		ifxSetupDataBufferAligned(&state->stmt_info);
	}
}

static TupleTableSlot *
ifxExecForeignInsert(EState *estate,
					 ResultRelInfo *rinfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot)
{
	IfxFdwExecutionState *state;
	ListCell             *cell;
	int                   attnum;

	/*
	 * Setup action...
	 */
	state = rinfo->ri_FdwState;
	elog(DEBUG3, "informix_fdw: exec insert with cursor \"%s\"",
		 state->stmt_info.cursor_name);

	/*
	 * Copy column values into Informix SQLDA structure.
	 *
	 * NOTE:
	 * We preserve all columns in an INSERT statement.
	 */
	for (attnum = 0; attnum < state->pgAttrCount; attnum++)
	{
		/*
		 * Push all column value into the current Informix
		 * SQLDA structure, suitable to be executed later by
		 * PUT...
		 */
		if (state->pgAttrDefs[attnum].attnum > 0)
			ifxColumnValuesToSqlda(state, slot, state->pgAttrDefs[attnum].attnum - 1);
	}

	/*
	 * Execute the INSERT. Note that we have prepared
	 * an INSERT cursor the the planning phase before, re-using it
	 * here via PUT...
	 */
	ifxPutValuesInPrepared(&state->stmt_info);
	ifxCatchExceptions(&state->stmt_info, 0);

	return slot;
}

static TupleTableSlot *
ifxExecForeignDelete(EState *estate,
					 ResultRelInfo *rinfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot)
{
	IfxFdwExecutionState *state = rinfo->ri_FdwState;

	/*
	 * Setup action...
	 */
	state = rinfo->ri_FdwState;
	elog(DEBUG3, "informix_fdw: exec delete with statement \"%s\"",
		 state->stmt_info.stmt_name);

	/*
	 * Execute the DELETE action on the remote table. We just
	 * need to execute the prepared statement and we're done.
	 *
	 * The cursor should have already been positioned on the
	 * right tuple, the generated SQL query attached to the
	 * current execution state will just do a WHERE CURRENT OF
	 * to delete it.
	 */
	ifxExecuteStmt(&state->stmt_info);

	/*
	 * Check for errors.
	 */
	ifxCatchExceptions(&state->stmt_info, 0);

	/*
	 * And we're done.
	 */
	return slot;
}

static TupleTableSlot *
ifxExecForeignUpdate(EState *estate,
					 ResultRelInfo *rinfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot)
{
	IfxFdwExecutionState *state = rinfo->ri_FdwState;

	elog(DEBUG3, "informix_fdw: exec update");

	return slot;
}

static void ifxEndForeignModify(EState *estate,
								ResultRelInfo *rinfo)
{
	IfxFdwExecutionState *state = rinfo->ri_FdwState;

	elog(DEBUG3, "end foreign modify");

	/*
	 * If a cursor is in use, we must flush it. Only the
	 * case if we had an INSERT action, though...
	 */
	if (state->stmt_info.cursorUsage != IFX_NO_CURSOR)
	{
		ifxFlushCursor(&state->stmt_info);
	}

	/*
	 * Dispose any allocated resources.
	 */
	ifxRewindCallstack(&state->stmt_info);
}

/*
 * Prepare parameters for modify action.
 */
static void ifxPrepareParamsForModify(IfxFdwExecutionState *state,
									  IfxConnectionInfo    *coninfo,
									  ModifyTable          *plan,
									  Oid                   foreignTableOid)
{
	CmdType   operation = plan->operation;
	Relation  rel;

	/*
	 * Determine affected attributes of the modify action.
	 * No lock required, since the planner should already acquired
	 * one...
	 */
	rel = heap_open(foreignTableOid, NoLock);

	switch(operation)
	{
		case CMD_INSERT:
		{
			/*
			 * Retrieve attribute numbers for all columns. We apply all
			 * columns in an INSERT action.
			 */
			TupleDesc tupdesc = RelationGetDescr(rel);
			int       attnum;

			for (attnum = 1; attnum <= tupdesc->natts; attnum++)
			{
				Form_pg_attribute pgattr = tupdesc->attrs[attnum - 1];

				state->affectedAttrNums = lappend_int(state->affectedAttrNums,
													  pgattr->attnum);
			}

			/* ...and we're done */
			break;
		}
		case CMD_UPDATE:
		case CMD_DELETE:
		default:
			break;
	}

	heap_close(rel, NoLock);
}

#endif

/*
 * Allocates memory for the specified structures to
 * make the usable to store Informix values retrieved by
 * ifxGetValuesFromTuple().
 */
static void
ifxSetupTupleTableSlot(IfxFdwExecutionState *state,
					   TupleTableSlot *tupleSlot)
{

	Assert((tupleSlot != NULL) && (state != NULL));

	tupleSlot->tts_isempty = false;
	tupleSlot->tts_nvalid = state->pgAttrCount;
	tupleSlot->tts_values = (Datum *) palloc(sizeof(Datum)
											 * tupleSlot->tts_nvalid);
	tupleSlot->tts_isnull = (bool *) palloc(sizeof(bool)
											* tupleSlot->tts_nvalid);

}

/*
 * Converts the current fetched tuple from informix into
 * PostgreSQL datums and store them into the specified
 * TupleTableSlot.
 */
static void
ifxGetValuesFromTuple(IfxFdwExecutionState *state,
					  TupleTableSlot *tupleSlot)
{
	int i;

	/*
	 * Allocate slots for column value data.
	 *
	 * Used to retrieve Informix values by ifxColumnValueByAttnum().
	 */
	state->values = palloc0fast(sizeof(IfxValue)
								* state->stmt_info.ifxAttrCount);

	for (i = 0; i <= state->pgAttrCount - 1; i++)
	{
		bool isnull;

		elog(DEBUG5, "get column pg/ifx mapped attnum %d/%d",
			 i, PG_MAPPED_IFX_ATTNUM(state, i));

		/*
		 * It might happen that the FDW table has dropped
		 * columns...check for them and insert a NULL value instead..
		 */
		if (state->pgAttrDefs[i].attnum < 0)
		{
			tupleSlot->tts_isnull[i] = true;
			tupleSlot->tts_values[i] = PointerGetDatum(NULL);
			continue;
		}

		/*
		 * Retrieve a converted datum from the current
		 * column and store it within state context. This also
		 * sets and checks the indicator variable to record any
		 * NULL occurences.
		 */
		ifxColumnValueByAttNum(state, i, &isnull);

		/*
		 * Same for retrieved NULL values from informix.
		 */
		if (isnull)
		{
			/*
			 * If we encounter a NULL value from Informix where
			 * the local definition is NOT NULL, we throw an error.
			 *
			 * The PostgreSQL optimizer makes some assumptions about
			 * columns and their NULLability, so treat 'em accordingly.
			 */
			if (state->pgAttrDefs[i].attnotnull)
			{
				/* Reset remote resources */
				ifxRewindCallstack(&(state->stmt_info));
				elog(ERROR, "NULL value for column \"%s\" violates local NOT NULL constraint",
					 state->pgAttrDefs[i].attname);
			}

			tupleSlot->tts_isnull[i] = true;
			tupleSlot->tts_values[i] = PointerGetDatum(NULL);
			continue;
		}

		/*
		 * ifxColumnValueByAttnum() has already converted the current
		 * column value into a datum. We just need to assign it to the
		 * tupleSlot and we're done.
		 */
		tupleSlot->tts_isnull[i] = false;
		tupleSlot->tts_values[i] = state->values[PG_MAPPED_IFX_ATTNUM(state, i)].val;
	}
}

/*
 * Moves the cursor one row forward and fetches the tuple
 * into the internal SQLDA informix structure referenced
 * by the specified state handle.
 *
 * If the specified IfxFdwExecutionState was prepared with a
 * ReScan event, ifxFetchTuple() will set the cursor to
 * the first tuple, in case the current cursor is SCROLLable.
 * If not, the cursor is reopened for a rescan.
 */
static IfxSqlStateClass
ifxFetchTuple(IfxFdwExecutionState *state)
{

	/*
	 * Fetch tuple from cursor
	 */
	if (state->rescan)
	{
		if (state->stmt_info.cursorUsage == IFX_SCROLL_CURSOR)
			ifxFetchFirstRowFromCursor(&state->stmt_info);
		else
		{
			elog(DEBUG3, "re-opening informix cursor in rescan state");
			ifxCloseCursor(&state->stmt_info);
			ifxCatchExceptions(&state->stmt_info, 0);

			ifxOpenCursorForPrepared(&state->stmt_info);
			ifxCatchExceptions(&state->stmt_info, 0);

			ifxFetchRowFromCursor(&state->stmt_info);
		}
		state->rescan = false;
	}
	else
	{
		ifxFetchRowFromCursor(&state->stmt_info);
	}

	/*
	 * Catch any informix exception. We also need to
	 * check for IFX_NOT_FOUND, in which case no more rows
	 * must be processed.
	 */
	return ifxSetException(&(state->stmt_info));

}

/*
 * Entry point for scan preparation. Does all the leg work
 * for preparing the query and cursor definitions before
 * entering the executor.
 */
static void ifxPrepareScan(IfxConnectionInfo *coninfo,
						   IfxFdwExecutionState *state)
{
	/*
	 * Prepare parameters of the state structure
	 * for scan later.
	 */
	ifxPrepareParamsForScan(state, coninfo);

	/* Finally do the cursor preparation */
	ifxPrepareCursorForScan(&state->stmt_info, coninfo);
}

/*
 * Guts of connection establishing.
 *
 * Creates a new cached connection handle if not already cached
 * and sets the connection current. If already cached, make the
 * cached handle current, too.
 *
 * Returns the cached connection handle (either newly created or already
 * cached).
 */
static IfxCachedConnection * ifxSetupConnection(IfxConnectionInfo **coninfo,
												Oid foreignTableOid,
												IfxForeignScanMode mode,
												bool error_ok)
{
	IfxCachedConnection *cached_handle;
	bool                 conn_cached;
	IfxSqlStateClass     err;

	/*
	 * If not already done, initialize cache data structures.
	 */
	InformixCacheInit();

	/*
	 * Initialize connection structures and retrieve FDW options
	 */

	*coninfo = ifxMakeConnectionInfo(foreignTableOid);
	elog(DEBUG1, "informix connection dsn \"%s\"", (*coninfo)->dsn);

	/*
	 * Set requested scan mode.
	 */
	(*coninfo)->scan_mode = mode;

	/*
	 * Lookup the connection name in the connection cache.
	 */
	cached_handle = ifxConnCache_add(foreignTableOid, *coninfo, &conn_cached);

	/*
	 * Establish a new INFORMIX connection with transactions,
	 * in case a new one needs to be created. Otherwise make
	 * the requested connection current.
	 */
	if (!conn_cached)
	{
		ifxCreateConnectionXact(*coninfo);
		elog(DEBUG2, "created new cached informix connection \"%s\"",
			 (*coninfo)->conname);
	}
	else
	{
		/*
		 * Make the requested connection current.
		 */
		ifxSetConnection(*coninfo);
		elog(DEBUG2, "reusing cached informix connection \"%s\"",
			 (*coninfo)->conname);
	}

	/*
	 * Check connection status. This should happen directly
	 * after connection establishing, otherwise we might get confused by
	 * other ESQL API calls in the meantime.
	 */
	if ((err = ifxConnectionStatus()) != IFX_CONNECTION_OK)
	{
		if (err == IFX_CONNECTION_WARN)
		{
			IfxSqlStateMessage message;
			ifxGetSqlStateMessage(1, &message);

			ereport(WARNING, (errcode(WARNING),
							  errmsg("opened informix connection with warnings"),
							  errdetail("informix SQLSTATE %s: \"%s\"",
										message.sqlstate, message.text)));
		}

		if (err == IFX_CONNECTION_ERROR)
		{
			/*
			 * If we are here, something went wrong with connection
			 * establishing. Remove the already cached entry and force
			 * the connection to re-established again later.
			 */
			ifxConnCache_rm((*coninfo)->conname, &conn_cached);

			/* finally, error out */
			elog(error_ok ? ERROR : WARNING, "could not open connection to informix server: SQLCODE=%d",
				 ifxGetSqlCode());

			/* in case of !error_ok */
			return NULL;
		}
	}

	/*
	 * Give a notice if the connection supports transactions.
	 * Don't forget to register this information into the cached connection
	 * handle as well, since we didn't have this information available
	 * during connection startup and cached connection initialization.
	 *
	 * Also start a transaction. We do not care about the current state
	 * of the connection, ifxStartTransaction() does all necessary.
	 */
	if ((*coninfo)->tx_enabled == 1)
	{
		elog(DEBUG1, "informix database connection using transactions");
		cached_handle->con.tx_enabled = (*coninfo)->tx_enabled;

        /* ... and start the transaction */
        if (ifxStartTransaction(&cached_handle->con, *coninfo) < 0)
		{
			IfxSqlStateMessage message;
			ifxGetSqlStateMessage(1, &message);

			/*
			 * In case we can't emit a transaction, print a WARNING,
			 * but don't throw an error for now. We might do it
			 * the other way around, if that proves to be more correct,
			 * but leave it for now...
			 */
			elog(WARNING, "informix_fdw: could not start transaction: \"%s\", SQLSTATE %s",
				 message.text, message.sqlstate);
		}
		else
			RegisterXactCallback(ifx_fdw_xact_callback, NULL);
	}

	/* ...the same for ANSI mode */
	if ((*coninfo)->db_ansi == 1)
	{
		elog(DEBUG1, "informix database runs in ANSI-mode");
		cached_handle->con.db_ansi = (*coninfo)->db_ansi;
	}

	/*
	 * Give a warning if we have mismatching DBLOCALE settings.
	 */
	if (ifxGetSQLCAWarn(SQLCA_WARN_DB_LOCALE_MISMATCH) == 'W')
		elog(WARNING, "mismatching DBLOCALE \"%s\"",
			 (*coninfo)->db_locale);

	/*
	 * Give a NOTICE in case this is an INFORMIX SE
	 * database instance.
	 */
	if (ifxGetSQLCAWarn(SQLCA_WARN_NO_IFX_SE) == 'W')
		elog(NOTICE, "connected to an non-Informix SE instance");

	return cached_handle;
}

/*
 * Setup a foreign scan. This will initialize all
 * state and connection structures as well as the
 * connection cache.
 *
 * Never ever create or prepare any database visible
 * actions here!
 */
static void ifxSetupFdwScan(IfxConnectionInfo **coninfo,
							IfxFdwExecutionState **state,
							List    **plan_values,
							Oid       foreignTableOid,
							IfxForeignScanMode mode)
{
	IfxCachedConnection  *cached_handle;

	/*
	 * Activate the required Informix database connection.
	 */
	cached_handle = ifxSetupConnection(coninfo, foreignTableOid, mode, true);

	/*
	 * Save parameters for later use
	 * in executor.
	 */
	*plan_values = NIL;

	/*
	 * Make a generic informix execution state
	 * structure.
	 */
	*state = makeIfxFdwExecutionState(cached_handle->con.usage);
}

/*
 * Returns a fully initialized pointer to
 * an IfxFdwExecutionState structure. All pointers
 * are initialized to NULL.
 *
 * refid should be a unique number identifying the returned
 * structure throughout the backend.
 */
static IfxFdwExecutionState *makeIfxFdwExecutionState(int refid)
{
	IfxFdwExecutionState *state = palloc(sizeof(IfxFdwExecutionState));

	/* Assign the specified reference id. */
	state->stmt_info.refid = refid;

	bzero(state->stmt_info.conname, IFX_CONNAME_LEN + 1);
	state->stmt_info.cursorUsage = IFX_SCROLL_CURSOR;

	state->stmt_info.query        = NULL;
	state->stmt_info.predicate    = NULL;
	state->stmt_info.cursor_name  = NULL;
	state->stmt_info.stmt_name    = NULL;
	state->stmt_info.descr_name   = NULL;
	state->stmt_info.sqlda        = NULL;
	state->stmt_info.ifxAttrCount = 0;
	state->stmt_info.ifxAttrDefs  = NULL;
	state->stmt_info.call_stack   = IFX_STACK_EMPTY;
	state->stmt_info.row_size     = 0;
	state->stmt_info.special_cols = IFX_NO_SPECIAL_COLS;
	state->stmt_info.predicate    = NULL;

	bzero(state->stmt_info.sqlstate, 6);
	state->stmt_info.exception_count = 0;

	state->pgAttrCount = 0;
	state->pgAttrDefs  = NULL;
	state->values = NULL;
	state->rescan = false;
	state->affectedAttrNums = NIL;

	return state;
}

#if PG_VERSION_NUM >= 90200

/*
 * Callback for ANALYZE
 */
static bool
ifxAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func,
					   BlockNumber *totalpages)
{
	IfxConnectionInfo    *coninfo;
	IfxCachedConnection  *cached_handle;
	IfxFdwExecutionState *state;
	ForeignTable         *foreignTable;
	ListCell             *elem;
	bool                  is_table;
	IfxPlanData           planData;
	IfxSqlStateClass      errclass;

	/*
	 * Examine wether query or table is specified to form
	 * the foreign table. In case we get a query, don't allow
	 * ANALYZE to be run...
	 */
	foreignTable = GetForeignTable(RelationGetRelid(relation));
	is_table     = false;
	*totalpages  = 1;

	foreach(elem, foreignTable->options)
	{
		DefElem *def = (DefElem *) lfirst(elem);
		if (strcmp(def->defname, "table") == 0)
			is_table = true;
	}

	/*
	 * We don't support analyzing a foreign table which is based
	 * on a SELECT. Proceed only in case coninfo->table is specified.
	 *
	 * We cannot simply error out here, since in case someone wants
	 * to ANALYZE a whole database this will abort the whole run...
	 *
	 * XXX: However, it might have already cached a database connection. Leave
	 * it for now, but we might want to close it, not sure...
	 */
	if (!is_table)
	{
		/* analyze.c already prints a WARNING message, so leave it out here */
		return false;
	}

	/*
	 * Retrieve a connection from cache or open a new one. Instruct
	 * an IFX_PLAN_SCAN, since we treat ifxAnalyzeForeignTable() which
	 * does all the setup required to do ifxAcquireSampleRows() separately.
	 *
	 * XXX: should we error out in case we get an connection error?
	 *      This will abandon the whole ANALYZE run when
	 *      issued against the whole database...
	 */
	if ((cached_handle = ifxSetupConnection(&coninfo,
											RelationGetRelid(relation),
											IFX_PLAN_SCAN,
											false)) == NULL)
	{
		/*
		 * again, analyze.c will print a "skip message" in case we abort
		 * this ANALYZE round, but give the user a hint what actually happened
		 * as an additional WARNING.
		 *
		 * Safe to exit here, since no database visible changes are made so far.
		 */
		ereport(WARNING,
				(errmsg("cannot establish remote database connection"),
				 errdetail("error retrieving or creating cached connection handle")));
		return false;
	}

	/*
	 * Catch any possible errors. Create a generic execution state which
	 * will carry any possible exceptions.
	 */
	state = makeIfxFdwExecutionState(cached_handle->con.usage);

	/*
	 * Retrieve basic statistics from Informix for this table,
	 * calculate totalpages according to them.
	 */
	ifxGetSystableStats(coninfo->tablename, &planData);

	/*
	 * Suppress any ERRORs, we don't want to interrupt a database-wide
	 * ANALYZE run...
	 */
	errclass = ifxSetException(&(state->stmt_info));

	if (errclass != IFX_SUCCESS)
	{

		if (errclass == IFX_NOT_FOUND)
		{
			/* no data found, use default 1 page
			 *
			 * XXX: could that really happen??
			 * systable *should* have a matching tuple for this
			 * table...
			 */
			elog(DEBUG1, "informix fdw: no remote stats data found for table \"%s\"",
				 RelationGetRelationName(relation));
		}

		/*
		 * All other error/warning cases should be catched. We do
		 * this here to suppress any ERROR, since we don't want to
		 * abandon a database-wise ANALYZE run...
		 *
		 * XXX: Actually i don't like this coding, maybe its better
		 *      to change ifxCatchExceptions() to mark any errors to
		 *      be ignored...
		 */
		PG_TRY();
		{
			ifxCatchExceptions(&(state->stmt_info), 0);
		}
		PG_CATCH();
		{
			IfxSqlStateMessage message;

			ifxGetSqlStateMessage(1, &message);
			ereport(WARNING, (errcode(ERRCODE_FDW_ERROR),
							  errmsg("informix FDW warning: \"%s\"",
									 message.text),
							  errdetail("SQLSTATE %s", message.sqlstate)));
		}
		PG_END_TRY();
	}
	else
	{
		elog(DEBUG2, "informix_fdw \"%s\" stats(nrows, npused, rowsize, pagesize): %2f, %2f, %d, %d",
			 RelationGetRelationName(relation), planData.nrows,
			 planData.npages, planData.row_size, planData.pagesize);

		/*
		 * Calculate and convert statistics information to
		 * match expectations of PostgreSQL...
		 *
		 * Default Informix installations run with 2KB block size
		 * but this could be configured depending on the tablespace.
		 *
		 * The idea is to calculate the numbers of pages to match
		 * the blocksize PostgreSQL currently uses to get a smarter
		 * cost estimate, thus the following formula is used:
		 *
		 * (npages * pagesize) / BLCKSZ
		 *
		 * If npage * pagesize is less than BLCKSZ, but the row estimate
		 * returned show a number larger than 0, we assume one block.
		 */
		if (planData.nrows > 0)
		{
			*totalpages
				= (BlockNumber) ((((planData.npages * planData.pagesize) / BLCKSZ) < 1)
								 ? 1
								 : (planData.npages * planData.pagesize) / BLCKSZ);
		}
		else
			*totalpages = 0;

		elog(DEBUG1, "totalpages = %d", *totalpages);
	}

	*func = ifxAcquireSampleRows;
	return true;
}

/*
 * Internal function for ANALYZE callback
 *
 * This is essentially the guts for ANALYZE <foreign table>
 */
static int
ifxAcquireSampleRows(Relation relation, int elevel, HeapTuple *rows,
					 int targrows, double *totalrows, double *totaldeadrows)
{
	Oid foreignTableId;
	IfxConnectionInfo *coninfo;
	IfxFdwExecutionState *state;
	List                 *plan_values;
	double                anl_state;
	IfxSqlStateClass      errclass;
	TupleDesc             tupDesc;
	Datum                *values;
	bool                 *nulls;
	int                   rows_visited;
	int                   rows_to_skip;

	elog(DEBUG1, "informix_fdw: analyze");

	/*
	 * Initialize stuff
	 */
	*totalrows      = 0;
	*totaldeadrows = 0;
	rows_visited   = 0;
	rows_to_skip   = -1; /* not set yet */
	foreignTableId = RelationGetRelid(relation);

	/*
	 * Establish a connection to the Informix server
	 * or get a previously cached one...there should
	 * already be a cached connection for this table, if
	 * ifxAnalyzeForeignTable() found some remote
	 * statistics to be reused.
	 *
	 * NOTE:
	 *
	 * ifxAnalyzeForeignTable should have prepare all required
	 * steps to prepare the scan finally, so we don't need to
	 * get a new scan refid...thus we pass IFX_BEGIN_SCAN to
	 * tell the connection cache that everything is already
	 * in place.
	 *
	 * This also initializes all required infrastructure
	 * to scan the remote table.
	 */
	ifxSetupFdwScan(&coninfo, &state, &plan_values,
					foreignTableId, IFX_BEGIN_SCAN);

	/*
	 * XXX: Move this into a separate function, shared
	 * code with ifxBeginForeignScan()!!!
	 */

	/*
	 * Prepare the scan. This creates a cursor we can use to
	 */
	ifxPrepareScan(coninfo, state);

	/*
	 * Get column definitions for local table...
	 */
	ifxPgColumnData(foreignTableId, state);

	/*
	 * Populate the DESCRIPTOR area, required to get
	 * the column values later...
	 */
	elog(DEBUG1, "populate descriptor area for statement \"%s\"",
		 state->stmt_info.stmt_name);
	ifxDescribeAllocatorByName(&state->stmt_info);
	ifxCatchExceptions(&state->stmt_info, IFX_STACK_ALLOCATE | IFX_STACK_DESCRIBE);

	/*
	 * Get the number of columns.
	 */
	state->stmt_info.ifxAttrCount = ifxDescriptorColumnCount(&state->stmt_info);
	elog(DEBUG1, "get descriptor column count %d",
		 state->stmt_info.ifxAttrCount);
	ifxCatchExceptions(&state->stmt_info, 0);

	/*
	 * XXX: It makes no sense to have a local column list with *more*
	 * columns than the remote table. I can't think of any use case
	 * for this atm, anyone?
	 */
	if (PG_VALID_COLS_COUNT(state) > state->stmt_info.ifxAttrCount)
	{
		ifxRewindCallstack(&(state->stmt_info));
		ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
						errmsg("foreign table \"%s\" has more columns than remote source",
							   get_rel_name(foreignTableId))));
	}

	state->stmt_info.ifxAttrDefs = palloc(state->stmt_info.ifxAttrCount
										  * sizeof(IfxAttrDef));

	/*
	 * Populate result set column info array.
	 */
	if ((state->stmt_info.row_size = ifxGetColumnAttributes(&state->stmt_info)) == 0)
	{
		/* oops, no memory to allocate? Something surely went wrong,
		 * so abort */
		ifxRewindCallstack(&state->stmt_info);
		ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
						errmsg("could not initialize informix column properties")));
	}

	/*
	 * NOTE:
	 *
	 * ifxGetColumnAttributes() obtained all information about the
	 * returned column and stored them within the informix SQLDA and
	 * sqlvar structs. However, we don't want to allocate memory underneath
	 * our current memory context, thus we allocate the required memory structure
	 * on top here. ifxSetupDataBufferAligned() will assign the allocated
	 * memory area to the SQLDA structure and will maintain the data offsets
	 * properly aligned.
	 */
	state->stmt_info.data = (char *) palloc0(state->stmt_info.row_size);
	state->stmt_info.indicator = (short *) palloc0(sizeof(short)
												   * state->stmt_info.ifxAttrCount);

	/*
	 * Assign sqlvar pointers to the allocated memory area.
	 */
	ifxSetupDataBufferAligned(&state->stmt_info);

	/*
	 * Open the cursor.
	 */
	elog(DEBUG1, "open cursor \"%s\"",
		 state->stmt_info.cursor_name);
	ifxOpenCursorForPrepared(&state->stmt_info);
	ifxCatchExceptions(&state->stmt_info, IFX_STACK_OPEN);

	/*
	 * Okay, we are ready to read the tuples from the remote
	 * table now.
	 */
	anl_state = anl_init_selection_state(targrows);

	/*
	 * Prepare tuple...
	 */
	tupDesc = RelationGetDescr(relation);

	/* XXX: might differ, if we have dynamic target list
	 *      some time in the future */
	values  = (Datum *) palloc(state->pgAttrCount
							   * sizeof(Datum));
	nulls   = (bool *) palloc(state->pgAttrCount
							  * sizeof(bool));

	/*
	 * Allocate the data buffer structure required to
	 * extract column values via our API...
	 */
	state->values = palloc(sizeof(IfxValue)
						   * state->stmt_info.ifxAttrCount);

	/* Start the scan... */
	ifxFetchRowFromCursor(&(state->stmt_info));

	/*
	 * Catch exception, especially IFX_NOT_FOUND...
	 */
	errclass = ifxSetException(&(state->stmt_info));

	while (errclass == IFX_SUCCESS)
	{
		int i;

		*totalrows += 1;

		/*
		 * Allow delay...
		 */
		vacuum_delay_point();

		/*
		 * Read the tuple...
		 */
		for (i = 0; i <= state->pgAttrCount - 1; i++)
		{
			bool isnull;

			elog(DEBUG5, "get column pg/ifx mapped attnum %d/%d",
				 i, PG_MAPPED_IFX_ATTNUM(state, i));

			/* ignore dropped columns */
			if (state->pgAttrDefs[i].attnum < 0)
			{
				values[i] = PointerGetDatum(NULL);
				nulls[i]  = true;
				continue;
			}

			/*
			 * Get the converted value from Informix
			 * (we get a PostgreSQL datum from the conversion
			 * routines, suitable to be assigned directly to our
			 * values array).
			 */
			ifxColumnValueByAttNum(state, i, &isnull);

			/*
			 * Take care for NULL returned by Informix.
			 */
			if (isnull)
			{
				values[i] = PointerGetDatum(NULL);
				nulls[i]  = true;
				continue;
			}

			/*
			 * If a datum is not NULL, ifxColumnValueByAttNum()
			 * had converted the column value into a proper
			 * PostgreSQL datum.
			 */
			nulls[i] = false;
			values[i] = state->values[PG_MAPPED_IFX_ATTNUM(state, i)].val;
		}

		/*
		 * Built a HeapTuple object from the current row.
		 */
		if (rows_visited < targrows)
		{
			rows[rows_visited++] = heap_form_tuple(tupDesc, values, nulls);
		}
		else
		{
			/*
			 * Follow Vitter's algorithm as defined in
			 * src/backend/command/analyze.c.
			 *
			 * See function acquire_sample_rows() for details.
			 *
			 */

			if (rows_to_skip < 0)
				rows_to_skip = anl_get_next_S(*totalrows, targrows, &anl_state);

			if (rows_to_skip <= 0)
			{
				/*
				 * Found a suitable tuple, replace
				 * a random tuple within the rows array
				 */
				int k = (int) (targrows * anl_random_fract());
				Assert(k >= 0 && k < targrows);

				/* Free the old tuple */
				heap_freetuple(rows[k]);

				/* Assign a new one... */
				rows[k] = heap_form_tuple(tupDesc, values, nulls);
			}

			rows_to_skip -= 1;
		}

		/*
		 * Next one ...
		 */
		ifxFetchRowFromCursor(&(state->stmt_info));
		errclass = ifxSetException(&(state->stmt_info));
	}

	/* Done, cleanup ... */
	ifxRewindCallstack(&state->stmt_info);

	ereport(elevel,
			(errmsg("\"%s\": remote Informix table contains %.0f rows; "
					"%d rows in sample",
					RelationGetRelationName(relation),
					*totalrows, rows_visited)));

	return rows_visited;
}

/*
 * Get the foreign informix relation estimates. This function
 * is also responsible to setup the informix database connection
 * and create a corresponding cached connection, if not already
 * done.
 */
static void ifxGetForeignRelSize(PlannerInfo *planInfo,
								 RelOptInfo *baserel,
								 Oid foreignTableId)
{
	IfxConnectionInfo    *coninfo;
	List                 *plan_values;
	IfxFdwExecutionState *state;
	IfxFdwPlanState      *planState;

	elog(DEBUG3, "informix_fdw: get foreign relation size, cmd %d",
		planInfo->parse->commandType);

	planState = palloc(sizeof(IfxFdwPlanState));

	/*
	 * Establish remote informix connection or get
	 * a already cached connection from the informix connection
	 * cache.
	 */
	ifxSetupFdwScan(&coninfo, &state, &plan_values,
					foreignTableId, IFX_PLAN_SCAN);

	/*
	 * Check for predicates that can be pushed down
	 * to the informix server, but skip it in case the user
	 * has set the disable_predicate_pushdown option...
	 */
	if (coninfo->predicate_pushdown)
	{
		/*
		 * Also save a list of excluded RestrictInfo structures not carrying any
		 * predicate found to be pushed down by ifxFilterQuals(). Those will
		 * passed later to ifxGetForeignPlan()...
		 */
		state->stmt_info.predicate = ifxFilterQuals(planInfo, baserel,
													&(planState->excl_restrictInfo),
													foreignTableId);
		elog(DEBUG2, "predicate for pushdown: %s", state->stmt_info.predicate);
	}
	else
	{
		elog(DEBUG2, "predicate pushdown disabled");
		state->stmt_info.predicate = "";
	}

	/*
	 * Establish the remote query on the informix server. To do this,
	 * we create the cursor, which will allow us to get the cost estimates
	 * informix calculates for the query execution. We _don't_ open the
	 * cursor yet, this is left to the executor later.
	 *
	 * If we have an UPDATE or DELETE query, the foreign scan needs to
	 * employ an FOR UPDATE cursor, since we are going to reuse it
	 * during modify.
	 *
	 * There's also another difficulty here: We might have a non-logged
	 * remote Informix database here and BLOBs might be used (indicated by
	 * the FDW table option enable_blobs). This means we must force
	 * a non-SCROLL cursor with FOR UPDATE here. Also note that
	 * ifxBeginForeignScan() *will* error out in case we scan a remote
	 * table with BLOBs but without having enable_blobs. We can't do this
	 * sanity check here, since we currently don't have any idea how the
	 * result set from the remote table looks like yet. So just make sure
	 * we select the right cursor type for now, delaying the error check
	 * to the execution phase later.
	 *
	 * This must happen before calling ifxPrepareScan(), this this will
	 * generate the SELECT query passed to the cursor later on!
	 */
	if ((planInfo->parse->commandType == CMD_UPDATE)
		|| (planInfo->parse->commandType == CMD_DELETE))
	{
		if (coninfo->enable_blobs)
			state->stmt_info.cursorUsage = IFX_UPDATE_CURSOR;
		else
			state->stmt_info.cursorUsage = IFX_SCROLL_UPDATE_CURSOR;
	}

	ifxPrepareScan(coninfo, state);

	/*
	 * Now it should be possible to get the cost estimates
	 * from the actual cursor.
	 */
	coninfo->planData.estimated_rows = (double) ifxGetSQLCAErrd(SQLCA_NROWS_PROCESSED);
	coninfo->planData.costs          = (double) ifxGetSQLCAErrd(SQLCA_NROWS_WEIGHT);

	/*
	 * Estimate total_cost in conjunction with the per-tuple cpu cost
	 * for FETCHing each particular tuple later on.
	 */
	coninfo->planData.total_costs    = coninfo->planData.costs
		+ (coninfo->planData.estimated_rows * cpu_tuple_cost);

	/* should be calculated nrows from foreign table */
	baserel->rows        = coninfo->planData.estimated_rows;
	planState->coninfo   = coninfo;
	planState->state     = state;
	baserel->fdw_private = (void *) planState;
}

/*
 * Create possible access paths for the foreign data
 * scan. Consider any pushdown predicate and create
 * an appropiate path for it.
 */
static void ifxGetForeignPaths(PlannerInfo *root,
							   RelOptInfo *baserel,
							   Oid foreignTableId)
{
	IfxFdwPlanState *planState;

	elog(DEBUG3, "informix_fdw: get foreign paths");

	planState = (IfxFdwPlanState *) baserel->fdw_private;

	/*
	 * Create a generic foreign path for now. We need to consider any
	 * restriction quals later, to get a smarter path generation here.
	 *
	 * For example, it is quite interesting to consider any index scans
	 * or sorted output on the remote side and reflect it in the
	 * choosen paths (helps nested loops et al.).
	 */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
									 baserel->rows,
									 planState->coninfo->planData.costs,
									 planState->coninfo->planData.total_costs,
									 NIL,
									 NULL,
									 NIL));
}

static ForeignScan *ifxGetForeignPlan(PlannerInfo *root,
									  RelOptInfo *baserel,
									  Oid foreignTableId,
									  ForeignPath *best_path,
									  List *tlist,
									  List *scan_clauses)
{
	Index scan_relid;
	IfxFdwPlanState  *planState;
	List             *plan_values;

	elog(DEBUG3, "informix_fdw: get foreign plan");

	scan_relid = baserel->relid;
	planState = (IfxFdwPlanState *) baserel->fdw_private;

	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * Serialize current plan data into a format suitable
	 * for copyObject() later. This is required to be able to
	 * push down the collected information here down to the
	 * executor.
	 */
	plan_values = ifxSerializePlanData(planState->coninfo,
									   planState->state,
									   root);

	return make_foreignscan(tlist,
							scan_clauses,
							scan_relid,
							NIL,
							plan_values);
}

#else

/*
 * ifxPlanForeignScan
 *
 * Plans a foreign scan on an remote informix relation.
 */
static FdwPlan *
ifxPlanForeignScan(Oid foreignTableOid, PlannerInfo *planInfo, RelOptInfo *baserel)
{
	IfxConnectionInfo    *coninfo;
	FdwPlan              *plan;
	List                 *plan_values;
	IfxFdwExecutionState *state;
	List                 *excl_restrictInfo;

	elog(DEBUG3, "informix_fdw: plan scan");

	/*
	 * Prepare a generic plan structure
	 */
	plan = makeNode(FdwPlan);

	/*
	 * Establish remote informix connection or get
	 * a already cached connection from the informix connection
	 * cache.
	 */
	ifxSetupFdwScan(&coninfo, &state, &plan_values,
					foreignTableOid, IFX_PLAN_SCAN);

	/*
	 * Check for predicates that can be pushed down
	 * to the informix server, but skip it in case the user
	 * has set the disable_predicate_pushdown option...
	 */
	if (coninfo->predicate_pushdown)
	{
		state->stmt_info.predicate = ifxFilterQuals(planInfo, baserel,
													&excl_restrictInfo,
													foreignTableOid);
		elog(DEBUG2, "predicate for pushdown: %s", state->stmt_info.predicate);
	}
	else
	{
		elog(DEBUG2, "predicate pushdown disabled");
		state->stmt_info.predicate = "";
	}

	/*
	 * Prepare parameters of the state structure
	 * and cursor definition.
	 */
	ifxPrepareScan(coninfo, state);

	/*
	 * After declaring the cursor we are able to retrieve
	 * row and cost estimates via SQLCA fields. Do that and save
	 * them into the IfxPlanData structure member of
	 * IfxConnectionInfo and, more important, assign the
	 * values to our plan node.
	 */
	coninfo->planData.estimated_rows = (double) ifxGetSQLCAErrd(SQLCA_NROWS_PROCESSED);
	coninfo->planData.costs          = (double) ifxGetSQLCAErrd(SQLCA_NROWS_WEIGHT);
	baserel->rows = coninfo->planData.estimated_rows;
	plan->startup_cost = 0.0;
	plan->total_cost = coninfo->planData.costs + plan->startup_cost;

	/*
	 * Save parameters to our plan. We need to make sure they
	 * are copyable by copyObject(), so use a list with
	 * bytea const nodes.
	 *
	 * NOTE: we *must* not allocate serialized nodes within
	 *       the current memory context, because this will crash
	 *       on prepared statements and subsequent EXECUTE calls
	 *       since they will be freed after. Instead, use the
	 *       planner context, which will remain as long as
	 *       the plan exists.
	 *
	 */
	plan_values = ifxSerializePlanData(coninfo, state, planInfo);
	plan->fdw_private = plan_values;

	return plan;
}

#endif

/*
 * ifxPushCallstack()
 *
 * Updates the call stack with the new
 * stackentry.
 */
static inline void ifxPushCallstack(IfxStatementInfo *info,
									unsigned short stackentry)
{
	if (stackentry == 0)
		return;
	info->call_stack |= stackentry;
}

/*
 * ifxPopCallstack()
 *
 * Sets the status of the call stack to the
 * given state.
 */
static inline void ifxPopCallstack(IfxStatementInfo *info,
								   unsigned short stackentry)
{
	info->call_stack &= ~stackentry;
}

/*
 * ifxRewindCallstack()
 *
 * Gets the call back and tries to free
 * all resources associated with the call stack
 * in the given state.
 */
void ifxRewindCallstack(IfxStatementInfo *info)
{
	/*
	 * NOTE: IFX_STACK_DESCRIBE doesn't need any special handling here,
	 * so just ignore it until the end of rewinding the call stack
	 * and set it to IFX_STACK_EMPTY if everything else is undone.
	 */

	if ((info->call_stack & IFX_STACK_OPEN) == IFX_STACK_OPEN)
	{
		ifxCloseCursor(info);
		elog(DEBUG2, "informix_fdw: undo open");
		ifxPopCallstack(info, IFX_STACK_OPEN);
	}

	if ((info->call_stack & IFX_STACK_ALLOCATE) == IFX_STACK_ALLOCATE)
	{
		/*
		 * Deallocating allocated memory by sqlda data structure
		 * is going to be little tricky here: sqlda is allocated
		 * by the Informix ESQL/C API, so we don't have any influence
		 * via memory contexts...we aren't allowed to use pfree()!
		 *
		 * The memory area for SQL data values retrieved by any
		 * FETCH from the underlying cursor is allocated by palloc(),
		 * however. We don't free them immediately and leave this up
		 * to memory context cleanup.
		 */
		ifxDeallocateSQLDA(info);
		elog(DEBUG2, "informix_fdw: undo allocate");
		ifxPopCallstack(info, IFX_STACK_ALLOCATE);
	}

	if ((info->call_stack & IFX_STACK_DECLARE) == IFX_STACK_DECLARE)
	{
		ifxFreeResource(info, IFX_STACK_DECLARE);
		elog(DEBUG2, "informix_fdw: undo declare");
		ifxPopCallstack(info, IFX_STACK_DECLARE);
	}

	if ((info->call_stack & IFX_STACK_PREPARE) == IFX_STACK_PREPARE)
	{
		ifxFreeResource(info, IFX_STACK_PREPARE);
		elog(DEBUG2, "informix_fdw: undo prepare");
		ifxPopCallstack(info, IFX_STACK_PREPARE);
	}

	info->call_stack = IFX_STACK_EMPTY;
}

/*
 * Trap errors from the informix FDW API.
 *
 * This function checks exceptions from ESQL
 * and creates corresponding NOTICE, WARN or ERROR
 * messages.
 *
 */
static IfxSqlStateClass ifxCatchExceptions(IfxStatementInfo *state,
										   unsigned short stackentry)
{
	IfxSqlStateClass errclass;

	/*
	 * Set last error, if any
	 */
	errclass = ifxSetException(state);

	if (errclass != IFX_SUCCESS)
	{
		/*
		 * Obtain the error message. Since ifxRewindCallstack()
		 * will release any associated resources before we can
		 * print an ERROR message, we save the current from
		 * the caller within an IfxSqlStateMessage structure.
		 */
		IfxSqlStateMessage message;

		elog(DEBUG1, "informix FDW exception count: %d",
			 state->exception_count);

		ifxGetSqlStateMessage(1, &message);

		switch (errclass)
		{
			case IFX_RT_ERROR:
				/*
				 * log Informix runtime error.
				 *
				 * There's no ERRCODE_FDW_FATAL, so we go with a HV000 error
				 * code for now, but print out the error message as ERROR.
				 *
				 * A runtime error normally means a SQL error. Formerly, we did
				 * a FATAL here, but this stroke me as far to hard (it will exit
				 * the backend). Go with an ERROR instead...
				 */
				ifxRewindCallstack(state);
			case IFX_ERROR:
			case IFX_ERROR_INVALID_NAME:
				/* log ERROR */
				ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
								errmsg("informix FDW error: \"%s\"",
									   message.text),
								errdetail("SQLSTATE %s (SQLCODE=%d)",
										  message.sqlstate, message.sqlcode)));
				break;
			case IFX_WARNING:
				/* log WARN */
				ereport(WARNING, (errcode(ERRCODE_FDW_ERROR),
								  errmsg("informix FDW warning: \"%s\"",
										 message.text),
								  errdetail("SQLSTATE %s", message.sqlstate)));
				break;
			case IFX_ERROR_TABLE_NOT_FOUND:
				/* log missing FDW table */
				ereport(ERROR, (errcode(ERRCODE_FDW_TABLE_NOT_FOUND),
								errmsg("informix FDW missing table: \"%s\"",
									   message.text),
								errdetail("SQLSTATE %s", message.sqlstate)));
				break;
			case IFX_NOT_FOUND:
			default:
				/* needs no log */
				break;
		}
	}

	/*
	 * IFX_SUCCESS
	 */
	ifxPushCallstack(state, stackentry);

	return errclass;
}

/*
 * Retrieve the local column definition of the
 * foreign table (attribute number, type and additional
 * options).
 */
static void ifxPgColumnData(Oid foreignTableOid, IfxFdwExecutionState *festate)
{
	HeapTuple         tuple;
	Relation          attrRel;
	SysScanDesc       scan;
	ScanKeyData       key[2];
	Form_pg_attribute attrTuple;
	Relation          foreignRel;
	int               pgAttrIndex;
	int               ifxAttrIndex;

	pgAttrIndex  = 0;
	ifxAttrIndex = 0;
	festate->pgDroppedAttrCount = 0;

	/* open foreign table, should be locked already */
	foreignRel = heap_open(foreignTableOid, NoLock);
	festate->pgAttrCount = RelationGetNumberOfAttributes(foreignRel);
	heap_close(foreignRel, NoLock);

	festate->pgAttrDefs = palloc0fast(sizeof(PgAttrDef) * festate->pgAttrCount);

	/*
	 * Get all attributes for the given foreign table.
	 */
	attrRel = heap_open(AttributeRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(foreignTableOid));
	ScanKeyInit(&key[1], Anum_pg_attribute_attnum,
				BTGreaterStrategyNumber, F_INT2GT,
				Int16GetDatum((int16)0));
	scan = systable_beginscan(attrRel, AttributeRelidNumIndexId, true,
							  SnapshotNow, 2, key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		attrTuple = (Form_pg_attribute) GETSTRUCT(tuple);

		/*
		 * Current PostgreSQL attnum.
		 */
		++pgAttrIndex;

		/*
		 * Ignore dropped columns.
		 */
		if (attrTuple->attisdropped)
		{
			festate->pgAttrDefs[pgAttrIndex - 1].attnum = -1;

			/*
			 * In case of dropped columns, we differ from the attribute
			 * numbers used for Informix. Record them accordingly.
			 */
			festate->pgAttrDefs[pgAttrIndex - 1].ifx_attnum = -1;
			festate->pgAttrDefs[pgAttrIndex - 1].atttypid = -1;
			festate->pgAttrDefs[pgAttrIndex - 1].atttypmod = -1;
			festate->pgAttrDefs[pgAttrIndex - 1].attname = NULL;
			festate->pgDroppedAttrCount++;
			continue;
		}

		/*
		 * Don't rely on pgAttrIndex directly.
		 *
		 * RelationGetNumberOfAttributes() always counts the number
		 * of attributes *including* dropped columns.
		 *
		 * Increment ifxAttrIndex only in case we don't have
		 * a dropped column. Otherwise we won't match the
		 * Informix attribute list.
		 */
		++ifxAttrIndex;

		/*
		 * Protect against corrupted numbers in pg_class.relnatts
		 * and number of attributes retrieved from pg_attribute.
		 */
		if (pgAttrIndex > festate->pgAttrCount)
		{
			systable_endscan(scan);
			heap_close(attrRel, AccessShareLock);
			elog(ERROR, "unexpected number of attributes in foreign table");
		}

		/*
		 * Save the attribute and all required properties for
		 * later usage.
		 */
		festate->pgAttrDefs[pgAttrIndex - 1].attnum = attrTuple->attnum;
		festate->pgAttrDefs[pgAttrIndex - 1].ifx_attnum = ifxAttrIndex;
		festate->pgAttrDefs[pgAttrIndex - 1].atttypid = attrTuple->atttypid;
		festate->pgAttrDefs[pgAttrIndex - 1].atttypmod = attrTuple->atttypmod;
		festate->pgAttrDefs[pgAttrIndex - 1].attname = pstrdup(NameStr(attrTuple->attname));
		festate->pgAttrDefs[pgAttrIndex - 1].attnotnull = attrTuple->attnotnull;

		elog(DEBUG5, "mapped attnum PG/IFX %d => %d",
			 festate->pgAttrDefs[pgAttrIndex - 1].attnum,
			 PG_MAPPED_IFX_ATTNUM(festate, pgAttrIndex - 1));
	}

	/* finish */
	systable_endscan(scan);
	heap_close(attrRel, AccessShareLock);
}

/*
 * Checks for duplicate and redundant options.
 *
 * Check for redundant options. Error out in case we've found
 * any duplicates or, in case it is an empty option, assign
 * it to the connection info.
 */
static void
ifxGetOptionDups(IfxConnectionInfo *coninfo, DefElem *def)
{
	Assert(coninfo != NULL);

	if (strcmp(def->defname, "informixdir") == 0)
	{
		if (coninfo->informixdir)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: informixdir(%s)",
								   defGetString(def))));

		coninfo->informixdir = defGetString(def);
	}

	if (strcmp(def->defname, "gl_date") == 0)
	{
		if (coninfo->gl_date)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: gl_date(%s)",
								   defGetString(def))));

		coninfo->gl_date = defGetString(def);
	}

	if (strcmp(def->defname, "db_locale") == 0)
	{
		if (coninfo->db_locale)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: db_locale(%s)",
								   defGetString(def))));

		coninfo->db_locale = defGetString(def);
	}


	if (strcmp(def->defname, "gl_datetime") == 0)
	{
		if (coninfo->gl_datetime)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: gl_datetime(%s)",
								   defGetString(def))));

		coninfo->gl_datetime = defGetString(def);
	}

	if (strcmp(def->defname, "client_locale") == 0)
	{
		if (coninfo->client_locale)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: client_locale(%s)",
								   defGetString(def))));

		coninfo->client_locale = defGetString(def);
	}


	if (strcmp(def->defname, "servername") == 0)
	{
		if (coninfo->servername)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: servername(%s)",
								   defGetString(def))));

		coninfo->servername = defGetString(def);
	}

	if (strcmp(def->defname, "database") == 0)
	{
		if (coninfo->database)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: database(%s)",
								   defGetString(def))));

		coninfo->database = defGetString(def);
	}

	if (strcmp(def->defname, "username") == 0)
	{
		if (coninfo->database)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: username(%s)",
								   defGetString(def))));

		coninfo->username = defGetString(def);
	}

	if (strcmp(def->defname, "password") == 0)
	{
		if (coninfo->password)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: password(%s)",
								   defGetString(def))));

		coninfo->password = defGetString(def);
	}

	if (strcmp(def->defname, "query") == 0)
	{
		if (coninfo->tablename)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting options: query cannot be used with table")
						));

		if (coninfo->query)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting or redundant options: query (%s)", defGetString(def))
						));

		coninfo->tablename = defGetString(def);
	}

	if (strcmp(def->defname, "table") == 0)
	{
		if (coninfo->query)
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("conflicting options: query cannot be used with query")));

		if (coninfo->tablename)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: table(%s)",
								   defGetString(def))));

		coninfo->tablename = defGetString(def);
	}

}

/*
 * Returns the database connection string
 * as 'dbname@servername'
 */
static StringInfoData *
ifxGetDatabaseString(IfxConnectionInfo *coninfo)
{
	StringInfoData *buf;

	buf = makeStringInfo();
	initStringInfo(buf);
	appendStringInfo(buf, "%s@%s", coninfo->database, coninfo->servername);

	return buf;
}

/*
 * Create a unique name for the database connection.
 *
 * Currently the name is generated by concatenating the
 * database name, server name and user into a single string.
 */
static StringInfoData *
ifxGenerateConnName(IfxConnectionInfo *coninfo)
{
	StringInfoData *buf;

	buf = makeStringInfo();
	initStringInfo(buf);
	appendStringInfo(buf, "%s%s%s", coninfo->username, coninfo->database,
					 coninfo->servername);

	return buf;
}

Datum
ifx_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwRoutine = makeNode(FdwRoutine);

	fdwRoutine->ExplainForeignScan = ifxExplainForeignScan;
	fdwRoutine->BeginForeignScan   = ifxBeginForeignScan;
	fdwRoutine->IterateForeignScan = ifxIterateForeignScan;
	fdwRoutine->EndForeignScan     = ifxEndForeignScan;
	fdwRoutine->ReScanForeignScan  = ifxReScanForeignScan;

	#if PG_VERSION_NUM < 90200

	fdwRoutine->PlanForeignScan    = ifxPlanForeignScan;

	#else

	fdwRoutine->GetForeignRelSize = ifxGetForeignRelSize;
	fdwRoutine->GetForeignPaths   = ifxGetForeignPaths;
	fdwRoutine->GetForeignPlan    = ifxGetForeignPlan;
	fdwRoutine->AnalyzeForeignTable = ifxAnalyzeForeignTable;

	#endif

	/*
	 * Since PostgreSQL 9.3 we support updatable foreign tables.
	 */
	#if PG_VERSION_NUM >= 90300

	fdwRoutine->AddForeignUpdateTargets = ifxAddForeignUpdateTargets;
	fdwRoutine->PlanForeignModify       = ifxPlanForeignModify;
	fdwRoutine->BeginForeignModify      = ifxBeginForeignModify;
	fdwRoutine->ExecForeignInsert       = ifxExecForeignInsert;
	fdwRoutine->ExecForeignDelete       = ifxExecForeignDelete;
	fdwRoutine->ExecForeignUpdate       = ifxExecForeignUpdate;
	fdwRoutine->EndForeignModify        = ifxEndForeignModify;

	#endif

	PG_RETURN_POINTER(fdwRoutine);
}


/*
 * ifxReScanForeignScan
 *
 *   Restart the scan with new parameters.
 */
static void ifxReScanForeignScan(ForeignScanState *state)
{
	IfxFdwExecutionState *fdw_state;
	fdw_state = (IfxFdwExecutionState *) state->fdw_state;

	elog(DEBUG1, "informix_fdw: rescan");

	/*
	 * We're in a rescan condition on our foreign table.
	 */
	fdw_state->rescan = true;
}

/*
 * Validate options passed to the INFORMIX FDW (that are,
 * FOREIGN DATA WRAPPER, SERVER, USER MAPPING and FOREIGN TABLE)
 */
Datum
ifx_fdw_validator(PG_FUNCTION_ARGS)
{
	List     *ifx_options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid       catalogOid = PG_GETARG_OID(1);
	IfxConnectionInfo coninfo = {0};
	ListCell *cell;

	/*
	 * Check options passed to this FDW. Validate values and required
	 * arguments.
	 */
	foreach(cell, ifx_options_list)
	{
		DefElem *def = (DefElem *) lfirst(cell);

		/*
		 * Unknown option specified, print an error message
		 * and a hint message what's wrong.
		 */
		if (!ifxIsValidOption(def->defname, catalogOid))
		{
			StringInfoData *buf;

			buf = ifxFdwOptionsToStringBuf(catalogOid);

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("Valid options in this context are: %s", buf->len ? buf->data : "<none>")
						));
		}

		/*
		 * Duplicates present in current options list?
		 */
		ifxGetOptionDups(&coninfo, def);
	}

	PG_RETURN_VOID();
}

/*
 * Retrieves options for ifx_fdw foreign data wrapper.
 */
 static void
ifxGetOptions(Oid foreigntableOid, IfxConnectionInfo *coninfo)
{
	ForeignTable  *foreignTable;
	ForeignServer *foreignServer;
	UserMapping   *userMap;
	List          *options;
	ListCell      *elem;
	bool           mandatory[IFX_REQUIRED_CONN_KEYWORDS] = { false, false, false, false };
	int            i;

	Assert(coninfo != NULL);

	foreignTable  = GetForeignTable(foreigntableOid);
	foreignServer = GetForeignServer(foreignTable->serverid);
	userMap       = GetUserMapping(GetUserId(), foreignTable->serverid);

	options = NIL;
	options = list_concat(options, foreignTable->options);
	options = list_concat(options, foreignServer->options);
	options = list_concat(options, userMap->options);

	/*
	 * Retrieve required arguments.
	 */
	foreach(elem, options)
	{
		DefElem *def = (DefElem *) lfirst(elem);

		elog(DEBUG5, "ifx_fdw set param %s=%s",
			 def->defname, defGetString(def));

		/*
		 * "informixserver" defines the INFORMIXSERVER to connect to
		 */
		if (strcmp(def->defname, "informixserver") == 0)
		{
			coninfo->servername = pstrdup(defGetString(def));
			mandatory[0] = true;
		}

		/*
		 * "informixdir" defines the INFORMIXDIR environment
		 * variable.
		 */
		if (strcmp(def->defname, "informixdir") == 0)
		{
			coninfo->informixdir = pstrdup(defGetString(def));
			mandatory[1] = true;
		}

		if (strcmp(def->defname, "database") == 0)
		{
			coninfo->database = pstrdup(defGetString(def));
			mandatory[3] = true;
		}

		if (strcmp(def->defname, "username") == 0)
		{
			coninfo->username = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "password") == 0)
		{
			coninfo->password = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "table") == 0)
		{
			coninfo->tablename = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "query") == 0)
		{
			coninfo->query = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "gl_date") == 0)
		{
			coninfo->gl_date = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "gl_datetime") == 0)
		{
			coninfo->gl_datetime = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "client_locale") == 0)
		{
			coninfo->client_locale = pstrdup(defGetString(def));
			mandatory[2] = true;
		}

		if (strcmp(def->defname, "db_locale") == 0)
		{
			coninfo->db_locale = pstrdup(defGetString(def));
			mandatory[2] = true;
		}

		if (strcmp(def->defname, "disable_predicate_pushdown") == 0)
		{
			/* we don't bother about the value passed to
			 * this argument, treat its existence to disable
			 * predicate pushdown.
			 */
			coninfo->predicate_pushdown = 0;
		}

		if (strcmp(def->defname, "enable_blobs") == 0)
		{
			/* we don't bother about the value passed
			 * to enable_blobs atm.
			 */
			coninfo->enable_blobs = 1;
		}
	}

	if ((coninfo->query == NULL)
		 && (coninfo->tablename == NULL))
	{
		ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
						errmsg("invalid options for remote table \"%s\"",
							   get_rel_name(foreignTable->relid)),
						errdetail("either parameter \"query\" or \"table\" is missing")));
	}

	/*
	 * Check for all other mandatory options
	 */
	for (i = 0; i < IFX_REQUIRED_CONN_KEYWORDS; i++)
	{
		if (!mandatory[i])
			ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
							errmsg("missing required FDW options (informixserver, informixdir, client_locale, database)")));
	}

}

/*
 * Generate a unique statement identifier to create
 * on the target database. Informix requires us to build
 * a unique name among all concurrent connections.
 *
 * Returns a palloc'ed string containing a statement identifier
 * suitable to pass to an Informix database.
 */
static char *ifxGenStatementName(IfxConnectionInfo *coninfo,
								 int stmt_id)
{
	char *stmt_name;
	size_t stmt_name_len;

	stmt_name_len = strlen(coninfo->conname) + 26;
	stmt_name     = (char *) palloc(stmt_name_len + 1);
	bzero(stmt_name, stmt_name_len + 1);

	snprintf(stmt_name, stmt_name_len, "%s_stmt%d_%d",
			 coninfo->conname, MyBackendId, stmt_id);

	return stmt_name;
}

static char *ifxGenDescrName(IfxConnectionInfo *coninfo,
							 int descr_id)
{
	char *descr_name;
	size_t descr_name_len;

	descr_name_len = strlen(coninfo->conname) + 27;
	descr_name     = (char *)palloc(descr_name_len + 1);
	bzero(descr_name, descr_name_len + 1);

	snprintf(descr_name, descr_name_len, "%s_descr%d_%d",
			 coninfo->conname, MyBackendId, descr_id);

	return descr_name;
}


/*
 * Generate a unique cursor identifier
 *
 * The specified curid should be a unique number
 * identifying the returned cursor name uniquely throughout
 * the backend.
 */
static char *ifxGenCursorName(IfxConnectionInfo *coninfo, int curid)
{
	char *cursor_name;
	size_t len;

	len = strlen(coninfo->conname) + 26;
	cursor_name = (char *) palloc(len + 1);
	bzero(cursor_name, len + 1);

	snprintf(cursor_name, len, "%s_cur%d_%d",
			 coninfo->conname, MyBackendId, curid);
	return cursor_name;
}

/*
 * Prepare informix query object identifier
 */
static void ifxPrepareParamsForScan(IfxFdwExecutionState *state,
									IfxConnectionInfo *coninfo)
{
	StringInfoData *buf;

	buf = makeStringInfo();
	initStringInfo(buf);

	/*
	 * Record the given query and pass it over
	 * to the state structure.
	 */
	if (coninfo->query)
	{
		if ((state->stmt_info.predicate != NULL)
			&& (strlen(state->stmt_info.predicate) > 0)
			&& coninfo->predicate_pushdown)
		{
			appendStringInfo(buf, "%s WHERE %s",
							 coninfo->query,
							 state->stmt_info.predicate);
		}
		else
		{
			appendStringInfo(buf, "%s",
							 coninfo->query);
		}
	}
	else
	{
		/*
		 * NOTE:
		 *
		 * Don't declare the query as READ ONLY. We can't really
		 * distinguish wether the scan is related to a DELETE or UPDATE.
		 *
		 * XXX:
		 *
		 * We declare the Informix transaction with REPEATABLE READ
		 * isolation level. Consider different modes here, e.g. FOR UPDATE
		 * with READ COMMITTED...
		 */
		if ((state->stmt_info.predicate != NULL)
			&& (strlen(state->stmt_info.predicate) > 0)
			&& coninfo->predicate_pushdown)
		{
			appendStringInfo(buf, "SELECT * FROM %s WHERE %s",
							 coninfo->tablename,
							 state->stmt_info.predicate);
		}
		else
		{
			appendStringInfo(buf, "SELECT * FROM %s",
							 coninfo->tablename);
		}
	}

	/*
	 * In case we got a foreign scan initiated by
	 * an UPDATE/DELETE DML command, we need to do a
	 * FOR UPDATE, otherwise the cursor won't be updatable
	 * later in the modify actions.
	 */
	if ((state->stmt_info.cursorUsage == IFX_UPDATE_CURSOR)
		|| (state->stmt_info.cursorUsage == IFX_SCROLL_UPDATE_CURSOR))
	{
		appendStringInfoString(buf, " FOR UPDATE");
	}

	state->stmt_info.query = buf->data;

	/*
	 * Save the connection identifier.
	 */
	StrNCpy(state->stmt_info.conname, coninfo->conname, IFX_CONNAME_LEN);
}

/*
 * ifxBeginForeignScan
 *
 * Implements FDW BeginForeignScan callback function.
 */
static void
ifxBeginForeignScan(ForeignScanState *node, int eflags)
{
	IfxConnectionInfo    *coninfo;
	IfxFdwExecutionState *festate;
	IfxCachedConnection  *cached;
	Oid                   foreignTableOid;
	bool                  conn_cached;
	List                 *plan_values;

	elog(DEBUG3, "informix_fdw: begin scan");

	plan_values = PG_SCANSTATE_PRIVATE_P(node);
	foreignTableOid = RelationGetRelid(node->ss.ss_currentRelation);
	Assert((foreignTableOid != InvalidOid));
	coninfo = ifxMakeConnectionInfo(foreignTableOid);

	/*
	 * Tell the connection cache that we are about to start to scan
	 * the remote table.
	 */
	coninfo->scan_mode = IFX_BEGIN_SCAN;

	/*
	 * We should have a cached connection entry for the requested table.
	 */
	cached = ifxConnCache_add(foreignTableOid, coninfo,
							  &conn_cached);

	/* should not happen here */
	Assert(conn_cached && cached != NULL);

	/* Initialize generic executation state structure */
	festate = makeIfxFdwExecutionState(-1);

	/*
	 * Make the connection current (otherwise we might
	 * get confused).
	 */
	if (conn_cached)
	{
		ifxSetConnection(coninfo);
	}

	/*
	 * Check connection status.
	 */
	if ((ifxConnectionStatus() != IFX_CONNECTION_OK)
		&& (ifxConnectionStatus() != IFX_CONNECTION_WARN))
	{
		elog(ERROR, "could not set requested informix connection");
	}

	/*
	 * Record our FDW state structures.
	 */
	node->fdw_state = (void *) festate;

	/*
	 * Cached plan data present?
	 */
	if (PG_SCANSTATE_PRIVATE_P(node) != NULL)
	{
		/*
		 * Retrieved cached parameters formerly prepared
		 * by ifxPlanForeignScan().
		 */
		ifxDeserializeFdwData(festate, plan_values);
	}
	else
	{
		elog(DEBUG1, "informix_fdw no cached plan data");
		ifxPrepareParamsForScan(festate, coninfo);
	}

	/*
	 * Recheck if everything is already prepared on the
	 * informix server. If not, we are either in a rescan condition
	 * or a cached query plan is used. Redo all necessary preparation
	 * previously done in the planning state. We do this to save
	 * some cycles when just doing plain SELECTs.
	 */
	if (festate->stmt_info.call_stack == IFX_STACK_EMPTY)
		ifxPrepareCursorForScan(&festate->stmt_info, coninfo);

	/*
	 * Get the definition of the local foreign table attributes.
	 */
	ifxPgColumnData(foreignTableOid, festate);

	/* EXPLAIN without ANALYZE... */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
	{
		elog(DEBUG1, "informix_fdw: explain only");
		return;
	}

	/*
	 * Populate the DESCRIPTOR area.
	 */
	elog(DEBUG1, "populate descriptor area for statement \"%s\"",
		 festate->stmt_info.stmt_name);
	ifxDescribeAllocatorByName(&festate->stmt_info);
	ifxCatchExceptions(&festate->stmt_info, IFX_STACK_ALLOCATE | IFX_STACK_DESCRIBE);

	/*
	 * Get the number of columns.
	 */
	festate->stmt_info.ifxAttrCount = ifxDescriptorColumnCount(&festate->stmt_info);
	elog(DEBUG1, "get descriptor column count %d",
		 festate->stmt_info.ifxAttrCount);
	ifxCatchExceptions(&festate->stmt_info, 0);

	/*
	 * XXX: It makes no sense to have a local column list with *more*
	 * columns than the remote table. I can't think of any use case
	 * for this atm, anyone?
	 */
	if (PG_VALID_COLS_COUNT(festate) > festate->stmt_info.ifxAttrCount)
	{
		ifxRewindCallstack(&(festate->stmt_info));
		ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
						errmsg("foreign table \"%s\" has more columns than remote source",
							   get_rel_name(foreignTableOid))));
	}

	festate->stmt_info.ifxAttrDefs = palloc(festate->stmt_info.ifxAttrCount
											* sizeof(IfxAttrDef));

	/*
	 * Populate result set column info array.
	 */
	if ((festate->stmt_info.row_size = ifxGetColumnAttributes(&festate->stmt_info)) == 0)
	{
		/* oops, no memory to allocate? Something surely went wrong,
		 * so abort */
		ifxRewindCallstack(&festate->stmt_info);
		ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
						errmsg("could not initialize informix column properties")));
	}

	/*
	 * Throw an error in case we select from a relation with
	 * BLOB types and enable_blobs FDW option is unset. We must not
	 * use a SCROLL cursor in this case. Switching the cursor options
	 * at this point is too late, since we already DESCRIBEd and PREPAREd
	 * the cursor. Alternatively, we could re-PREPARE the cursor as a
	 * NO SCROLL cursor again, but this strikes me as too dangerous (consider
	 * changing table definitions in the meantime).
	 *
	 * NOTE: A non-scrollable cursor requires a serialized transaction to
	 *       be safe. However, we don't enforce this isolation atm, since
	 *       Informix databases with no logging would not be queryable at all.
	 *       But someone have to keep in mind, that a ReScan of the foreign
	 *       table could lead to inconsistent data due to changed results
	 *       sets.
	 */
	if ((festate->stmt_info.special_cols & IFX_HAS_BLOBS)
		&& (festate->stmt_info.cursorUsage == IFX_SCROLL_CURSOR))
	{
		ifxRewindCallstack(&festate->stmt_info);
		ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
						errmsg("could not use a SCROLL cursor to query an "
							   "informix table with blobs"),
						errhint("set enable_blobs=1 to your foreign table "
								"to use a NO SCROLL cursor")));
	}

	/*
	 * NOTE:
	 *
	 * ifxGetColumnAttributes() obtained all information about the
	 * returned column and stored them within the informix SQLDA and
	 * sqlvar structs. However, we don't want to allocate memory underneath
	 * our current memory context, thus we allocate the required memory structure
	 * on top here. ifxSetupDataBufferAligned() will assign the allocated
	 * memory area to the SQLDA structure and will maintain the data offsets
	 * properly aligned.
	 */
	festate->stmt_info.data = (char *) palloc0(festate->stmt_info.row_size);
	festate->stmt_info.indicator = (short *) palloc0(sizeof(short)
													 * festate->stmt_info.ifxAttrCount);

	/*
	 * Assign sqlvar pointers to the allocated memory area.
	 */
	ifxSetupDataBufferAligned(&festate->stmt_info);

	/*
	 * Open the cursor.
	 */
	elog(DEBUG1, "open cursor \"%s\"",
		 festate->stmt_info.cursor_name);
	ifxOpenCursorForPrepared(&festate->stmt_info);
	ifxCatchExceptions(&festate->stmt_info, IFX_STACK_OPEN);

}

/*
 * Extract the corresponding Informix value for the given PostgreSQL attnum
 * from the SQLDA structure. The specified attnum should be the target column
 * of the local table definition and is translated internally to the matching
 * source column on the remote table.
 */
static void ifxColumnValueByAttNum(IfxFdwExecutionState *state, int attnum,
								   bool *isnull)
{
	Assert(state != NULL && attnum >= 0);
	Assert(state->stmt_info.data != NULL);
	Assert(state->values != NULL);
	Assert(state->pgAttrDefs);

	/*
	 * Setup...
	 */
	state->values[PG_MAPPED_IFX_ATTNUM(state, attnum)].def
		= &state->stmt_info.ifxAttrDefs[PG_MAPPED_IFX_ATTNUM(state, attnum)];
	IFX_SETVAL_P(state, attnum, PointerGetDatum(NULL));
	*isnull = false;

	/*
	 * Retrieve values from Informix and try to convert
	 * into an appropiate PostgreSQL datum.
	 */

	switch (IFX_ATTRTYPE_P(state, attnum))
	{
		case IFX_SMALLINT:
			/*
			 * All int values are handled
			 * by convertIfxInt()...so fall through.
			 */
		case IFX_INTEGER:
		case IFX_SERIAL:
		case IFX_INT8:
		case IFX_SERIAL8:
		case IFX_INFX_INT8:
		{
			Datum dat;

			dat = convertIfxInt(state, attnum);
			*isnull = (IFX_ATTR_ISNULL_P(state, attnum));

			/*
			 * Check for errors, but only if we
			 * didnt get a validated NULL attribute from
			 * informix.
			 */
			if (! IFX_ATTR_IS_VALID_P(state, attnum))
			{
				ifxRewindCallstack(&state->stmt_info);
				elog(ERROR, "could not convert informix type id %d into pg type %u",
					 IFX_ATTRTYPE_P(state, attnum),
					 PG_ATTRTYPE_P(state, attnum));
			}

			IFX_SETVAL_P(state, attnum, dat);
			break;
		}
		case IFX_CHARACTER:
		case IFX_VCHAR:
		case IFX_NCHAR:
		case IFX_LVARCHAR:
		case IFX_NVCHAR:
		{
			/* SQLCHAR, SQLVCHAR, SQLNCHAR, SQLLVARCHAR, SQLNVCHAR */
			Datum dat;

			dat = convertIfxCharacterString(state, attnum);
			*isnull = (IFX_ATTR_ISNULL_P(state, attnum));

			/*
			 * At this point we never expect a NULL datum without
			 * having retrieved NULL from informix. Check it.
			 * If it's a validated NULL value from informix,
			 * don't throw an error.
			 */
			if ((DatumGetPointer(dat) == NULL)
				&& !*isnull)
			{
				ifxRewindCallstack(&state->stmt_info);
				elog(ERROR, "could not convert informix type id %d into pg type %u",
					 IFX_ATTRTYPE_P(state, attnum),
					 PG_ATTRTYPE_P(state, attnum));
			}

			IFX_SETVAL_P(state, attnum, dat);
			break;
		}
		case IFX_BYTES:
		case IFX_TEXT:
		{
			Datum dat;

			dat = convertIfxSimpleLO(state, attnum);

			/*
			 * Check for invalid datum conversion.
			 */
			if (! IFX_ATTR_IS_VALID_P(state, attnum))
			{
				elog(ERROR, "could not convert informix type id %d into pg type %u",
					 IFX_ATTRTYPE_P(state, attnum),
					 PG_ATTRTYPE_P(state, attnum));
			}

			/*
			 * Valid NULL datum?
			 */
			*isnull = (IFX_ATTR_ISNULL_P(state, attnum));
			IFX_SETVAL_P(state, attnum, dat);

			break;
		}
		case IFX_BOOLEAN:
		{
			/* SQLBOOL value */
			Datum dat;
			dat = convertIfxBoolean(state, attnum);

			/*
			 * Unlike other types, a NULL datum is treated
			 * like a normal FALSE value in case the indicator
			 * value tells that we got a NOT NULL column.
			 */
			if (! IFX_ATTR_IS_VALID_P(state, attnum))
			{
				ifxRewindCallstack(&state->stmt_info);
				elog(ERROR, "could not convert informix type id %d into pg type %u",
					 IFX_ATTRTYPE_P(state, attnum),
					 PG_ATTRTYPE_P(state, attnum));
			}

			*isnull = (IFX_ATTR_ISNULL_P(state, attnum));
			IFX_SETVAL_P(state, attnum, dat);

			break;
		}
		case IFX_DATE:
		{
			/* SQLDATE value */
			Datum dat;
			dat = convertIfxDateString(state, attnum);

			/*
			 * Valid datum?
			 */
			if ((DatumGetPointer(dat) == NULL)
				&& ! IFX_ATTR_IS_VALID_P(state, attnum))
			{
				ifxRewindCallstack(&state->stmt_info);
				elog(ERROR, "could not convert informix type id %d into pg type %u",
					 IFX_ATTRTYPE_P(state, attnum),
					 PG_ATTRTYPE_P(state, attnum));
			}

			*isnull = (IFX_ATTR_ISNULL_P(state, attnum));
			IFX_SETVAL_P(state, attnum, dat);
			break;
		}
		case IFX_DTIME:
		{
			/* SQLDTIME value */
			Datum dat;
			dat = convertIfxTimestampString(state, attnum);

			/*
			 * Valid datum?
			 */
			if ((DatumGetPointer(dat) == NULL)
				&& ! IFX_ATTR_IS_VALID_P(state, attnum))
			{
				ifxRewindCallstack(&state->stmt_info);
				elog(ERROR, "could not convert informix type id %d into pg type %u",
					 IFX_ATTRTYPE_P(state, attnum),
					 PG_ATTRTYPE_P(state, attnum));
			}

			*isnull = (IFX_ATTR_ISNULL_P(state, attnum));
			IFX_SETVAL_P(state, attnum, dat);
			break;
		}
		case IFX_DECIMAL:
		{
			/* DECIMAL value */
			Datum dat;
			dat = convertIfxDecimal(state, attnum);

			/*
			 * Valid datum?
			 */
			if ((DatumGetPointer(dat) == NULL)
				&& ! IFX_ATTR_IS_VALID_P(state, attnum))
			{
				ifxRewindCallstack(&state->stmt_info);
				elog(ERROR, "could not convert informix decimal into pg type %u",
					 PG_ATTRTYPE_P(state, attnum));
			}

			*isnull = (IFX_ATTR_ISNULL_P(state, attnum));
			IFX_SETVAL_P(state, attnum, dat);
			break;
		}
		default:
		{
			ifxRewindCallstack(&state->stmt_info);
			elog(ERROR, "\"%d\" is not a known informix type id",
				state->stmt_info.ifxAttrDefs[attnum].type);
			break;
		}
	}
}

static void ifxEndForeignScan(ForeignScanState *node)
{
	IfxFdwExecutionState *state;
	List                 *plan_values;

	elog(DEBUG3, "informix_fdw: end scan");

	state = (IfxFdwExecutionState *) node->fdw_state;
	plan_values = PG_SCANSTATE_PRIVATE_P(node);
	ifxDeserializeFdwData(state, plan_values);

	/*
	 * Dispose SQLDA resource, allocated database objects, ...
	 */
	ifxRewindCallstack(&state->stmt_info);

	/*
	 * Save the callstack into cached plan structure. This
	 * is necessary to teach ifxBeginForeignScan() to do the
	 * right thing(tm)...
	 */
	ifxSetSerializedInt16Field(plan_values,
							   SERIALIZED_CALLSTACK,
							   state->stmt_info.call_stack);
}

static TupleTableSlot *ifxIterateForeignScan(ForeignScanState *node)
{
	TupleTableSlot       *tupleSlot = node->ss.ss_ScanTupleSlot;
	IfxConnectionInfo    *coninfo;
	IfxFdwExecutionState *state;
	IfxSqlStateClass      errclass;
	Oid                   foreignTableOid;
	bool                  conn_cached;

	state = (IfxFdwExecutionState *) node->fdw_state;

	elog(DEBUG3, "informix_fdw: iterate scan");

	/*
	 * Make the informix connection belonging to this
	 * iteration current.
	 */
	foreignTableOid = RelationGetRelid(node->ss.ss_currentRelation);
	coninfo= ifxMakeConnectionInfo(foreignTableOid);

	/*
	 * Set appropiate scan mode.
	 */
	coninfo->scan_mode = IFX_ITERATE_SCAN;

	/*
	 * ...and get the handle.
	 */
	ifxConnCache_add(foreignTableOid, coninfo, &conn_cached);

	/*
	 * Make the connection current (otherwise we might
	 * get confused).
	 */
	if (conn_cached)
	{
		ifxSetConnection(coninfo);
	}

	/*
	 * Check connection status.
	 */
	if ((ifxConnectionStatus() != IFX_CONNECTION_OK)
		&& (ifxConnectionStatus() != IFX_CONNECTION_WARN))
	{
		elog(ERROR, "could not set requested informix connection");
	}

	tupleSlot->tts_mintuple      = NULL;
	tupleSlot->tts_buffer        = InvalidBuffer;
	tupleSlot->tts_tuple         = NULL;
	tupleSlot->tts_shouldFree    = false;
	tupleSlot->tts_shouldFreeMin = false;

	/*
	 * Catch any informix exception. We also need to
	 * check for IFX_NOT_FOUND, in which case no more rows
	 * must be processed.
	 */
	errclass = ifxFetchTuple(state);

	if (errclass != IFX_SUCCESS)
	{

		if (errclass == IFX_NOT_FOUND)
		{
			/*
			 * Create an empty tuple slot and we're done.
			 */
			elog(DEBUG2, "informix fdw scan end");

			tupleSlot->tts_isempty = true;
			tupleSlot->tts_nvalid  = 0;
			/* XXX: not required here ifxRewindCallstack(&(state->stmt_info)); */
			return tupleSlot;
		}

		/*
		 * All other error/warning cases should be catched.
		 */
		ifxCatchExceptions(&(state->stmt_info), 0);
	}

	ifxSetupTupleTableSlot(state, tupleSlot);

	/*
	 * The cursor should now be positioned at the current row
	 * we want to retrieve. Loop through the columns and retrieve
	 * their values. Note: No conversion into a PostgreSQL specific
	 * datatype is done yet.
	 */
	ifxGetValuesFromTuple(state, tupleSlot);

	return tupleSlot;
}

/*
 * Returns a new allocated pointer
 * to IfxConnectionInfo.
 */
static IfxConnectionInfo *ifxMakeConnectionInfo(Oid foreignTableOid)
{
	IfxConnectionInfo *coninfo;
	StringInfoData    *buf;
	StringInfoData    *dsn;

	/*
	 * Initialize connection handle, set
	 * defaults.
	 */
	coninfo = (IfxConnectionInfo *) palloc(sizeof(IfxConnectionInfo));
	bzero(coninfo->conname, IFX_CONNAME_LEN + 1);
	ifxConnInfoSetDefaults(coninfo);
	ifxGetOptions(foreignTableOid, coninfo);

	buf = ifxGenerateConnName(coninfo);
	StrNCpy(coninfo->conname, buf->data, IFX_CONNAME_LEN);

	dsn = ifxGetDatabaseString(coninfo);
	coninfo->dsn = pstrdup(dsn->data);

	return coninfo;
}

/*
 * ifxFilterQuals
 *
 * Walk through all FDW-related predicate expressions passed
 * by baserel->restrictinfo and examine them for pushdown.
 *
 * Any predicates able to be pushed down are converted into a
 * character string, suitable to be passed directly as SQL to
 * an informix server. An empty string is returned in case
 * no predicates are found.
 *
 * NOTE: excl_restrictInfo is a List, holding all rejected RestrictInfo
 * structs found not able to be pushed down. This is currently only
 * used in PostgreSQL version starting with 9.2, 9.1 version always
 * returns an empty list!
 */
static char * ifxFilterQuals(PlannerInfo *planInfo,
							 RelOptInfo *baserel,
							 List **excl_restrictInfo,
							 Oid foreignTableOid)
{
	IfxPushdownOprContext pushdownCxt;
	ListCell             *cell;
	StringInfoData       *buf;
	char                 *oprStr;
	int i;

	Assert(foreignTableOid != InvalidOid);

	pushdownCxt.foreign_relid = foreignTableOid;
	pushdownCxt.foreign_rtid  = baserel->relid;
	pushdownCxt.predicates    = NIL;
	pushdownCxt.count         = 0;

	/* Be paranoid, excluded RestrictInfo list initialized to be empty */
	*excl_restrictInfo = NIL;

	buf = makeStringInfo();
	initStringInfo(buf);

	/*
	 * Loop through the operator nodes and try to
	 * extract the pushdown expressions as a text datum
	 * to the pushdown context structure.
	 */
	foreach(cell, baserel->baserestrictinfo)
	{
		RestrictInfo *info;
		int found;

		info = (RestrictInfo *) lfirst(cell);

		found = pushdownCxt.count;
		ifx_predicate_tree_walker((Node *)info->clause, &pushdownCxt);

		if (found == pushdownCxt.count)
		{
			elog(DEBUG2, "RestrictInfo doesn't hold anything interesting, skipping");
			*excl_restrictInfo = lappend(*excl_restrictInfo, info);
		}

		/*
		 * Each list element from baserestrictinfo is AND'ed together.
		 * Record a corresponding IfxPushdownOprInfo structure in
		 * the context, so that it get decoded properly below.
		 */
		if (lnext(cell) != NULL)
		{
			IfxPushdownOprInfo *pushAndInfo;

			pushAndInfo              = palloc(sizeof(IfxPushdownOprInfo));
			pushAndInfo->type        = IFX_OPR_AND;
			pushAndInfo->expr_string = cstring_to_text("AND");

			pushdownCxt.predicates = lappend(pushdownCxt.predicates, pushAndInfo);
			pushdownCxt.count++;
		}
	}

	/*
	 * Since restriction clauses are always AND together,
	 * assume a AND_EXPR per default.
	 */
	oprStr = "AND";

	/*
	 * Filter step done, if any predicates to be able to be
	 * pushed down are found, we have a list of IfxPushDownOprInfo
	 * structure in the IfxPushdownOprContext structure. Loop
	 * through them and attach all supported filter quals into
	 * our result buffer.
	 */
	for (i = 0; i < pushdownCxt.count; i++)
	{
		IfxPushdownOprInfo *info;

		info = (IfxPushdownOprInfo *) list_nth(pushdownCxt.predicates, i);

		/* ignore filtered expressions */
		if (info->type == IFX_OPR_NOT_SUPPORTED)
		{
			continue;
		}

		switch (info->type)
		{
			case IFX_OPR_OR:
			case IFX_OPR_AND:
			case IFX_OPR_NOT:
				/* save current boolean opr context */
				oprStr = text_to_cstring(info->expr_string);
				break;
			case IFX_IS_NULL:
			case IFX_IS_NOT_NULL:
				/* fall through, no special action necessary */
			default:
				appendStringInfo(buf, " %s %s",
								 (i > 1) ? oprStr : "",
								 text_to_cstring(info->expr_string));
		}
	}

	/* empty string in case no pushdown predicates are found */
	return buf->data;
}

/*
 * ifxPrepareCursorForScan()
 *
 * Prepares the remote informix FDW to scan the relation.
 * This basically means to allocate the SQLDA description area and
 * declaring the cursor. The reason why this is a separate function is,
 * that we are eventually required to do it twice,
 * once in ifxPlanForeignScan() and in ifxBeginForeignScan().
 * When doing a scan, we  need the query plan from
 * the DECLARE CURSOR statement in ifxPlanForeignScan()
 * to get the query costs from the informix server easily. However, that
 * involves declaring the cursor in ifxPlanForeignScan(), which will be then
 * reused in ifxBeginForeignScan() later. To save extra cycles and declaring
 * the cursor twice, we just reuse the cursor previously declared in
 * ifxBeginForeignScan() later. However, if used for example with a prepared
 * statement, ifxPlanForeignScan() won't be called again, instead the
 * previously plan prepared by ifxPlanForeignScan() will be re-used. Since
 * ifxEndForeignScan() already has deallocated the complete structure, we
 * are required to redeclare the cursor again, to satisfy subsequent
 * EXECUTE calls to the prepared statement. This is relatively easy
 * to check, since the only thing we need to do in ifxBeginForeignScan()
 * is to recheck wether the call stack is empty or not.
 */
static void ifxPrepareCursorForScan(IfxStatementInfo *info,
									IfxConnectionInfo *coninfo)
{
	/*
	 * Generate a statement identifier. Required to uniquely
	 * identify the prepared statement within Informix.
	 */
	info->stmt_name = ifxGenStatementName(coninfo,
										  info->refid);

	/*
	 * An identifier for the dynamically allocated
	 * DESCRIPTOR area.
	 */
	info->descr_name = ifxGenDescrName(coninfo,
									   info->refid);

	/*
	 * ...and finally the cursor name.
	 */
	info->cursor_name = ifxGenCursorName(coninfo, info->refid);

	/* Prepare the query. */
	elog(DEBUG1, "prepare query \"%s\"", info->query);
	ifxPrepareQuery(info->query,
					info->stmt_name);
	ifxCatchExceptions(info, IFX_STACK_PREPARE);

	/*
	 * Declare the cursor for the prepared
	 * statement. Check out, if we need to switch the cursor
	 * type depending on special datatypes first.
	 */
	if (coninfo->enable_blobs)
	{
		elog(NOTICE, "informix_fdw: enable_blobs specified, forcing NO SCROLL cursor");

		if (!coninfo->tx_enabled)
			ereport(WARNING,
					(errcode(ERRCODE_FDW_INCONSISTENT_DESCRIPTOR_INFORMATION),
					 errmsg("informix_fdw: using NO SCROLL cursor without transactions")));

		info->cursorUsage = IFX_DEFAULT_CURSOR;
	}

	elog(DEBUG1, "declare cursor \"%s\"", info->cursor_name);
	ifxDeclareCursorForPrepared(info->stmt_name,
								info->cursor_name,
								info->cursorUsage);
	ifxCatchExceptions(info, IFX_STACK_DECLARE);
}

/*
 * ifxExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
ifxExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	IfxFdwExecutionState *festate;
	List                 *plan_values;
	IfxPlanData           planData;

	festate = (IfxFdwExecutionState *) node->fdw_state;

	/*
	 * XXX: We need to get the info from the cached connection!
	 */
	plan_values = PG_SCANSTATE_PRIVATE_P(node);
	ifxDeserializeFdwData(festate, plan_values);
	ifxDeserializePlanData(&planData, plan_values);

	/* Give some possibly useful info about startup costs */
	if (es->costs)
	{
		ExplainPropertyFloat("Informix costs", planData.costs, 2, es);
		ExplainPropertyText("Informix query", festate->stmt_info.query, es);
	}
}


static void ifxConnInfoSetDefaults(IfxConnectionInfo *coninfo)
{
	Assert(coninfo != NULL);

	if (coninfo == NULL)
		return;

	/* Assume non-tx enabled database, determined later */
	coninfo->tx_enabled = 0;

	/* Assume non-ANSI database */
	coninfo->db_ansi = 0;

    /* enable predicate pushdown */
	coninfo->predicate_pushdown = 1;

	/* disable enable_blobs per default */
	coninfo->enable_blobs = 0;

	coninfo->gl_date       = IFX_ISO_DATE;
	coninfo->gl_datetime   = IFX_ISO_TIMESTAMP;
	coninfo->db_locale     = NULL;
	coninfo->client_locale = NULL;
	coninfo->query         = NULL;
	coninfo->tablename     = NULL;
	coninfo->username      = "\0";

	/* default scan mode */
	coninfo->scan_mode     = IFX_PLAN_SCAN;
}

static StringInfoData *
ifxFdwOptionsToStringBuf(Oid context)
{
	StringInfoData      *buf;
	struct IfxFdwOption *ifxopt;

	buf = makeStringInfo();
	initStringInfo(buf);

	for (ifxopt = ifx_valid_options; ifxopt->optname; ifxopt++)
	{
		if (context == ifxopt->optcontext)
		{
			appendStringInfo(buf, "%s%s", (buf->len > 0) ? "," : "",
							 ifxopt->optname);
		}
	}

	return buf;
}

/*
 * Check if specified option is actually known
 * to the Informix FDW.
 */
static bool
ifxIsValidOption(const char *option, Oid context)
{
	struct IfxFdwOption *ifxopt;

	for (ifxopt = ifx_valid_options; ifxopt->optname; ifxopt++)
	{
		if (context == ifxopt->optcontext
			&& strcmp(ifxopt->optname, ifxopt->optname) == 0)
		{
			return true;
		}
	}
	/*
	 * Only reached in case of mismatch
	 */
	return false;
}

Datum
ifxCloseConnection(PG_FUNCTION_ARGS)
{
	IfxCachedConnection *conn_cached;
	char                *conname;
	bool                 found;

	/*
	 * Check if connection cache is already
	 * initialized. If not, we don't have anything
	 * to do and can exit immediately.
	 */
	if (!IfxCacheIsInitialized)
		elog(ERROR, "informix connection cache not yet initialized");

	/* Get connection name from argument */
	conname = text_to_cstring(PG_GETARG_TEXT_P(0));
	elog(DEBUG1, "connection identifier \"%s\"",
		 conname);
	Assert(conname);

	/*
	 * Lookup connection.
	 *
	 * We remove the connection handle from the cache first,
	 * closing it afterwards then. This is assumed to be safe,
	 * even when the function is used in a query predicate
	 * where the connection itself is used again. Subsequent
	 * references to this connection will find the cache returning
	 * NULL when requesting the connection identifier and will
	 * reconnect again implicitely.
	 */
	conn_cached = ifxConnCache_rm(conname, &found);

	/* Check wether the handle was valid */
	if (!found || !conn_cached)
	{
		elog(ERROR, "unknown informix connection name: \"%s\"",
			 conname);
		PG_RETURN_VOID();
	}

	/* okay, we have a valid connection handle...close it */
	ifxDisconnectConnection(conname);

    /* Check for any Informix exceptions */
	if (ifxGetSqlStateClass() == IFX_ERROR)
	{
		IfxSqlStateMessage message;

		ifxGetSqlStateMessage(1, &message);
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("could not close specified connection \"%s\"",
						conname),
				 errdetail("informix error: %s, SQLSTATE %s",
						   message.text, message.sqlstate)));
	}

	PG_RETURN_VOID();
}

Datum
ifxGetConnections(PG_FUNCTION_ARGS)
{
	FuncCallContext *fcontext;
	TupleDesc        tupdesc;
	AttInMetadata   *attinmeta;
	int              conn_processed;
	int              conn_expected;
	struct ifx_sp_call_data *call_data;

	attinmeta = NULL;

	/* First call */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		fcontext = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(fcontext->multi_call_memory_ctx);

        /*
		 * Are we called in a context which accepts a record?
		 */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		/*
		 * Check wether informix connection cache is already
		 * initialized. If not, no active connections are present,
		 * thus we don't have to do anything.
		 */
		if (!IfxCacheIsInitialized)
		{
			fcontext->max_calls = 0;
			fcontext->user_fctx = NULL;
		}
		else
		{
			fcontext->max_calls = hash_get_num_entries(ifxCache.connections);
			elog(DEBUG2, "found %d entries in informix connection cache",
				 fcontext->max_calls);

			/*
			 * Retain the status of the hash search and other info.
			 */
			call_data = (struct ifx_sp_call_data *) palloc(sizeof(struct ifx_sp_call_data));
			call_data->hash_status = (HASH_SEQ_STATUS *) palloc(sizeof(HASH_SEQ_STATUS));
			call_data->tupdesc     = tupdesc;

			/*
			 * It is already guaranteed that the connection cache
			 * is alive. Prepare for sequential read of all active connections.
			 */
			hash_seq_init(call_data->hash_status, ifxCache.connections);
			fcontext->user_fctx = call_data;
		}

		/*
		 * Prepare attribute metadata.
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		fcontext->attinmeta = attinmeta;
		MemoryContextSwitchTo(oldcontext);
	}

	fcontext = SRF_PERCALL_SETUP();
	conn_processed = fcontext->call_cntr;
	conn_expected  = fcontext->max_calls;

	if (conn_processed < conn_expected)
	{
		IfxCachedConnection *conn_cached;
		Datum                values[14];
		bool                 nulls[14];
		HeapTuple            tuple;
		Datum                result;

		call_data = (struct ifx_sp_call_data *) fcontext->user_fctx;
		Assert(call_data != NULL);
		conn_cached = (IfxCachedConnection *) hash_seq_search(call_data->hash_status);

		/*
		 * Values array. This will hold the values to be returned.
		 */
		elog(DEBUG2, "connection name %s", conn_cached->con.ifx_connection_name);
		values[0] = PointerGetDatum(cstring_to_text(conn_cached->con.ifx_connection_name));
		values[1] = Int32GetDatum(conn_cached->establishedByOid);
		values[2] = PointerGetDatum(cstring_to_text(conn_cached->con.servername));
		values[3] = PointerGetDatum(cstring_to_text(conn_cached->con.informixdir));
		values[4] = PointerGetDatum(cstring_to_text(conn_cached->con.database));
		values[5] = PointerGetDatum(cstring_to_text(conn_cached->con.username));
		values[6] = Int32GetDatum(conn_cached->con.usage);

		nulls[0] = false;
		nulls[1] = false;
		nulls[2] = false;
		nulls[3] = false;
		nulls[4] = false;
		nulls[5] = false;
		nulls[6] = false;

		/* db_locale and client_locale might be undefined */

		if (conn_cached->con.db_locale != NULL)
		{
			values[7] = PointerGetDatum(cstring_to_text(conn_cached->con.db_locale));
			nulls[7] = false;
		}
		else
		{
			nulls[7] = true;
			values[7] = PointerGetDatum(NULL);
		}

		if (conn_cached->con.client_locale != NULL)
		{
			values[8] = PointerGetDatum(cstring_to_text(conn_cached->con.client_locale));
			nulls[8] = false;
		}
		else
		{
			nulls[8] = true;
			values[8] = PointerGetDatum(NULL);
		}

		/*
		 * Show transaction usage.
		 */
		values[9] = Int32GetDatum(conn_cached->con.tx_enabled);
		nulls[9]  = false;

		/*
		 * Transaction in progress...
		 */
		values[10] = Int32GetDatum(conn_cached->con.tx_in_progress);
		nulls[10]  = false;

		/*
		 * Show wether database is ANSI enabled or not.
		 */
		values[11] = Int32GetDatum(conn_cached->con.db_ansi);
		nulls[11]  = false;

		/*
		 * Additional stats columns...
		 */
		values[12] = Int32GetDatum(conn_cached->con.tx_num_commit);
		nulls[12]  = false;

		values[13] = Int32GetDatum(conn_cached->con.tx_num_rollback);
		nulls[13]  = false;

		/*
		 * Build the result tuple.
		 */
		tuple = heap_form_tuple(call_data->tupdesc, values, nulls);

		/*
		 * Transform the result tuple into a valid datum.
		 */
		result = HeapTupleGetDatum(tuple);

		/*
		 * Finalize...
		 */
		SRF_RETURN_NEXT(fcontext, result);
	}
	else
	{
		call_data = (struct ifx_sp_call_data *) fcontext->user_fctx;

		/*
		 * Done processing. Terminate hash_seq_search(), since we haven't
		 * processed forward until NULL (but only if we had processed
		 * any connections).
		 */
		if ((fcontext->max_calls >= 0) && IfxCacheIsInitialized)
			hash_seq_term(call_data->hash_status);

		SRF_RETURN_DONE(fcontext);
	}
}

/*
 * ifxXactFinalize()
 *
 * Commits or rollbacks a transaction on the remote
 * server, depending on the specified IfxXactAction.
 *
 * Internally, this function makes the specified informix
 * connection current and depending on the specified action
 * commits or rolls back the current transaction. The caller
 * should make sure, that there's really a transaction in
 * progress.
 *
 * If connection_error_ok is true, an error is thrown
 * if the specified cached informix connection can't be made
 * current. Otherwise the loglevel is decreased to a WARNING,
 * indicating the exact SQLSTATE and error message what happened.
 */
static int ifxXactFinalize(IfxCachedConnection *cached,
						   IfxXactAction action,
						   bool connection_error_ok)
{
	int result = -1;
	IfxSqlStateMessage message;

	/*
	 * Make this connection current (otherwise we aren't able to commit
	 * anything.
	 */
	if ((result = ifxSetConnectionIdent(cached->con.ifx_connection_name)) < 0)
	{
		/*
		 * Can't make this connection current, so throw an
		 * ERROR. This will return to this callback by
		 * XACT_EVENT_ABORT and do all necessary cleanup.
		 */
		ifxGetSqlStateMessage(1, &message);

		elog(((connection_error_ok) ? ERROR : WARNING),
			  "informix_fdw: error committing transaction: \"%s\", SQLSTATE %s",
			  message.text, message.sqlstate);
	}

	if (action == IFX_TX_COMMIT)
	{
		/*
		 * Commit the transaction
		 */
		if ((result = ifxCommitTransaction(&cached->con)) < 0)
		{
			/* oops, something went wrong ... */
			ifxGetSqlStateMessage(1, &message);

			/*
			 * Error out in case we can't commit this transaction.
			 */
			elog(ERROR, "informix_fdw: error committing transaction: \"%s\", SQLSTATE %s",
				 message.text, message.sqlstate);
		}
	}
	else if (action == IFX_TX_ROLLBACK)
	{
		/* Rollback current transaction */
		if (ifxRollbackTransaction(&cached->con) < 0)
		{
			/* oops, something went wrong ... */
			ifxGetSqlStateMessage(1, &message);

			/*
			 * Don't throw an error, but emit a warning something went
			 * wrong on the remote server with the SQLSTATE error message.
			 * Otherwise we end up in an endless loop.
			 */
			elog(WARNING, "informix_fdw: error committing transaction: \"%s\"",
				 message.text);
		}
	}

	return result;
}

/*
 * Internal function for ifx_fdw_xact_callback().
 *
 * Depending on the specified XactEvent, rolls a transaction back
 * or commits it on the remote server.
 */
static void ifx_fdw_xact_callback_internal(IfxCachedConnection *cached,
										  XactEvent event)
{
	switch(event)
	{
#if PG_VERSION_NUM >= 90300
		case XACT_EVENT_PRE_COMMIT:
		{
            ifxXactFinalize(cached, IFX_TX_COMMIT, true);
			break;
		}
		case XACT_EVENT_PRE_PREPARE:
		{
			/*
			 * Not supported.
			 *
			 * NOTE: I had a hard time to figure out how this works correctly,
			 *       but fortunately the postgres_fdw shows an example on how to
			 *       do this right: when an ERROR is thrown here, we come back
			 *       later with XACT_EVENT_ABORT, which will then do the whole
			 *       cleanup stuff.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("informix_fdw: cannot prepare a transaction")));
			break;
		}
		case XACT_EVENT_COMMIT:
#else
        case XACT_EVENT_COMMIT:
		{
            ifxXactFinalize(cached, IFX_TX_COMMIT, true);
			break;
		}
#endif
		case XACT_EVENT_PREPARE:
			/* Not reach, since pre-commit does everything required. */
			elog(ERROR, "missed cleaning up connection during pre-commit");
			break;
		case XACT_EVENT_ABORT:
		{
			/*
			 * Beware that we can't throw an error here, since this would bring
			 * us into an endless loop by subsequent triggering XACT_EVENT_ABORT.
			 */
            ifxXactFinalize(cached, IFX_TX_ROLLBACK, false);
		}
	}
}

static void ifx_fdw_xact_callback(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS      hsearch_status;
	IfxCachedConnection *cached;

	/*
	 * No-op if this backend has no in-progress transactions in Informix.
	 */
	if (ifxXactInProgress < 1)
		return;

	/*
	 * We need to scan through all cached connections to check
	 * wether they have in-progress transactions.
	 */
	hash_seq_init(&hsearch_status, ifxCache.connections);
	while ((cached = (IfxCachedConnection *) hash_seq_search(&hsearch_status)))
	{
		/*
		 * No transaction in progress? If true, get to next...
		 */
		if (cached->con.tx_in_progress < 1)
			continue;

		elog(DEBUG3, "informix_fdw: xact_callback on connection \"%s\"",
			 cached->con.ifx_connection_name);

		/*
		 * Execute required actions...
		 */
		ifx_fdw_xact_callback_internal(cached, event);
	}
}
