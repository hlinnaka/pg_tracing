-- Create pg_tracing extension with sampling on
/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000001-0000000000000001-01'*/ CREATE EXTENSION pg_tracing;

-- This will create the following spans (non exhaustive list):
--
-- +-------------------------------------------------------------------------------------+
-- | A: Utility: Create Extension                                                        |
-- +-+-----------------------------------------------------------------------------------+
--   +----------------------------------------------------------------------------------+
--   |B: ProcessUtility: Create Extension                                               |
--   +---+-----------------------------------+---+--------------------------------------+
--       +-----------------------------------+   +-------------------------------------+
--       |C: Utility: Create Function1       |   |E: Utility: Create Function2         |
--       ++----------------------------------+   ++-----------------------------------++
--        +----------------------------------+    +-----------------------------------+
--        |D: ProcessUtility: Create Function|    |F: ProcessUtility: Create Function2|
--        +----------------------------------+    +-----------------------------------+

-- Extract span_ids, start and end of those spans
SELECT span_id AS span_a_id,
        get_epoch(span_start) as span_a_start,
        get_epoch(span_end) as span_a_end
		from pg_tracing_peek_spans where parent_id='0000000000000001' and span_type='Utility query' \gset

SELECT span_id AS span_b_id,
        get_epoch(span_start) as span_b_start,
        get_epoch(span_end) as span_b_end
		from pg_tracing_peek_spans where parent_id=:'span_a_id' and span_type='ProcessUtility' \gset

SELECT span_id AS span_c_id,
        get_epoch(span_start) as span_c_start,
        get_epoch(span_end) as span_c_end
		from pg_tracing_peek_spans where parent_id=:'span_b_id' and span_type='Utility query' limit 1 \gset

SELECT span_id AS span_d_id,
        get_epoch(span_start) as span_d_start,
        get_epoch(span_end) as span_d_end
		from pg_tracing_peek_spans where parent_id=:'span_c_id' and span_type='ProcessUtility' \gset

SELECT span_id AS span_e_id,
        get_epoch(span_start) as span_e_start,
        get_epoch(span_end) as span_e_end
		from pg_tracing_peek_spans where parent_id=:'span_b_id' and span_type='Utility query' limit 1 offset 1 \gset

-- Check that the start and end of those spans are within expectation
SELECT :span_a_start <= :span_b_start AS span_a_starts_first,
		:span_a_end >= :span_b_end AS span_a_ends_last,

		:span_d_end <= :span_c_end AS nested_span_ends_before_parent,
		:span_c_end <= :span_e_start AS next_utility_starts_after;

-- Clean current spans
CALL clean_spans();

--
-- Test that no utility is captured with track_utility off
--

-- Set utility off
SET pg_tracing.track_utility = off;

-- Test utility tracking disabled + full sampling
SET pg_tracing.sample_rate = 1.0;
DROP EXTENSION pg_tracing;
CREATE EXTENSION pg_tracing;
SET pg_tracing.sample_rate = 0.0;

-- View displaying spans with their nested level
CREATE VIEW peek_spans_with_level AS
    WITH RECURSIVE list_trace_spans(trace_id, parent_id, span_id, query_id, span_type, span_operation, span_start, span_end, sql_error_code, userid, dbid, pid, subxact_count, plan_startup_cost, plan_total_cost, plan_rows, plan_width, rows, nloops, shared_blks_hit, shared_blks_read, shared_blks_dirtied, shared_blks_written, local_blks_hit, local_blks_read, local_blks_dirtied, local_blks_written, blk_read_time, blk_write_time, temp_blks_read, temp_blks_written, temp_blk_read_time, temp_blk_write_time, wal_records, wal_fpi, wal_bytes, jit_functions, jit_generation_time, jit_inlining_time, jit_optimization_time, jit_emission_time, startup, parameters, deparse_info, lvl) AS (
        SELECT p.*, 1
        FROM pg_tracing_peek_spans p where not parent_id=ANY(SELECT span_id from pg_tracing_peek_spans)
      UNION ALL
        SELECT s.*, lvl + 1
        FROM pg_tracing_peek_spans s, list_trace_spans st
        WHERE s.parent_id = st.span_id
    ) SELECT * FROM list_trace_spans;

-- Create utility view to keep order stable
CREATE VIEW peek_ordered_spans AS
  SELECT * FROM peek_spans_with_level order by span_start, lvl, span_end, deparse_info;

-- Nothing should have been generated
select count(*) = 0 from pg_tracing_consume_spans;

-- Prepare and execute a prepared statement
PREPARE test_prepared_one_param (integer) AS SELECT $1;
EXECUTE test_prepared_one_param(100);

-- Nothing should be generated
select count(*) = 0 from pg_tracing_consume_spans;

-- Force a query to start from ExecutorRun
SET plan_cache_mode='force_generic_plan';
EXECUTE test_prepared_one_param(200);
SET plan_cache_mode TO DEFAULT;

-- Again, nothing should be generated
select count(*) = 0 from pg_tracing_consume_spans;

--
-- Test that no utility is captured with track_utility off
--

-- Enable utility tracking and track everything
SET pg_tracing.track_utility = on;
SET pg_tracing.sample_rate = 1.0;

-- Prepare and execute a prepared statement
PREPARE test_prepared_one_param_2 (integer) AS SELECT $1;
EXECUTE test_prepared_one_param_2(100);

-- Check the number of generated spans
select count(distinct(trace_id)) from pg_tracing_peek_spans;
-- Check spans of test_prepared_one_param_2 execution
select span_operation, parameters, lvl from peek_ordered_spans;
-- Check the top span (standalone top span has trace_id=parent_id)
select span_operation, parameters, lvl from peek_ordered_spans where right(trace_id, 16) = parent_id;
CALL clean_spans();

-- Test prepared statement with generic plan
SET plan_cache_mode='force_generic_plan';
EXECUTE test_prepared_one_param(200);
SET plan_cache_mode TO DEFAULT;

-- Check the number of generated spans
select count(distinct(trace_id)) from pg_tracing_peek_spans;
-- Check spans of test_prepared_one_param execution
select span_operation, parameters, lvl from peek_ordered_spans;
-- Check the top span (standalone top span has trace_id=parent_id)
select span_operation, parameters, lvl from peek_ordered_spans where right(trace_id, 16) = parent_id;
CALL clean_spans();

-- Second create extension should generate an error that is captured by span
/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000001-0000000000000001-01'*/ CREATE EXTENSION pg_tracing;
select span_operation, parameters, sql_error_code, lvl from peek_ordered_spans where trace_id='00000000000000000000000000000001';

-- Create test table
/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000002-0000000000000002-01'*/ CREATE TABLE test_create_table (a int, b char(20));
-- Check create table spans
select trace_id, span_type, span_operation, lvl from peek_ordered_spans where trace_id='00000000000000000000000000000002';

/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000003-0000000000000003-01'*/ CREATE INDEX test_create_table_index ON test_create_table (a);
-- Check create index spans
select trace_id, span_type, span_operation, lvl from peek_ordered_spans where trace_id='00000000000000000000000000000003';

CREATE OR REPLACE FUNCTION function_with_error(IN anyarray, OUT x anyelement, OUT n int)
    RETURNS SETOF RECORD
    LANGUAGE sql STRICT IMMUTABLE PARALLEL SAFE
    AS 'select s from pg_catalog.generate_series(1, 1, 1) as g(s)';

-- Check that tracing a function call with the wrong number of arguments is managed correctly
/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000004-0000000000000004-01'*/ select function_with_error('{1,2,3}'::int[]);

-- Check lazy function call with error
select trace_id, span_type, span_operation, sql_error_code, lvl from peek_ordered_spans where trace_id='00000000000000000000000000000004';

/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000005-0000000000000005-01'*/ ANALYZE test_create_table;
select trace_id, span_type, span_operation, sql_error_code, lvl from peek_ordered_spans where trace_id='00000000000000000000000000000005';

/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000006-0000000000000006-01'*/ DROP TABLE test_create_table;
select trace_id, span_type, span_operation, sql_error_code, lvl from peek_ordered_spans where trace_id='00000000000000000000000000000006';

-- Cleanup
CALL clean_spans();
SET pg_tracing.track_utility TO DEFAULT;
