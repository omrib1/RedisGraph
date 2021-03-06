/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "op_conditional_traverse.h"
#include "shared/print_functions.h"
#include "../../util/arr.h"
#include "../../GraphBLASExt/GxB_Delete.h"
#include "../../arithmetic/arithmetic_expression.h"

/* Forward declarations. */
static Record CondTraverseConsume(OpBase *opBase);
static OpResult CondTraverseReset(OpBase *opBase);
static void CondTraverseFree(OpBase *opBase);

static void _setupTraversedRelations(CondTraverse *op, QGEdge *e) {
	uint reltype_count = array_len(e->reltypeIDs);
	if(reltype_count > 0) {
		array_clone(op->edgeRelationTypes, e->reltypeIDs);
		op->edgeRelationCount = reltype_count;
	} else {
		op->edgeRelationCount = 1;
		op->edgeRelationTypes = array_new(int, 1);
		op->edgeRelationTypes = array_append(op->edgeRelationTypes, GRAPH_NO_RELATION);
	}
}

// Updates query graph edge.
static int _CondTraverse_SetEdge(CondTraverse *op, Record r) {
	// Consumed edges connecting current source and destination nodes.
	if(!array_len(op->edges)) return 0;

	Edge *e = op->edges + (array_len(op->edges) - 1);
	Record_AddEdge(r, op->edgeRecIdx, *e);
	array_pop(op->edges);
	return 1;
}

// Collect edges between the source and destination nodes.
static void __CondTraverse_CollectEdges(CondTraverse *op, int src, int dest) {
	Node *srcNode = Record_GetNode(op->r, src);
	Node *destNode = Record_GetNode(op->r, dest);
	for(int i = 0; i < op->edgeRelationCount; i++) {
		Graph_GetEdgesConnectingNodes(op->graph,
									  ENTITY_GET_ID(srcNode),
									  ENTITY_GET_ID(destNode),
									  op->edgeRelationTypes[i],
									  &op->edges);
	}
}

// Collect edges between the source and destination nodes matching the op's traversal direction.
static void _CondTraverse_CollectEdges(CondTraverse *op, int src, int dest) {
	switch(op->direction) {
	case GRAPH_EDGE_DIR_OUTGOING:
		__CondTraverse_CollectEdges(op, op->srcNodeIdx, op->destNodeIdx);
		return;
	case GRAPH_EDGE_DIR_INCOMING:
		// If we're traversing incoming edges, swap the source and destination.
		__CondTraverse_CollectEdges(op, op->destNodeIdx, op->srcNodeIdx);
		return;
	case GRAPH_EDGE_DIR_BOTH:
		// If we're traversing in both directions, collect edges in both directions.
		__CondTraverse_CollectEdges(op, op->srcNodeIdx, op->destNodeIdx);
		__CondTraverse_CollectEdges(op, op->destNodeIdx, op->srcNodeIdx);
		return;
	}
}

static void _populate_filter_matrix(CondTraverse *op) {
	for(uint i = 0; i < op->recordsLen; i++) {
		Record r = op->records[i];
		/* Update filter matrix F, set row i at position srcId
		 * F[i, srcId] = true. */
		Node *n = Record_GetNode(r, op->srcNodeIdx);
		NodeID srcId = ENTITY_GET_ID(n);
		GrB_Matrix_setElement_BOOL(op->F, true, i, srcId);
	}
}

/* Evaluate algebraic expression:
 * prepends filter matrix as the left most operand
 * perform multiplications
 * set iterator over result matrix
 * removed filter matrix from original expression
 * clears filter matrix. */
void _traverse(CondTraverse *op) {
	// Create both filter and result matrices.
	if(op->F == GrB_NULL) {
		size_t required_dim = Graph_RequiredMatrixDim(op->graph);
		GrB_Matrix_new(&op->M, GrB_BOOL, op->recordsCap, required_dim);
		GrB_Matrix_new(&op->F, GrB_BOOL, op->recordsCap, required_dim);
	}

	// Populate filter matrix.
	_populate_filter_matrix(op);
	// Clone expression, as we're about to modify the structure with Optimize.
	AlgebraicExpression *clone = AlgebraicExpression_Clone(op->ae);
	// Prepend matrix to algebraic expression, as the left most operand.
	AlgebraicExpression_MultiplyToTheLeft(&clone, op->F);
	// TODO: consider performing optimization as part of evaluation.
	AlgebraicExpression_Optimize(&clone);
	// Evaluate expression.
	AlgebraicExpression_Eval(clone, op->M);
	// Free clone.
	AlgebraicExpression_Free(clone);

	if(op->iter == NULL) GxB_MatrixTupleIter_new(&op->iter, op->M);
	else GxB_MatrixTupleIter_reuse(op->iter, op->M);

	// Clear filter matrix.
	GrB_Matrix_clear(op->F);
}

static inline int CondTraverseToString(const OpBase *ctx, char *buf, uint buf_len) {
	return TraversalToString(ctx, buf, buf_len, ((const CondTraverse *)ctx)->ae);
}

OpBase *NewCondTraverseOp(const ExecutionPlan *plan, Graph *g, AlgebraicExpression *ae,
						  uint records_cap) {
	CondTraverse *op = rm_calloc(1, sizeof(CondTraverse));
	op->graph = g;
	op->ae = ae;
	op->r = NULL;
	op->iter = NULL;
	op->edges = NULL;
	op->F = GrB_NULL;
	op->M = GrB_NULL;
	op->recordsLen = 0;
	op->direction = GRAPH_EDGE_DIR_OUTGOING;
	op->edgeRelationTypes = NULL;
	op->recordsCap = records_cap;
	op->records = rm_calloc(op->recordsCap, sizeof(Record));

	// Set our Op operations
	OpBase_Init((OpBase *)op, OPType_CONDITIONAL_TRAVERSE, "Conditional Traverse", NULL,
				CondTraverseConsume, CondTraverseReset, CondTraverseToString, CondTraverseFree, false, plan);

	assert(OpBase_Aware((OpBase *)op, AlgebraicExpression_Source(ae), &op->srcNodeIdx));
	op->destNodeIdx = OpBase_Modifies((OpBase *)op, AlgebraicExpression_Destination(ae));

	const char *edge = AlgebraicExpression_Edge(ae);
	if(edge) {
		op->edges = array_new(Edge, 32);
		QGEdge *qg_edge = QueryGraph_GetEdgeByAlias(plan->query_graph, edge);
		_setupTraversedRelations(op, qg_edge);
		op->edgeRecIdx = OpBase_Modifies((OpBase *)op, edge);
		op->setEdge = true;
		// Determine the edge directions we need to collect.
		if(qg_edge->bidirectional) {
			op->direction = GRAPH_EDGE_DIR_BOTH;
		} else if(AlgebraicExpression_ContainsOp(ae, AL_EXP_TRANSPOSE)) {
			/* If this operation traverses a transposed edge, the source and destination nodes
			 * will be swapped in the Record. */
			op->direction = GRAPH_EDGE_DIR_INCOMING;
		}
	}

	return (OpBase *)op;
}

/* CondTraverseConsume next operation
 * each call will update the graph
 * returns OP_DEPLETED when no additional updates are available */
static Record CondTraverseConsume(OpBase *opBase) {
	CondTraverse *op = (CondTraverse *)opBase;
	OpBase *child = op->op.children[0];

	/* If we're required to update edge,
	 * try to get an edge, if successful we can return quickly,
	 * otherwise try to get a new pair of source and destination nodes. */
	if(op->setEdge) {
		if(_CondTraverse_SetEdge(op, op->r)) {
			return Record_Clone(op->r);
		}
	}

	bool depleted = true;
	NodeID src_id = INVALID_ENTITY_ID;
	NodeID dest_id = INVALID_ENTITY_ID;

	while(true) {
		if(op->iter) GxB_MatrixTupleIter_next(op->iter, &src_id, &dest_id, &depleted);

		// Managed to get a tuple, break.
		if(!depleted) break;

		/* Run out of tuples, try to get new data.
		 * Free old records. */
		op->r = NULL;
		for(int i = 0; i < op->recordsLen; i++) Record_Free(op->records[i]);

		// Ask child operations for data.
		for(op->recordsLen = 0; op->recordsLen < op->recordsCap; op->recordsLen++) {
			Record childRecord = OpBase_Consume(child);
			if(!childRecord) break;

			// Store received record.
			op->records[op->recordsLen] = childRecord;
		}

		// No data.
		if(op->recordsLen == 0) return NULL;

		_traverse(op);
	}

	/* Get node from current column. */
	op->r = op->records[src_id];
	Node *destNode = Record_GetNode(op->r, op->destNodeIdx);
	Graph_GetNode(op->graph, dest_id, destNode);

	if(op->setEdge) {
		_CondTraverse_CollectEdges(op, op->destNodeIdx, op->srcNodeIdx);
		// We're guaranteed to have at least one edge.
		_CondTraverse_SetEdge(op, op->r);
	}

	return Record_Clone(op->r);
}

static OpResult CondTraverseReset(OpBase *ctx) {
	CondTraverse *op = (CondTraverse *)ctx;
	if(op->r) Record_Free(op->r);
	if(op->edges) array_clear(op->edges);
	if(op->iter) {
		GxB_MatrixTupleIter_free(op->iter);
		op->iter = NULL;
	}
	if(op->F != GrB_NULL) GrB_Matrix_clear(op->F);
	return OP_OK;
}

/* Frees CondTraverse */
static void CondTraverseFree(OpBase *ctx) {
	CondTraverse *op = (CondTraverse *)ctx;
	if(op->iter) {
		GxB_MatrixTupleIter_free(op->iter);
		op->iter = NULL;
	}

	if(op->F != GrB_NULL) {
		GrB_Matrix_free(&op->F);
		op->F = GrB_NULL;
	}

	if(op->M != GrB_NULL) {
		GrB_Matrix_free(&op->M);
		op->M = GrB_NULL;
	}

	if(op->edges) {
		array_free(op->edges);
		op->edges = NULL;
	}

	if(op->ae) {
		AlgebraicExpression_Free(op->ae);
		op->ae = NULL;
	}

	if(op->edgeRelationTypes) {
		array_free(op->edgeRelationTypes);
		op->edgeRelationTypes = NULL;
	}

	if(op->records) {
		for(int i = 0; i < op->recordsLen; i++) Record_Free(op->records[i]);
		rm_free(op->records);
		op->records = NULL;
	}
}

