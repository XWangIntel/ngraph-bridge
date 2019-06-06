/*******************************************************************************
 * Copyright 2019 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include "../test_utilities.h"
#include "gtest/gtest.h"
#include "ngraph_cluster_manager.h"
#include "ngraph_encapsulate_clusters.h"
#include "tensorflow/core/graph/node_builder.h"

using namespace std;
namespace ng = ngraph;

namespace tensorflow {

namespace ngraph_bridge {

namespace testing {

#define ASSERT_OK(x) ASSERT_EQ((x), ::tensorflow::Status::OK());
#define ASSERT_NOT_OK(x) ASSERT_NE((x), ::tensorflow::Status::OK());

TEST(EncapsulateClusters, PopulateLibrary) {
  NGraphClusterManager::EvictAllClusters();
  Graph g(OpRegistry::Global());

  Tensor t_input_0(DT_FLOAT, TensorShape{2, 3});
  Tensor t_input_1(DT_INT32, TensorShape{2});
  t_input_1.flat<int32>().data()[0] = 3;
  t_input_1.flat<int32>().data()[1] = 2;

  int cluster_idx = NGraphClusterManager::NewCluster();

  Node* node1;
  ASSERT_OK(NodeBuilder("node1", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_input_0)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node1));

  Node* node2;
  ASSERT_OK(NodeBuilder("node2", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_input_1)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node2));

  Node* node3;
  ASSERT_OK(NodeBuilder("node3", "Add")
                .Input(node1)
                .Input(node2)
                .Attr("T", DT_FLOAT)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node3));

  Node* source = g.source_node();
  Node* sink = g.sink_node();
  g.AddEdge(source, Graph::kControlSlot, node1, Graph::kControlSlot);
  g.AddEdge(source, Graph::kControlSlot, node2, Graph::kControlSlot);
  g.AddEdge(node3, Graph::kControlSlot, sink, Graph::kControlSlot);

  FunctionDefLibrary* fdeflib_new = new FunctionDefLibrary();
  ASSERT_OK(EncapsulateClusters(&g, 0, fdeflib_new, nullptr));

  int num_encapsulates = 0;
  int num_tf_nodes = 0;
  for (auto itr : g.nodes()) {
    auto node_type = itr->type_string();
    num_encapsulates += (node_type == "NGraphEncapsulate" ? 1 : 0);
    num_tf_nodes += ((node_type == "Add" || node_type == "Const") ? 1 : 0);
  }

  // Number of encapsulates == number of functions
  ASSERT_EQ(num_encapsulates, fdeflib_new->function_size());

  // No Add or Const nodes left in the graph
  ASSERT_EQ(num_tf_nodes, 0);

  // In this case, only 1 function has been added in the library
  ASSERT_EQ(fdeflib_new->function_size(), 1);

  // Check the name of the signature of the first (and only) function
  auto first_func = fdeflib_new->function(0);
  ASSERT_EQ(first_func.signature().name(),
            ("ngraph_cluster_" + to_string(cluster_idx)));

  // The first function in the flib should have 3 nodes
  ASSERT_EQ(first_func.node_def_size(), 3);

  // Ensure that the function is made of 2 op types, Add, Const, Const
  auto present = multiset<string>{string(first_func.node_def(0).op()),
                                  string(first_func.node_def(1).op()),
                                  string(first_func.node_def(2).op())};
  auto expected = multiset<string>{"Const", "Add", "Const"};
  ASSERT_EQ(present, expected);
  free(fdeflib_new);
}

TEST(EncapsulateClusters, CollectSharedTensorsTest0) {
  // E0-->E1.
  // Simplest test: Output of E0 is sharable with input of E1
  // In this test E0 has 1 const, and E1 has another const and an add

  NGraphClusterManager::EvictAllClusters();
  Graph g(OpRegistry::Global());

  Tensor t_input(DT_FLOAT, TensorShape{2, 3});
  Tensor t_shape(DT_INT32, TensorShape{2});
  t_shape.flat<int32>().data()[0] = 3;
  t_shape.flat<int32>().data()[1] = 2;

  int cluster_idx_0 = NGraphClusterManager::NewCluster();

  Node* node1;
  ASSERT_OK(NodeBuilder("node1", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_input)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_0)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node1));

  int cluster_idx_1 = NGraphClusterManager::NewCluster();

  Node* node2;
  ASSERT_OK(NodeBuilder("node2", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_shape)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_1)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node2));

  Node* node3;
  ASSERT_OK(NodeBuilder("node3", "Add")
                .Input(node1)
                .Input(node2)
                .Attr("T", DT_FLOAT)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_1)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node3));

  Node* source = g.source_node();
  Node* sink = g.sink_node();
  g.AddEdge(source, Graph::kControlSlot, node1, Graph::kControlSlot);
  g.AddEdge(source, Graph::kControlSlot, node2, Graph::kControlSlot);
  g.AddEdge(node3, Graph::kControlSlot, sink, Graph::kControlSlot);

  FunctionDefLibrary* fdeflib_new = new FunctionDefLibrary();
  std::vector<std::set<UniqueTensorId>> shared_tensors;
  ASSERT_OK(EncapsulateClusters(&g, 0, fdeflib_new, &shared_tensors));

  // There is only 1 group
  ASSERT_EQ(shared_tensors.size(), 1);
  // That group contains 2 shareable tensors
  ASSERT_EQ(shared_tensors[0].size(), 2);

  // Input slot 0 of encapsulate 1 is shared with output slot 0 of encapsulate 0
  ASSERT_EQ(shared_tensors[0], (std::set<string>{"1_0_0", "0_1_0"}));
}

TEST(EncapsulateClusters, CollectSharedTensorsTest1) {
  // E2-->E0<--E1.
  // Input of E0 is sharable with outputs of E1 and E2
  // In this test E0 has 1 add, and E1/E2 has consts

  NGraphClusterManager::EvictAllClusters();
  Graph g(OpRegistry::Global());

  Tensor t_input(DT_FLOAT, TensorShape{2, 3});
  Tensor t_shape(DT_INT32, TensorShape{2});
  t_shape.flat<int32>().data()[0] = 3;
  t_shape.flat<int32>().data()[1] = 2;

  int cluster_idx_0 = NGraphClusterManager::NewCluster();

  Node* node1;
  ASSERT_OK(NodeBuilder("node1", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_input)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_0)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node1));

  int cluster_idx_1 = NGraphClusterManager::NewCluster();

  Node* node2;
  ASSERT_OK(NodeBuilder("node2", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_shape)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_1)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node2));

  int cluster_idx_2 = NGraphClusterManager::NewCluster();

  Node* node3;
  ASSERT_OK(NodeBuilder("node3", "Add")
                .Input(node1)
                .Input(node2)
                .Attr("T", DT_FLOAT)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_2)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node3));

  Node* source = g.source_node();
  Node* sink = g.sink_node();
  g.AddEdge(source, Graph::kControlSlot, node1, Graph::kControlSlot);
  g.AddEdge(source, Graph::kControlSlot, node2, Graph::kControlSlot);
  g.AddEdge(node3, Graph::kControlSlot, sink, Graph::kControlSlot);

  FunctionDefLibrary* fdeflib_new = new FunctionDefLibrary();
  std::vector<std::set<UniqueTensorId>> shared_tensors;
  ASSERT_OK(EncapsulateClusters(&g, 0, fdeflib_new, &shared_tensors));

  // There are 2 groups
  ASSERT_EQ(shared_tensors.size(), 2);
  // Each group contains 2 shareable tensors
  ASSERT_EQ(shared_tensors[0].size(), 2);
  ASSERT_EQ(shared_tensors[1].size(), 2);

  // Input slot 0 of encapsulate 2 is shared with output slot 0 of encapsulate 0
  ASSERT_EQ(shared_tensors[0], (std::set<string>{"2_0_0", "0_1_0"}));

  // Input slot 1 of encapsulate 2 is shared with output slot 0 of encapsulate 1
  ASSERT_EQ(shared_tensors[1], (std::set<string>{"2_0_1", "1_1_0"}));
}

TEST(EncapsulateClusters, CollectSharedTensorsTest2) {
  // E0<--TF-->E1.
  //      |
  //      v
  //      E2
  // A TF node feeds to inputs of E0, E1 and E2
  // In this test E0, E1 and E2 are Abs, and the TF node is a const (which is
  // disabled for ngraph for this test)

  NGraphClusterManager::EvictAllClusters();
  Graph g(OpRegistry::Global());

  Tensor t_input(DT_FLOAT, TensorShape{2, 3});

  Node* node1;
  ASSERT_OK(NodeBuilder("node1", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_input)
                .Finalize(&g, &node1));

  int cluster_idx_0 = NGraphClusterManager::NewCluster();
  Node* node2;
  ASSERT_OK(NodeBuilder("node2", "Abs")
                .Input(node1)
                .Attr("T", DT_FLOAT)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_0)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node2));

  int cluster_idx_1 = NGraphClusterManager::NewCluster();
  Node* node3;
  ASSERT_OK(NodeBuilder("node3", "Abs")
                .Input(node1)
                .Attr("T", DT_FLOAT)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_1)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node3));

  int cluster_idx_2 = NGraphClusterManager::NewCluster();
  Node* node4;
  ASSERT_OK(NodeBuilder("node3", "Abs")
                .Input(node1)
                .Attr("T", DT_FLOAT)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx_2)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node4));

  Node* source = g.source_node();
  Node* sink = g.sink_node();
  g.AddEdge(source, Graph::kControlSlot, node1, Graph::kControlSlot);
  g.AddEdge(node2, Graph::kControlSlot, sink, Graph::kControlSlot);
  g.AddEdge(node3, Graph::kControlSlot, sink, Graph::kControlSlot);
  g.AddEdge(node4, Graph::kControlSlot, sink, Graph::kControlSlot);

  FunctionDefLibrary* fdeflib_new = new FunctionDefLibrary();
  std::vector<std::set<UniqueTensorId>> shared_tensors;
  ASSERT_OK(EncapsulateClusters(&g, 0, fdeflib_new, &shared_tensors));

  // There is only 1 group
  ASSERT_EQ(shared_tensors.size(), 1);
  // That group contains 2 shareable tensors
  ASSERT_EQ(shared_tensors[0].size(), 3);

  // Input slot 0 of encapsulate 0, 1 and 2 are shareable
  ASSERT_EQ(shared_tensors[0], (std::set<string>{"0_0_0", "1_0_0", "2_0_0"}));
}
}
}
}