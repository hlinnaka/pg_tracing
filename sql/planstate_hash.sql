/*dddbs='postgres.db',traceparent='00-00000000000000000000000000000001-0000000000000001-01'*/ select count(*) from pg_tracing_test r join pg_tracing_test s using (a);
SELECT span_operation, deparse_info, parameters, lvl from peek_ordered_spans where trace_id='00000000000000000000000000000001';

-- +-----------------------------------------+
-- | A: HashJoin                             |
-- ++-----------------+----------------------+
--  | B: SeqScan      |
--  +-----------------+
--    +--------------------+
--    | C: Hash            |
--    +--------------------+
--    | D: SeqScan   |
--    +--------------+

SELECT span_id AS span_a_id,
        get_epoch(span_start) as span_a_start,
        get_epoch(span_end) as span_a_end
		from pg_tracing_peek_spans
        where trace_id='00000000000000000000000000000001' AND span_operation='Hash Join' \gset
SELECT span_id AS span_b_id,
        get_epoch(span_start) as span_b_start,
        get_epoch(span_end) as span_b_end
		from pg_tracing_peek_spans
        where parent_id =:'span_a_id' and span_operation='SeqScan on pg_tracing_test r' \gset
SELECT span_id AS span_c_id,
        get_epoch(span_start) as span_c_start,
        get_epoch(span_end) as span_c_end
		from pg_tracing_peek_spans
        where parent_id =:'span_a_id' and span_operation='Hash' \gset
SELECT span_id AS span_d_id,
        get_epoch(span_start) as span_d_start,
        get_epoch(span_end) as span_d_end
		from pg_tracing_peek_spans
        where parent_id =:'span_c_id' and span_operation='SeqScan on pg_tracing_test s' \gset

SELECT :span_a_end >= :span_c_end as root_ends_last,
       :span_c_start >= :span_b_start as hash_start_after_seqscan,
       :span_c_start = :span_d_start as hash_start_same_as_child_seqscan,
       :span_d_end <= :span_c_end as nested_seq_scan_end_before_parent;

-- Clean created spans
CALL clean_spans();
