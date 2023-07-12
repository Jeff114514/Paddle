// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/framework/ir/identity_op_clean_pass.h"

#include "paddle/fluid/framework/ir/graph_pattern_detector.h"
#include "paddle/fluid/framework/op_version_registry.h"

namespace paddle {
namespace framework {
namespace ir {

namespace patterns {

// pre_op -> useless_op_in -> useless_op -> useless_op_out
// ->
// pre_op -> useless_op_out
struct FindUselessOpPattern : public PatternBase {
  FindUselessOpPattern(PDPattern* pattern, const std::string& name_scope);

  // declare operator node's name
  PATTERN_DECL_NODE(useless_op_in);
  PATTERN_DECL_NODE(useless_op);
  PATTERN_DECL_NODE(useless_op_out);
};

FindUselessOpPattern::FindUselessOpPattern(PDPattern* pattern,
                                           const std::string& name_scope)
    : PatternBase(pattern, name_scope, name_scope) {
  auto* useless_op_in = pattern->NewNode(useless_op_in_repr())
                            ->assert_is_var()
                            ->assert_var_not_persistable()
                            ->assert_has_n_outputs(1)
                            ->assert_more([](Node* x) {
                              for (auto* op : x->inputs) {
                                CHECK_EQ(op->IsOp(), true);
                                const auto& op_type = op->Op()->Type();
                                if (op_type == "conditional_block" ||
                                    op_type == "while" || op_type == "feed") {
                                  return false;
                                }
                              }
                              return true;
                            });

  // This useless_op must have only one input and one output!
  auto* useless_op =
      pattern->NewNode(useless_op_repr())
          ->assert_is_op()
          ->assert_has_n_inputs(1)
          ->assert_has_n_outputs(1)
          ->assert_more([](Node* x) {
            const auto& op_type = x->Op()->Type();
            if (op_type == "scale") {
              auto scale = x->Op()->GetAttrIfExists<float>("scale");
              auto bias = x->Op()->GetAttrIfExists<float>("bias");
              return bias == 0.f && scale == 1.f;
            } else if (op_type == "cast") {
              auto in_dtype = x->Op()->GetAttrIfExists<int>("in_dtype");
              auto out_dtype = x->Op()->GetAttrIfExists<int>("out_dtype");
              return in_dtype == out_dtype;
            } else if (op_type == "c_identity") {
              return true;
            } else if (op_type == "assign") {
              const auto& in_name = x->Op()->Input("X")[0];
              const auto& out_name = x->Op()->Output("Out")[0];
              return in_name == out_name;
            } else if (op_type == "concat") {
              return x->Op()->Input("X").size() == 1;
            }
            // you can add more cases here.
            return false;
          });

  auto* useless_op_out =
      pattern->NewNode(useless_op_out_repr())->assert_is_var();

  useless_op->LinksFrom({useless_op_in}).LinksTo({useless_op_out});
}

}  // namespace patterns

void IdentityOpCleanPass::ApplyImpl(ir::Graph* graph) const {
  Init(name_scope_, graph);

  GraphPatternDetector gpd;
  patterns::FindUselessOpPattern pattern(gpd.mutable_pattern(), name_scope_);

  int found_count = 0;
  GraphPatternDetector::handle_t handler =
      [&](const GraphPatternDetector::subgraph_t& subgraph, Graph* graph) {
        GET_IR_NODE_FROM_SUBGRAPH(useless_op_in, useless_op_in, pattern);
        GET_IR_NODE_FROM_SUBGRAPH(useless_op, useless_op, pattern);
        GET_IR_NODE_FROM_SUBGRAPH(useless_op_out, useless_op_out, pattern);
        CHECK_EQ(useless_op_in->IsVar(), true);
        CHECK_EQ(useless_op_out->IsVar(), true);
        CHECK_EQ(useless_op->IsOp(), true);

        for (auto* prev_op : useless_op_in->inputs) {
          CHECK_EQ(prev_op->IsOp(), true);
          prev_op->Op()->RenameOutput(useless_op_in->Var()->Name(),
                                      useless_op_out->Var()->Name());
          IR_NODE_LINK_TO(prev_op, useless_op_out);
        }

        GraphSafeRemoveNodes(graph, {useless_op_in, useless_op});
        found_count++;
      };

  gpd(graph, handler);
  AddStatis(found_count);
}

}  // namespace ir
}  // namespace framework
}  // namespace paddle

REGISTER_PASS(identity_op_clean_pass,
              paddle::framework::ir::IdentityOpCleanPass);
REGISTER_PASS_CAPABILITY(identity_op_clean_pass)
    .AddCombination(
        paddle::framework::compatible::OpVersionComparatorCombination()
            .EQ("scale", 0)
            .LE("c_identity", 1));
