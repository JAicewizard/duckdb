#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_window.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/set/physical_union.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"

namespace duckdb {

static vector<unique_ptr<Expression>> CreatePartitionedRowNumExpression(const vector<LogicalType> &types) {
	vector<unique_ptr<Expression>> res;
	auto expr =
	    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
	expr->start = WindowBoundary::UNBOUNDED_PRECEDING;
	expr->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	for (idx_t i = 0; i < types.size(); i++) {
		expr->partitions.push_back(make_uniq<BoundReferenceExpression>(types[i], i));
	}
	res.push_back(std::move(expr));
	return res;
}

static JoinCondition CreateNotDistinctComparison(const LogicalType &type, idx_t i) {
	JoinCondition cond;
	cond.left = make_uniq<BoundReferenceExpression>(type, i);
	cond.right = make_uniq<BoundReferenceExpression>(type, i);
	cond.comparison = ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	return cond;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalSetOperation &op) {
	D_ASSERT(op.children.size() == 2);

	reference<PhysicalOperator> left = CreatePlan(*op.children[0]);
	reference<PhysicalOperator> right = CreatePlan(*op.children[1]);

	if (left.get().GetTypes() != right.get().GetTypes()) {
		throw InvalidInputException("Type mismatch for SET OPERATION");
	}

	optional_ptr<PhysicalOperator> result;
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_UNION:
		// UNION
		result = Make<PhysicalUnion>(op.types, left, right, op.estimated_cardinality, op.allow_out_of_order);
		break;
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT: {
		auto &types = left.get().GetTypes();
		vector<JoinCondition> conditions;
		// create equality condition for all columns
		for (idx_t i = 0; i < types.size(); i++) {
			conditions.push_back(CreateNotDistinctComparison(types[i], i));
		}
		// For EXCEPT ALL / INTERSECT ALL we push a window operator with a ROW_NUMBER into the scans and join to get bag
		// semantics.
		if (op.setop_all) {
			vector<LogicalType> window_types = types;
			window_types.push_back(LogicalType::BIGINT);

			auto select_list = CreatePartitionedRowNumExpression(types);
			auto &left_window =
			    Make<PhysicalWindow>(window_types, std::move(select_list), left.get().estimated_cardinality);
			left_window.children.push_back(left);
			left = left_window;

			select_list = CreatePartitionedRowNumExpression(types);
			auto &right_window =
			    Make<PhysicalWindow>(window_types, std::move(select_list), right.get().estimated_cardinality);
			right_window.children.push_back(right);
			right = right_window;

			// add window expression result to join condition
			conditions.push_back(CreateNotDistinctComparison(LogicalType::BIGINT, types.size()));
			// join (created below) now includes the row number result column
			op.types.push_back(LogicalType::BIGINT);
		}

		// EXCEPT is ANTI join
		// INTERSECT is SEMI join

		JoinType join_type = op.type == LogicalOperatorType::LOGICAL_EXCEPT ? JoinType::ANTI : JoinType::SEMI;
		result = Make<PhysicalHashJoin>(op, left, right, std::move(conditions), join_type, op.estimated_cardinality);

		// For EXCEPT ALL / INTERSECT ALL we need to remove the row number column again
		if (op.setop_all) {
			vector<unique_ptr<Expression>> select_list;
			for (idx_t i = 0; i < types.size(); i++) {
				select_list.push_back(make_uniq<BoundReferenceExpression>(types[i], i));
			}
			auto &proj = Make<PhysicalProjection>(types, std::move(select_list), op.estimated_cardinality);
			proj.children.push_back(*result);
			result = proj;
		}
		break;
	}
	default:
		throw InternalException("Unexpected operator type for set operation");
	}

	// if the ALL specifier is not given, we have to ensure distinct results. Hence, push a GROUP BY ALL
	if (!op.setop_all) { // no ALL, use distinct semantics
		auto &types = result->GetTypes();
		vector<unique_ptr<Expression>> groups, aggregates /* left empty */;
		for (idx_t i = 0; i < types.size(); i++) {
			groups.push_back(make_uniq<BoundReferenceExpression>(types[i], i));
		}
		auto &group_by = Make<PhysicalHashAggregate>(context, op.types, std::move(aggregates), std::move(groups),
		                                             result->estimated_cardinality);
		group_by.children.push_back(*result);
		result = group_by;
	}

	D_ASSERT(result);
	return *result;
}

} // namespace duckdb
