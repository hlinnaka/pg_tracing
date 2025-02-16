/*-------------------------------------------------------------------------
 *
 * pg_tracing_planstate.c
 * 		pg_tracing planstate manipulation functions.
 *
 * IDENTIFICATION
 *	  contrib/pg_tracing/pg_tracing_planstate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/pg_prng.h"
#include "pg_tracing.h"
#include "utils/timestamp.h"

#define US_IN_S INT64CONST(1000000)

/* List of planstate to start found for the current query */
static TracedPlanstate * traced_planstates = NULL;
/* Previous value of the ExecProcNode pointer in planstates */
static ExecProcNodeMtd previous_ExecProcNode = NULL;

/* Current available slot in the traced_planstates array */
static int	index_planstart = 0;

/* Maximum elements allocated in the traced_planstates array */
static int	max_planstart = 0;

static void override_ExecProcNode(PlanState *planstate);

/*
 * Reset the traced_planstates array
 */
void
cleanup_planstarts(void)
{
	traced_planstates = NULL;
	max_planstart = 0;
	index_planstart = 0;
}

/*
 * Drop all traced_planstates after the provided nested level
 */
void
drop_traced_planstate(int exec_nested_level)
{
	int			i;
	int			new_index_start = 0;

	for (i = index_planstart; i > 0; i--)
	{
		if (traced_planstates[i - 1].nested_level <= exec_nested_level)
		{
			/*
			 * Found a new planstate from a previous nested level, we can stop
			 * here
			 */
			index_planstart = i;
			return;
		}
		else
			/* the traced_planstate should be dropped */
			new_index_start = i - 1;
	}
	index_planstart = new_index_start;
}

/*
 * If spans from planstate is requested, we override planstate's ExecProcNode's pointer with this function.
 * It will track the time of the first node call needed to place the planstate span.
 */
static TupleTableSlot *
ExecProcNodeFirstPgTracing(PlanState *node)
{
	if (max_planstart == 0)

		/*
		 * Queries calling pg_tracing_spans will have aborted tracing and
		 * everything cleaned.
		 */
		goto exit;

	if (index_planstart >= max_planstart)
	{
		/*
		 * We need to extend the traced_planstates array, switch to pg_tracing
		 * memory context beforehand
		 */
		MemoryContext oldcxt;
		int			old_max_planstart = max_planstart;

		Assert(traced_planstates != NULL);
		max_planstart += 5;
		oldcxt = MemoryContextSwitchTo(pg_tracing_mem_ctx);
		traced_planstates = repalloc0(traced_planstates,
									  old_max_planstart * sizeof(TracedPlanstate),
									  max_planstart * sizeof(TracedPlanstate));
		MemoryContextSwitchTo(oldcxt);
	}

	/* Register planstate start */
	traced_planstates[index_planstart].planstate = node;
	traced_planstates[index_planstart].node_start = GetCurrentTimestamp();
	traced_planstates[index_planstart].span_id = pg_prng_uint64(&pg_global_prng_state);
	traced_planstates[index_planstart].nested_level = exec_nested_level;
	index_planstart++;

exit:
	/* Restore previous exec proc */
	node->ExecProcNode = previous_ExecProcNode;
	return node->ExecProcNode(node);
}

/*
 * Override ExecProcNode on all planstate members
 */
static void
override_member_nodes(PlanState **planstates, int nplans)
{
	int			j;

	for (j = 0; j < nplans; j++)
		override_ExecProcNode(planstates[j]);
}

/*
 * Override ExecProcNode on all planstate of a custom scan
 */
static void
override_custom_scan(CustomScanState *css)
{
	ListCell   *cell;

	foreach(cell, css->custom_ps)
		override_ExecProcNode((PlanState *) lfirst(cell));
}

/*
 * Walk the planstate and override all executor pointer
 */
static void
override_ExecProcNode(PlanState *planstate)
{
	ListCell   *l;

	if (planstate->instrument == NULL)
		/* No instrumentation set, do nothing */
		return;

	/* Walk the outerplan */
	if (outerPlanState(planstate))
		override_ExecProcNode(outerPlanState(planstate));
	/* Walk the innerplan */
	if (innerPlanState(planstate))
		override_ExecProcNode(innerPlanState(planstate));

	/* Handle init plans */
	foreach(l, planstate->initPlan)
	{
		SubPlanState *sstate = (SubPlanState *) lfirst(l);
		PlanState  *splan = sstate->planstate;

		override_ExecProcNode(splan);
	}

	/* Handle sub plans */
	foreach(l, planstate->subPlan)
	{
		SubPlanState *sstate = (SubPlanState *) lfirst(l);
		PlanState  *splan = sstate->planstate;

		override_ExecProcNode(splan);
	}

	/* Handle special nodes with children nodes */
	switch (nodeTag(planstate->plan))
	{
		case T_Append:
			override_member_nodes(((AppendState *) planstate)->appendplans,
								  ((AppendState *) planstate)->as_nplans);
			break;
		case T_MergeAppend:
			override_member_nodes(((MergeAppendState *) planstate)->mergeplans,
								  ((MergeAppendState *) planstate)->ms_nplans);
			break;
		case T_BitmapAnd:
			override_member_nodes(((BitmapAndState *) planstate)->bitmapplans,
								  ((BitmapAndState *) planstate)->nplans);
			break;
		case T_BitmapOr:
			override_member_nodes(((BitmapOrState *) planstate)->bitmapplans,
								  ((BitmapOrState *) planstate)->nplans);
			break;
		case T_SubqueryScan:
			override_ExecProcNode(((SubqueryScanState *) planstate)->subplan);
			break;
		case T_CustomScan:
			override_custom_scan((CustomScanState *) planstate);
			break;
		default:
			break;
	}

	planstate->ExecProcNode = ExecProcNodeFirstPgTracing;
}

/*
 * Override all ExecProcNode pointer of the planstate with ExecProcNodeFirstPgTracing to track first call of a node.
 */
void
setup_ExecProcNode_override(QueryDesc *queryDesc)
{
	if (max_planstart == 0)
	{
		MemoryContext oldcxt;

		max_planstart = 10;
		oldcxt = MemoryContextSwitchTo(pg_tracing_mem_ctx);
		traced_planstates = palloc0(max_planstart * sizeof(TracedPlanstate));
		MemoryContextSwitchTo(oldcxt);
	}
	Assert(queryDesc->planstate->instrument);
	/* Pointer should target ExecProcNodeFirst. Save it to restore it later. */
	previous_ExecProcNode = queryDesc->planstate->ExecProcNode;
	override_ExecProcNode(queryDesc->planstate);
}

/*
 * Iterate over a list of planstate to generate span node
 */
static TimestampTz
generate_member_nodes(PlanState **planstates, int nplans, planstateTraceContext * planstateTraceContext, uint64 parent_id,
					  uint64 query_id, TimestampTz fallback_start, TimestampTz root_end, TimestampTz *latest_end)
{
	int			j;
	TimestampTz child_end;

	for (j = 0; j < nplans; j++)
		child_end = generate_span_from_planstate(planstates[j], planstateTraceContext, parent_id, query_id, fallback_start, root_end, latest_end);
	return child_end;
}

/*
 * Iterate over children of BitmapOr and BitmapAnd
 */
static TimestampTz
generate_bitmap_nodes(PlanState **planstates, int nplans, planstateTraceContext * planstateTraceContext, uint64 parent_id,
					  uint64 query_id, TimestampTz fallback_start, TimestampTz root_end, TimestampTz *latest_end)
{
	int			j;

	/* We keep track of the end of the last sibling end to use as start */
	TimestampTz sibling_end = fallback_start;

	for (j = 0; j < nplans; j++)
		sibling_end = generate_span_from_planstate(planstates[j], planstateTraceContext, parent_id, query_id, sibling_end, root_end, latest_end);
	return sibling_end;
}

/*
 * Iterate over custom scan planstates to generate span node
 */
static TimestampTz
generate_span_from_custom_scan(CustomScanState *css, planstateTraceContext * planstateTraceContext, uint64 parent_id,
							   uint64 query_id, TimestampTz fallback_start, TimestampTz root_end, TimestampTz *latest_end)
{
	ListCell   *cell;
	TimestampTz last_end;

	foreach(cell, css->custom_ps)
		last_end = generate_span_from_planstate((PlanState *) lfirst(cell), planstateTraceContext, parent_id, query_id, fallback_start, root_end, latest_end);
	return last_end;
}

/*
 * Get end time for a span node from the provided planstate.
 */
TimestampTz
get_span_end_from_planstate(PlanState *planstate, TimestampTz plan_start, TimestampTz root_end)
{
	TimestampTz span_end_time;

	if (!INSTR_TIME_IS_ZERO(planstate->instrument->starttime) && root_end > 0)

		/*
		 * Node was ongoing but aborted due to an error, use root end as the
		 * end
		 */
		span_end_time = root_end;
	else if (planstate->instrument->total == 0)
		span_end_time = GetCurrentTimestamp();
	else
	{
		span_end_time = plan_start + planstate->instrument->total * US_IN_S;

		/*
		 * Since we convert from double seconds to microseconds again, we can
		 * have a span_end_time greater to the root due to the loss of
		 * precision for long durations. Fallback to the root end in this
		 * case.
		 */
		if (span_end_time > root_end)
			span_end_time = root_end;
	}
	return span_end_time;
}

/*
 * Fetch the node start of a planstate
 */
static TracedPlanstate *
get_traced_planstate(PlanState *planstate)
{
	for (int i = 0; i < index_planstart; i++)
	{
		if (planstate == traced_planstates[i].planstate)
			return &traced_planstates[i];
	}
	return NULL;
}

/*
 * Get traced_planstate from index
 */
TracedPlanstate *
get_traced_planstate_from_index(int index)
{
	Assert(index > -1);
	Assert(index < max_planstart);
	return &traced_planstates[index];
}

/*
 * Get index in traced_planstates array of a possible parent traced_planstate
 */
int
get_parent_traced_planstate_index(int nested_level)
{
	TracedPlanstate *traced_planstate;

	if (index_planstart >= 2)
	{
		traced_planstate = &traced_planstates[index_planstart - 2];
		if (traced_planstate->nested_level == nested_level && nodeTag(traced_planstate->planstate->plan) == T_ProjectSet)
			return index_planstart - 2;
	}
	if (index_planstart >= 1)
	{
		traced_planstate = &traced_planstates[index_planstart - 1];
		if (traced_planstate->nested_level == nested_level && nodeTag(traced_planstate->planstate->plan) == T_Result)
			return index_planstart - 1;
	}
	return -1;
}

/*
 * Walk through the planstate tree generating a node span for each node.
 */
TimestampTz
generate_span_from_planstate(PlanState *planstate, planstateTraceContext * planstateTraceContext,
							 uint64 parent_id, uint64 query_id,
							 TimestampTz fallback_start, TimestampTz root_end, TimestampTz *latest_end)
{
	ListCell   *l;
	uint64		span_id;
	Span		span_node;
	TracedPlanstate *traced_planstate = NULL;
	TimestampTz span_start;
	TimestampTz span_end;
	TimestampTz child_end = 0;
	bool		haschildren = false;

	/* The node was never executed, skip it */
	if (planstate->instrument == NULL)
		return fallback_start;

	if (!planstate->state->es_finished && !INSTR_TIME_IS_ZERO(planstate->instrument->starttime))
	{
		/*
		 * If the query is in an unfinished state, it means that we're in an
		 * error handler. Stop the node instrumentation to get the latest
		 * known state.
		 */
		InstrStopNode(planstate->instrument, planstate->state->es_processed);
	}

	/*
	 * Make sure stats accumulation is done. Function is a no-op if if was
	 * already done.
	 */
	InstrEndLoop(planstate->instrument);

	if (planstate->instrument->total == 0)
		/* The node was never executed, ignore it */
		return fallback_start;

	switch (nodeTag(planstate->plan))
	{
		case T_BitmapIndexScan:
		case T_BitmapAnd:
		case T_BitmapOr:

			/*
			 * Those nodes won't go through ExecProcNode so we won't have
			 * their start. Fallback to the parent start.
			 */
			span_start = fallback_start;
			break;
		case T_Hash:
			/* For hash node, use the child's start */
			traced_planstate = get_traced_planstate(outerPlanState(planstate));
			span_start = traced_planstate->node_start;
			break;
		default:
			traced_planstate = get_traced_planstate(planstate);
			/* TODO: better fallback if planstate is not found */
			Assert(traced_planstate != NULL);
			span_start = traced_planstate->node_start;
			break;
	}
	Assert(span_start > 0);

	if (traced_planstate != NULL)
	{
		switch (nodeTag(traced_planstate->planstate->plan))
		{
			case T_Result:
			case T_ProjectSet:
				span_id = traced_planstate->span_id;
				break;
			default:
				span_id = pg_prng_uint64(&pg_global_prng_state);
		}
	}
	else
		span_id = pg_prng_uint64(&pg_global_prng_state);

	span_end = get_span_end_from_planstate(planstate, span_start, root_end);

	/* Keep track of the last child end */
	if (*latest_end < span_end)
		*latest_end = span_end;

	haschildren = planstate->initPlan ||
		outerPlanState(planstate) ||
		innerPlanState(planstate) ||
		IsA(planstate->plan, Append) ||
		IsA(planstate->plan, MergeAppend) ||
		IsA(planstate->plan, BitmapAnd) ||
		IsA(planstate->plan, BitmapOr) ||
		IsA(planstate->plan, SubqueryScan) ||
		(IsA(planstate, CustomScanState) &&
		 ((CustomScanState *) planstate)->custom_ps != NIL) ||
		planstate->subPlan;
	if (haschildren)
		planstateTraceContext->ancestors = lcons(planstate->plan, planstateTraceContext->ancestors);

	/* Walk the outerplan */
	if (outerPlanState(planstate))
		generate_span_from_planstate(outerPlanState(planstate), planstateTraceContext, span_id, query_id, span_start, root_end, latest_end);
	/* Walk the innerplan */
	if (innerPlanState(planstate))
		generate_span_from_planstate(innerPlanState(planstate), planstateTraceContext, span_id, query_id, span_start, root_end, latest_end);

	/* Handle init plans */
	foreach(l, planstate->initPlan)
	{
		SubPlanState *sstate = (SubPlanState *) lfirst(l);
		PlanState  *splan = sstate->planstate;
		Span		initplan_span;
		TracedPlanstate *initplan_traced_planstate;
		TimestampTz initplan_span_end;
		uint64		init_plan_span_id;

		InstrEndLoop(splan->instrument);
		if (splan->instrument->total == 0)
			continue;

		/*
		 * There's no specific init_plan planstate so we need to generate a
		 * dedicated span_id. We will still use the child's traced planstate
		 * to get the span's end.
		 */
		initplan_traced_planstate = get_traced_planstate(splan);
		init_plan_span_id = pg_prng_uint64(&pg_global_prng_state);
		initplan_span_end = get_span_end_from_planstate(splan, initplan_traced_planstate->node_start, root_end);

		initplan_span = create_span_node(splan, planstateTraceContext,
										 &init_plan_span_id, span_id, query_id,
										 SPAN_NODE_INIT_PLAN, sstate->subplan->plan_name, initplan_traced_planstate->node_start, initplan_span_end);
		store_span(&initplan_span);
		/* Use the initplan span as a parent */
		generate_span_from_planstate(splan, planstateTraceContext, initplan_span.span_id, query_id, initplan_traced_planstate->node_start, root_end, latest_end);
	}

	/* Handle sub plans */
	foreach(l, planstate->subPlan)
	{
		SubPlanState *sstate = (SubPlanState *) lfirst(l);
		SubPlan    *sp = sstate->subplan;
		PlanState  *splan = sstate->planstate;
		Span		subplan_span;
		TracedPlanstate *subplan_traced_planstate;
		TimestampTz subplan_span_end;
		uint64		subplan_span_id;

		InstrEndLoop(splan->instrument);
		if (splan->instrument->total == 0)
			continue;

		/*
		 * Same as initplan, we create a dedicated span node for subplan but
		 * still use the tracedplan to get the end.
		 */
		subplan_traced_planstate = get_traced_planstate(splan);
		subplan_span_id = pg_prng_uint64(&pg_global_prng_state);
		subplan_span_end = get_span_end_from_planstate(subplan_traced_planstate->planstate, subplan_traced_planstate->node_start, root_end);

		/*
		 * Push subplan as an ancestor so that we can resolve referent of
		 * subplan parameters.
		 */
		planstateTraceContext->ancestors = lcons(sp, planstateTraceContext->ancestors);

		subplan_span = create_span_node(splan, planstateTraceContext,
										&subplan_span_id, span_id, query_id,
										SPAN_NODE_SUBPLAN, sstate->subplan->plan_name, subplan_traced_planstate->node_start, subplan_span_end);
		store_span(&subplan_span);
		child_end = generate_span_from_planstate(splan, planstateTraceContext, subplan_span.span_id, query_id, subplan_traced_planstate->node_start,
												 root_end, latest_end);

		planstateTraceContext->ancestors = list_delete_first(planstateTraceContext->ancestors);
	}

	/* Handle special nodes with children nodes */
	switch (nodeTag(planstate->plan))
	{
		case T_Append:
			child_end = generate_member_nodes(((AppendState *) planstate)->appendplans,
											  ((AppendState *) planstate)->as_nplans, planstateTraceContext, span_id, query_id, span_start, root_end, latest_end);
			break;
		case T_MergeAppend:
			child_end = generate_member_nodes(((MergeAppendState *) planstate)->mergeplans,
											  ((MergeAppendState *) planstate)->ms_nplans, planstateTraceContext, span_id, query_id, span_start, root_end, latest_end);
			break;
		case T_BitmapAnd:
			child_end = generate_bitmap_nodes(((BitmapAndState *) planstate)->bitmapplans,
											  ((BitmapAndState *) planstate)->nplans, planstateTraceContext, span_id, query_id, span_start, root_end, latest_end);
			break;
		case T_BitmapOr:
			child_end = generate_bitmap_nodes(((BitmapOrState *) planstate)->bitmapplans,
											  ((BitmapOrState *) planstate)->nplans, planstateTraceContext, span_id, query_id, span_start, root_end, latest_end);
			break;
		case T_SubqueryScan:
			child_end = generate_span_from_planstate(((SubqueryScanState *) planstate)->subplan, planstateTraceContext, span_id, query_id, span_start, root_end, latest_end);
			break;
		case T_CustomScan:
			child_end = generate_span_from_custom_scan((CustomScanState *) planstate, planstateTraceContext, span_id, query_id, span_start, root_end, latest_end);
			break;
		default:
			break;
	}
	if (haschildren)
		planstateTraceContext->ancestors = list_delete_first(planstateTraceContext->ancestors);

	/* If node had no duration, use the latest end of its child */
	if (planstate->instrument->total == 0)
		span_end = *latest_end;
	/* For special node with children, use the last child's end */
	if (child_end > 0)
		span_end = child_end;

	span_node = create_span_node(planstate, planstateTraceContext, &span_id, parent_id, query_id, SPAN_NODE, NULL, span_start, span_end);
	store_span(&span_node);
	return span_end;
}

/*
 * Create span node for the provided planstate
 */
Span
create_span_node(PlanState *planstate, const planstateTraceContext * planstateTraceContext,
				 uint64 *span_id, uint64 parent_id, uint64 query_id, SpanType span_type,
				 char *subplan_name, TimestampTz span_start, TimestampTz span_end)
{
	Span		span;
	Plan const *plan = planstate->plan;

	/*
	 * Make sure stats accumulation is done. Function is a no-op if if was
	 * already done.
	 */
	InstrEndLoop(planstate->instrument);

	/* We only create span node on node that were executed */
	Assert(planstate->instrument->total > 0);

	begin_span(planstateTraceContext->trace_id, &span, span_type, span_id, parent_id,
			   query_id, &span_start);

	/* first tuple time */
	span.startup = planstate->instrument->startup * NS_PER_S;

	if (span_type == SPAN_NODE)
	{
		/* Generate node specific variable strings and store them */
		const char *node_type;
		const char *deparse_info;
		char const *operation_name;
		int			deparse_info_len;

		node_type = plan_to_node_type(plan);
		span.node_type_offset = add_str_to_trace_buffer(node_type, strlen(node_type));

		operation_name = plan_to_operation(planstateTraceContext, planstate, node_type);
		span.operation_name_offset = add_str_to_trace_buffer(operation_name, strlen(operation_name));

		/* deparse_ctx is NULL if deparsing was disabled */
		if (planstateTraceContext->deparse_ctx != NULL)
		{
			deparse_info = plan_to_deparse_info(planstateTraceContext, planstate);
			deparse_info_len = strlen(deparse_info);
			if (deparse_info_len > 0)
				span.deparse_info_offset = add_str_to_trace_buffer(deparse_info, deparse_info_len);
		}
	}
	else if (subplan_name != NULL)
		span.operation_name_offset = add_str_to_trace_buffer(subplan_name, strlen(subplan_name));

	span.node_counters.rows = (int64) planstate->instrument->ntuples / planstate->instrument->nloops;
	span.node_counters.nloops = (int64) planstate->instrument->nloops;
	span.node_counters.buffer_usage = planstate->instrument->bufusage;
	span.node_counters.wal_usage = planstate->instrument->walusage;

	span.plan_counters.startup_cost = plan->startup_cost;
	span.plan_counters.total_cost = plan->total_cost;
	span.plan_counters.plan_rows = plan->plan_rows;
	span.plan_counters.plan_width = plan->plan_width;

	if (!planstate->state->es_finished)
		span.sql_error_code = planstateTraceContext->sql_error_code;
	end_span(&span, &span_end);

	return span;
}
