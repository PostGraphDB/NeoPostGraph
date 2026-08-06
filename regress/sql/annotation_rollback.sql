/*
 * Copyright (C) 2026 PostGraphDB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */ 
LOAD 'neopostgraph';
SET search_path TO  public, pg_catalog, NeoPostGraph;
-- ===================================================================
-- NeoPostGraph: Annotation Rollback & Cache Invalidation Tests
-- ===================================================================
\set ON_ERROR_STOP off

-- Setup a fresh graph for rollback testing
SELECT create_graph('rollback_db', 'public');
SELECT create_vlabel('rollback_db', 'entity');

-- Insert a base vertex (ID 1) using your native insert_vertex/vertex_build logic
DO $$
DECLARE
    g_id INT;
    l_id INT;
BEGIN
    SELECT id INTO g_id FROM np_graph WHERE name = 'rollback_db';
    EXECUTE format('SELECT id FROM np_vertex_label_%s WHERE ltree = ''_.entity''', g_id) INTO l_id;
    
    EXECUTE format(
        'SELECT insert_vertex(vertex_build(1::int8, %s::int4, %s::int4, 0::smallint, ''{"id": 1, "label": "entity", "properties": {}}''::gtype))', 
        g_id, l_id
    );
END $$;

-- -------------------------------------------------------------------
-- TEST 1: DDL Rollback (Schema Cache Invalidation)
-- -------------------------------------------------------------------
BEGIN;
SELECT add_annotation_label('rollback_db', 'entity', 'GhostTag');
-- Simulating a failure or user abort
ROLLBACK;

-- Verification: 'GhostTag' should NOT exist. 
DO $$
DECLARE
    g_id INT;
    l_id INT;
BEGIN
    SELECT id INTO g_id FROM np_graph WHERE name = 'rollback_db';
    EXECUTE format('SELECT id FROM np_vertex_label_%s WHERE ltree = ''_.entity''', g_id) INTO l_id;
    
    -- This MUST throw an error because 'GhostTag' was rolled back
    PERFORM add_vertex_annotation_label(1, l_id, g_id, 'GhostTag');
END $$;

\set ON_ERROR_STOP on

-- -------------------------------------------------------------------
-- TEST 2: DML Rollback (MVCC Bitset Reversion)
-- -------------------------------------------------------------------
-- First, successfully add a real annotation to the schema
SELECT add_annotation_label('rollback_db', 'entity', 'RealTag');

BEGIN;
DO $$
DECLARE
    g_id INT;
    l_id INT;
BEGIN
    SELECT id INTO g_id FROM np_graph WHERE name = 'rollback_db';
    EXECUTE format('SELECT id FROM np_vertex_label_%s WHERE ltree = ''_.entity''', g_id) INTO l_id;
    
    -- Add the annotation bit to the vertex inside the transaction
    PERFORM add_vertex_annotation_label(1, l_id, g_id, 'RealTag');
END $$;
-- Simulating a failure during the DML process
ROLLBACK;

-- Verification: The vertex should NOT have the 'RealTag' bit applied.
DO $$
DECLARE
    g_id INT;
    l_id INT;
    v_json JSONB;
BEGIN
    SELECT id INTO g_id FROM np_graph WHERE name = 'rollback_db';
    EXECUTE format('SELECT id FROM np_vertex_label_%s WHERE ltree = ''_.entity''', g_id) INTO l_id;
    
    EXECUTE format('SELECT vertex FROM np_vertex_%s_%s WHERE id = 1', g_id, l_id) INTO v_json;
    RAISE NOTICE 'Vertex after DML rollback: %', v_json;
END $$;