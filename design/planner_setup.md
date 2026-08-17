# NeoPostGraph Execution Layer: Internal Node Architecture and Join Mechanics

## 1. Join Mechanics & Optimizer Integration

NeoPostGraph does not build a custom join algorithm; it manipulates PostgreSQL's Cost-Based Optimizer (CBO) by injecting a logical dummy `RangeTblEntry` and exposing specific physical `CustomPath` options. This forces the CBO to naturally evaluate graph pointer-chasing against standard relational set-intersections.

### 1.1 The Parameterized Path & Nested Loop Join
When traversing a highly selective path (e.g., a specific user's connections), pointer-chasing is optimal.
*   **The Path Definition:** We provide a `CustomPath` that requires parameterized input (specifically, a `start_id`).
*   **The Join Mapping:** The PostgreSQL optimizer matches this path with a **Nested Loop Join**. The outer relation (the driving vertex) yields a 64-bit ID[cite: 2]. This ID is passed directly into our Custom Scan node as an execution parameter.
*   **Execution:** The Custom Scan executes an O(1) lookup in the Physical Mapping Table and traverses only the specific linked/array lists for that outer ID[cite: 1, 2].

### 1.2 The Full Scan Path & Hash Join
When executing a bidirectional "meet-in-the-middle" query (e.g., matching millions of records on both sides of an edge), parameterizing individual lookups becomes mathematically cost-prohibitive.
*   **The Path Definition:** We provide a second `CustomPath` for the exact same dummy table, but this path claims it can yield the entire dataset without parameters.
*   **The Join Mapping:** The optimizer selects this path and places it beneath a **Hash Join** (or Merge Join). 
*   **Execution:** The Custom Scan node ignores the Physical Mapping Table's 64-bit ID lookup. Instead, it performs a sequential scan across the entire Array List and Linked List tables for that specific edge label, dumping `(start_id, other_id)` pairs into PostgreSQL's in-memory hash table for set intersection.

---

## 2. Custom Scan Node Internal State (`NeoEdgeScanState`)

To act as a seamless tuple generator for the PostgreSQL executor, the Custom Scan node must maintain strict internal state between `ExecCustomScan` calls. Since a single vertex might have thousands of edges spanning across a "rotating chain" of linked list segments and paginated array lists[cite: 1, 2], the state machine must track exactly where it left off.

Here is the breakdown of the internal struct fields governing the node's execution context:

```c
typedef struct NeoEdgeScanState {
    CustomScanState css;  // Base PostgreSQL struct

    /* Execution Context */
    int64 current_start_id;      // The 64-bit driving vertex ID (from outer plan)
    bool needs_properties;       // Flag set during ExecInit if targetlist/quals demand properties
    uint32 target_other_lid;     // Pushed down Label ID for O(1) topological filtering

    /* Routing & Location State */
    Oid current_tbl_oid;         // The Postgres OID of the table currently being scanned
    ItemPointerData current_tid; // The physical TID we are currently evaluating

    /* State Machine Phase */
    typedef enum {
        PHASE_INIT,              // Need to fetch routing pointers
        PHASE_LINKED_LIST,       // Scanning uncompacted log segments
        PHASE_ARRAY_LIST,        // Scanning compacted base arrays
        PHASE_DONE               // Exhausted connections for current_start_id
    } ScanPhase phase;

    /* Array List Specific Cursor */
    int32 array_cursor;          // Index position inside the current adj_list array
    int32 array_max_elements;    // Total edges in the current array list page

} NeoEdgeScanState;
```

---

## 3. The `ExecCustomScan` State Machine

When the PostgreSQL executor requests the next tuple (`ExecCustomScan`), the node utilizes the internal state fields to yield exactly one valid edge, or returns `NULL` when exhausted.

### Phase 1: `PHASE_INIT` (Routing)
1.  If parametrized, retrieve the `current_start_id` from the outer plan slot.
2.  Query the Physical Routing Map using the `current_start_id` to acquire the `e_tbl_id` and `e_itemptr`[cite: 2].
3.  Set `current_tbl_oid` and `current_tid` to the returned values.
4.  Because new writes prepend to a rotating chain, this pointer explicitly represents the head of the active write-optimized linked list[cite: 1, 2].
5.  Transition `phase` to `PHASE_LINKED_LIST`.

### Phase 2: `PHASE_LINKED_LIST` (The Uncompacted Log)
1.  Read the tuple at `current_tid` from the linked list segment.
2.  **Visibility:** Evaluate the tuple's standard PostgreSQL header (xmin/xmax). If invisible to the current snapshot, skip to step 4.
3.  **Topology Filter:** Evaluate the `other_lid` stored directly on the edge[cite: 2]. If `target_other_lid` is set and does not match, skip to step 4.
4.  **Pointer Traversal:** Update `current_tid` and `current_tbl_oid` using the tuple's `prev_itemptr` and `prev_tbl` fields (which point backward toward older segments or the array list)[cite: 2].
5.  **Transition Check:** If the previous pointer indicates the start of the Array List table type, transition `phase` to `PHASE_ARRAY_LIST` and reset `array_cursor = 0`.
6.  **Yield:** If steps 2 and 3 passed, construct and yield the virtual tuple. 

### Phase 3: `PHASE_ARRAY_LIST` (The Compacted Base)
1.  Read the dense `adj_list` array located at `current_tid`[cite: 2].
2.  Access the element at `array_cursor`.
3.  **Visibility:** Evaluate the independent `xmin`/`xmax` values stored explicitly on that specific array entry[cite: 2]. If invisible, increment `array_cursor` and repeat.
4.  **Topology Filter:** Evaluate the `other_lid` stored in the array entry[cite: 2]. If it fails, increment `array_cursor` and repeat.
5.  **Pagination Check:** Increment `array_cursor`. If `array_cursor >= array_max_elements`, read the `next_itemptr` from the current page[cite: 2]. If null, transition to `PHASE_DONE`. If valid, update `current_tid` and reset `array_cursor = 0`.
6.  **Yield:** Construct and yield the virtual tuple.

### Phase 4: `PHASE_DONE`
1.  Return an empty `TupleTableSlot` to signal to the PostgreSQL executor that this specific parameterized lookup is complete.

---

## 4. Property Materialization (Lazy Fetch)

If the `ExecInitCustomScan` function determines `needs_properties == true` (because of a `WHERE` clause or `RETURN` projection), the yielding logic intercepts the tuple before returning it to the executor.

1.  The Custom Scan extracts the `e_id` (the 64-bit Edge ID)[cite: 2].
2.  It queries the Edge Physical Mapping Table to get the `ItemPointer` for the Edge Store[cite: 2].
3.  It accesses the append-style UNDO chain to fetch the full physical representation of the edge[cite: 2].
4.  If a `Qual` (filter) is present, it executes PostgreSQL's `ExecQual` on the properties within the scan node, immediately discarding the tuple if it fails, thereby saving the executor overhead.