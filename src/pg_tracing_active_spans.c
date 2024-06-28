/*-------------------------------------------------------------------------
 *
 * pg_tracing_active_spans.c
 * 		functions managing active spans.
 *
 * IDENTIFICATION
 *	  src/pg_tracing_active_spans.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pg_tracing.h"
#include "access/parallel.h"

/* Stack of active spans */
static pgTracingSpans * active_spans = NULL;

/*
 * Active parse for the next statement. Used to deal with extended
 * protocol parsing the next statement while tracing of the previous
 * statement is still ongoing. We save the span in next_active_span and
 * restore it once tracing of the previous statement is finished.
 */
static Span next_active_span;

/*
 * Add the worker name to the provided stringinfo
 */
static int
add_worker_name_to_trace_buffer(int parallel_worker_number)
{
	char	   *worker_name = psprintf("Worker %d", parallel_worker_number);

	return add_str_to_trace_buffer(worker_name, strlen(worker_name));
}

void
cleanup_active_spans(void)
{
	active_spans = NULL;
}

/*
 * Push a new active span to the active_spans stack
 */
static Span *
allocate_new_active_span(MemoryContext context)
{
	Span	   *span;

	if (active_spans == NULL)
	{
		active_spans = MemoryContextAllocZero(context,
											  sizeof(pgTracingSpans) + 10 * sizeof(Span));
		active_spans->max = 10;
	}
	else if (active_spans->end >= active_spans->max)
	{
		int			old_spans_max = active_spans->max;

		active_spans->max *= 2;
		active_spans = repalloc0(active_spans,
								 sizeof(pgTracingSpans) + old_spans_max * sizeof(Span),
								 sizeof(pgTracingSpans) + old_spans_max * 2 * sizeof(Span));
	}

	span = &active_spans->spans[active_spans->end++];
	span->nested_level = nested_level;
	span->span_id = 0;
	return span;
}

/*
 * Get the latest active span
 */
Span *
peek_active_span(void)
{
	if (active_spans == NULL || active_spans->end == 0)
		return NULL;
	return &active_spans->spans[active_spans->end - 1];
}

/*
 * Get the active span matching the current nested level.
 * Returns NULL if there's no span or it doesn't match the level.
 */
static Span * peek_active_span_for_current_level(void)
{
	Span	   *span = peek_active_span();

	if (span == NULL)
		return NULL;

	if (span->nested_level != nested_level)
		return NULL;

	return span;
}

/*
 * Pop the latest active span
 *
 * If end_time is provided, the span is ended and stored.
 */
Span *
pop_active_span(const TimestampTz *end_time)
{
	Span	   *span;

	if (active_spans == NULL || active_spans->end == 0)
		return NULL;

	span = &active_spans->spans[--active_spans->end];
	if (end_time != NULL)
	{
		end_span(span, end_time);
		store_span(span);
	}
	return span;
}

/*
 * Start a new active span if we've entered a new nested level or if the previous
 * span at the same level ended.
 */
static void
begin_active_span(const SpanContext * span_context, Span * span,
				  SpanType span_type, const JumbleState *jstate,
                  const char *query_text, bool export_parameters, Span * parent_span)
{
    const Query *query = span_context->query;
    const PlannedStmt *pstmt = span_context->pstmt;
	int			query_len;
	const char *normalised_query;
	uint64		parent_id;
	int8		parent_planstate_index = -1;
	uint64		query_id;

	if (nested_level == 0)
		/* Root active span, use the parent id from the trace context */
		parent_id = span_context->traceparent->parent_id;
	else
	{
		TracedPlanstate *parent_traced_planstate = NULL;

		Assert(parent_span != NULL);

		/*
		 * We're in a nested level, check if we have a parent planstate and
		 * use it as a parent span
		 */
		parent_planstate_index = get_parent_traced_planstate_index(nested_level);
		if (parent_planstate_index > -1)
			parent_traced_planstate = get_traced_planstate_from_index(parent_planstate_index);

		/*
		 * Both planstate and previous top span can be the parent for the new
		 * top span, we use the most recent as a parent
		 */
		if (parent_traced_planstate != NULL && parent_traced_planstate->node_start >= parent_span->start)
			parent_id = parent_traced_planstate->span_id;
		else
			parent_id = parent_span->span_id;
	}

	if (pstmt)
		query_id = pstmt->queryId;
	else
		query_id = query->queryId;

	begin_span(span_context->traceparent->trace_id, span, span_type,
			   NULL, parent_id, query_id, &span_context->start_time);
	/* Keep track of the parent planstate index */
	span->parent_planstate_index = parent_planstate_index;

	if (IsParallelWorker())
	{
		/*
		 * In a parallel worker, we use the worker name as the span's
		 * operation
		 */
		span->operation_name_offset = add_worker_name_to_trace_buffer(ParallelWorkerNumber);
		return;
	}

	if (jstate && jstate->clocations_count > 0 && query != NULL)
	{
		/* jstate is available, normalise query and extract parameters' values */
		char	   *param_str;
		int			param_len;

		query_len = query->stmt_len;
		normalised_query = normalise_query_parameters(jstate, query_text,
													  query->stmt_location, &query_len,
													  &param_str, &param_len);
		Assert(param_len > 0);
		if (export_parameters)
			span->parameter_offset = add_str_to_trace_buffer(param_str, param_len);
	}
	else
	{
		/*
		 * No jstate available, normalise query but we won't be able to
		 * extract parameters
		 */
		int			stmt_location;

		if (query != NULL && query->stmt_len > 0)
		{
			query_len = query->stmt_len;
			stmt_location = query->stmt_location;
		}
		else if (pstmt != NULL && pstmt->stmt_location != -1 && pstmt->stmt_len > 0)
		{
			query_len = pstmt->stmt_len;
			stmt_location = pstmt->stmt_location;
		}
		else
		{
			query_len = strlen(query_text);
			stmt_location = 0;
		}
		normalised_query = normalise_query(query_text, stmt_location, &query_len);
	}
	if (query_len > 0)
		span->operation_name_offset = add_str_to_trace_buffer(normalised_query,
															  query_len);
}

/*
 * Push a new span that will be the child of the latest active span
 */
Span *
push_child_active_span(MemoryContext context, const SpanContext * span_context,
					   SpanType span_type, const Query *query)
{
	Span	   *parent_span = peek_active_span();
	Span	   *span = allocate_new_active_span(context);
	uint64		query_id;

	if (span_context->pstmt)
		query_id = span_context->pstmt->queryId;
	else
		query_id = query->queryId;

	begin_span(span_context->traceparent->trace_id, span, span_type, NULL,
			   parent_span->span_id, query_id, &span_context->start_time);
	return span;
}

/*
 * Initialise buffers if we are in a new nested level and start associated active span.
 * If the active span already exists for the current nested level, this has no effect.
 *
 * This needs to be called every time an active span could be started: post parse,
 * planner, executor start and process utility
 *
 * In case of extended protocol using transaction block, the parse of the next
 * statement happens while the previous span is still ongoing. To avoid conflict,
 * we keep the active span for the next statement in next_active_span.
 */
Span *
push_active_span(MemoryContext context, const SpanContext * span_context, SpanType span_type,
				 JumbleState *jstate, const char *query_text, HookPhase hook_phase, bool export_parameters)
{
	Span	   *span = peek_active_span_for_current_level();
	Span	   *parent_span = peek_active_span();

	/* Either query or planned statement should be defined */
	Assert(span_context->query || span_context->pstmt);
	if (span == NULL)
	{
		/*
		 * No active span or it belongs to the previous level, allocate a new
		 * one
		 */
		span = allocate_new_active_span(context);
		if (next_active_span.span_id > 0)
		{
			/* next_active_span is valid, use it and reset it */
			*span = next_active_span;
			reset_span(&next_active_span);
			return span;
		}
	}
	else
	{
		if (hook_phase != HOOK_PARSE || nested_level > 0)
			return span;

		/*
		 * We're at level 0, in a parse hook while we still have an active
		 * span. This is the parse command for the next statement, save it in
		 * next_active_span.
		 */
		span = &next_active_span;
	}

	begin_active_span(span_context, span, span_type, jstate,
					  query_text, export_parameters, parent_span);
	return span;
}
