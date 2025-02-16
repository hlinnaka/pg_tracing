-- Create test function to sample
CREATE OR REPLACE FUNCTION test_function_project_set(a int) RETURNS SETOF oid AS
$BODY$
BEGIN
	RETURN QUERY SELECT oid from pg_class where oid = a;
END;
$BODY$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION test_function_result(a int, b text) RETURNS void AS
$BODY$
BEGIN
    INSERT INTO pg_tracing_test(a, b) VALUES (a, b);
END;
$BODY$
LANGUAGE plpgsql;


-- Trace a statement with a function call
/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000051-0000000000000051-01'*/ select test_function_project_set(1);

-- The test function call will generate the following spans (non exhaustive list):
-- +---------------------------------------------------------------------------------+
-- | A: Select test_function_project_set(1);                                         |
-- +-+----------++----------------++-------------------------------------------------+
--   |B: Planner||C: ExecutorStart||D: ExecutorRun                                |
--   +----------++----------------+-+---------------------------------------------+
--                                  |E: ProjectSet                              |
--                                  ++---------+------------------------------+-+
--                                   |F: Result| G: Select a from b where...  |
--                                   +---------+--------------+---------------+
--                                                            |H: ExecutorRun|
--                                                            +--------------+

-- Gather span_id, span start and span end of function call statement
SELECT span_id AS span_a_id,
        get_epoch(span_start) as span_a_start,
        get_epoch(span_end) as span_a_end
		from pg_tracing_peek_spans where parent_id='0000000000000051' \gset
SELECT span_id AS span_d_id,
        get_epoch(span_start) as span_d_start,
        get_epoch(span_end) as span_d_end
		from pg_tracing_peek_spans where parent_id=:'span_a_id' and span_type='Executor' and span_operation='ExecutorRun' \gset
SELECT span_id AS span_e_id,
        get_epoch(span_start) as span_e_start,
        get_epoch(span_end) as span_e_end
		from pg_tracing_peek_spans where parent_id=:'span_d_id' and span_type='ProjectSet' \gset
SELECT span_id AS span_g_id,
        get_epoch(span_start) as span_g_start,
        get_epoch(span_end) as span_g_end
		from pg_tracing_peek_spans where parent_id=:'span_e_id' and span_type='Result' \gset
SELECT span_id AS span_g_id,
        get_epoch(span_start) as span_g_start,
        get_epoch(span_end) as span_g_end
		from pg_tracing_peek_spans where parent_id=:'span_e_id' and span_type='Select query' \gset
SELECT span_id AS span_h_id,
        get_epoch(span_start) as span_h_start,
        get_epoch(span_end) as span_h_end
		from pg_tracing_peek_spans where parent_id=:'span_g_id' and span_operation='ExecutorRun' \gset

-- Check that spans' start and end are within expection
SELECT :span_a_start <= :span_d_start AS top_query_before_run,
		:span_a_end >= :span_d_end AS top_ends_after_run_end,

		:span_d_start <= :span_e_start AS top_run_starts_before_project,

		:span_d_end >= :span_e_end AS top_run_ends_after_project_end,
		:span_d_end >= :span_h_end AS top_run_ends_before_select_end,
		:span_d_end >= :span_g_end AS top_run_ends_after_nested_run_end;

SELECT
		:span_g_end >= :span_h_start AS nested_result_ends_before_parse,
		:span_h_end <= :span_g_end AS nested_parse_ends_before_select,

		:span_h_start >= :span_g_start AS run_starts_after_parent_select,
		:span_h_end <= :span_g_end AS run_ends_after_select_end;

-- Check that the root span is the longest one
WITH max_end AS (select max(span_end) from pg_tracing_peek_spans)
SELECT span_end = max_end.max from pg_tracing_peek_spans, max_end
    where span_id = :'span_a_id';

-- Check that ExecutorRun is attached to the nested top span
SELECT span_operation, deparse_info from pg_tracing_peek_spans where parent_id=:'span_h_id' order by span_operation;

-- Check tracking with top tracking
SET pg_tracing.track = 'top';
/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000052-0000000000000052-01'*/ select test_function_project_set(1);
SELECT count(*) from pg_tracing_consume_spans where trace_id='00000000000000000000000000000052';

-- Check tracking with no tracking
SET pg_tracing.track = 'none';
/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000053-0000000000000053-01'*/ select test_function_project_set(1);
SELECT count(*) from pg_tracing_consume_spans where trace_id='00000000000000000000000000000053';

-- Reset tracking setting
SET pg_tracing.track TO DEFAULT;

-- Create test procedure
CREATE OR REPLACE PROCEDURE sum_one(i int) AS $$
DECLARE
  r int;
BEGIN
  SELECT (i + i)::int INTO r;
END; $$ LANGUAGE plpgsql;

-- Test tracking of procedure with utility tracking enabled
SET pg_tracing.track_utility=on;
/*traceparent='00-00000000000000000000000000000054-0000000000000054-01'*/ CALL sum_one(3);
select span_operation, lvl FROM peek_ordered_spans where trace_id='00000000000000000000000000000054';

-- Test again with utility tracking disabled
SET pg_tracing.track_utility=off;
/*traceparent='00-00000000000000000000000000000055-0000000000000055-01'*/ CALL sum_one(10);
select span_operation, lvl FROM peek_ordered_spans where trace_id='00000000000000000000000000000055';

-- Create immutable function
CREATE OR REPLACE FUNCTION test_immutable_function(a int) RETURNS oid
AS 'SELECT oid from pg_class where oid = a;'
LANGUAGE sql IMMUTABLE;

/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000056-0000000000000056-01'*/ select test_immutable_function(1);
select span_operation, lvl FROM peek_ordered_spans where trace_id='00000000000000000000000000000056';

-- Create function with generate series

CREATE OR REPLACE FUNCTION test_generate_series(IN anyarray, OUT x anyelement)
    RETURNS SETOF anyelement
    LANGUAGE sql
    AS 'select * from pg_catalog.generate_series(array_lower($1, 1), array_upper($1, 1), 1)';

/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000057-0000000000000057-01'*/ select test_generate_series('{1,2,3,4}'::int[]) FROM (VALUES (1,2)) as t;

SELECT span_id AS span_project_set_id,
        get_epoch(span_start) as span_project_set_start,
        get_epoch(span_end) as span_project_set_end
		from pg_tracing_peek_spans where span_type='ProjectSet' \gset

SELECT span_id AS span_result_id,
        get_epoch(span_start) as span_result_start,
        get_epoch(span_end) as span_result_end
		from pg_tracing_peek_spans where parent_id=:'span_project_set_id' and span_type='Result' \gset

select span_operation, lvl FROM peek_ordered_spans where trace_id='00000000000000000000000000000057';

-- +-----------------------------------------------------------+
-- | A: Select test_function(1);                               |
-- +-+----------+---+--------------------------------------+---+
--   |B: Planner|   |C: ExecutorRun                        |
--   +----------+   ++-------------------------------------+
--                   |D: Result                           |
--                   +----+-----------------------------+-+
--                        |E: Insert INTO...            |
--                        +---+--------------+----------+
--                            |F: ExecutorRun|
--                            +--------------+

-- Check function with result node
/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000058-0000000000000058-01'*/ select test_function_result(1, 'test');

-- Gather span_id, span start and span end of function call statement
SELECT span_id AS span_a_id,
        get_epoch(span_start) as span_a_start,
        get_epoch(span_end) as span_a_end
		from pg_tracing_peek_spans where parent_id='0000000000000058' \gset
SELECT span_id AS span_c_id,
        get_epoch(span_start) as span_c_start,
        get_epoch(span_end) as span_c_end
		from pg_tracing_peek_spans where parent_id=:'span_a_id' and span_type='Executor' and span_operation='ExecutorRun' \gset
SELECT span_id AS span_d_id,
        get_epoch(span_start) as span_d_start,
        get_epoch(span_end) as span_d_end
		from pg_tracing_peek_spans where parent_id=:'span_c_id' and span_type='Result' \gset
SELECT span_id AS span_e_id,
        get_epoch(span_start) as span_e_start,
        get_epoch(span_end) as span_e_end
		from pg_tracing_peek_spans where parent_id=:'span_d_id' and span_type='Insert query' \gset

select span_operation, lvl FROM peek_ordered_spans where trace_id='00000000000000000000000000000058';

-- Cleanup
CALL clean_spans();
