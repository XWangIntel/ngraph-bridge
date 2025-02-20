/*******************************************************************************
 * Copyright 2017-2019 Intel Corporation
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
#include <cstdlib>
#include <mutex>
#include <utility>

#include "tensorflow/core/common_runtime/dma_helper.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/optimization_registry.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/graph_constructor.h"

#include "ngraph/event_tracing.hpp"
#include "ngraph/runtime/backend.hpp"

#if defined NGRAPH_DISTRIBUTED
#include "ngraph/distributed.hpp"
#endif

#include "logging/ngraph_log.h"
#include "ngraph_bridge/ngraph_backend_manager.h"
#include "ngraph_bridge/ngraph_builder.h"
#include "ngraph_bridge/ngraph_cluster_manager.h"
#include "ngraph_bridge/ngraph_freshness_tracker.h"
#include "ngraph_bridge/ngraph_mark_for_clustering.h"
#include "ngraph_bridge/ngraph_timer.h"
#include "ngraph_bridge/ngraph_utils.h"

#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
#include "ngraph_bridge/enable_variable_ops/ngraph_catalog.h"
#include "ngraph_bridge/enable_variable_ops/ngraph_var.h"
#endif

using namespace std;
namespace ng = ngraph;

namespace tensorflow {

// For each I/O tensor, cache TF's data ptr and nGraph's Tensor
using NgFunctionIOCache = std::unordered_map<
    std::shared_ptr<ngraph::runtime::Executable>,
    std::vector<std::pair<void*, shared_ptr<ng::runtime::Tensor>>>>;

namespace ngraph_bridge {

class NGraphEncapsulateOp : public OpKernel {
 public:
  //---------------------------------------------------------------------------
  //  NGraphEncapsulateOp::ctor
  //---------------------------------------------------------------------------
  explicit NGraphEncapsulateOp(OpKernelConstruction* ctx)
      : OpKernel(ctx),
        m_graph(OpRegistry::Global()),
        m_freshness_tracker(nullptr) {
    my_instance_id = s_instance_count;
    s_instance_count++;

    std::ostringstream oss;
    oss << "Encapsulate_" << my_instance_id << ": " << name();
    ngraph::Event event(oss.str(), name(), "");

    NGRAPH_VLOG(1) << "NGraphEncapsulateOp: " << my_instance_id
                   << " Name: " << name();

    GraphDef* graph_def;

    OP_REQUIRES_OK(ctx, ctx->GetAttr<int>("ngraph_cluster", &m_ngraph_cluster));
    graph_def = NGraphClusterManager::GetClusterGraph(m_ngraph_cluster);

    if (graph_def == nullptr) {
      string flib_key = "ngraph_cluster_" + to_string(m_ngraph_cluster);
      // Read graphdef from function library
      const FunctionLibraryDefinition flib =
          *ctx->function_library()->GetFunctionLibraryDefinition();
      const FunctionDef* fdef = flib.Find(flib_key);
      OP_REQUIRES(
          ctx, fdef != nullptr,
          errors::Internal("Did not find graphdef for encapsulate ", flib_key,
                           " in NGraphClusterManager or function library"));
      // TODO: how to convert from functiondef to graphdef. Anything easier?
      std::unique_ptr<FunctionBody> fnbody;
      const auto get_func_sig = [&flib](const string& op, const OpDef** sig) {
        return flib.LookUpOpDef(op, sig);
      };
      OP_REQUIRES_OK(ctx, FunctionDefToBodyHelper(*fdef, {}, &flib,
                                                  get_func_sig, &fnbody));
      CopyGraph(*fnbody->graph, &m_graph);
    } else {
      GraphConstructorOptions opts;
      opts.allow_internal_ops = true;
      OP_REQUIRES_OK(ctx, ConvertGraphDefToGraph(opts, *graph_def, &m_graph));
    }
    OP_REQUIRES_OK(ctx, ctx->GetAttr("ngraph_graph_id", &m_graph_id));
    //
    // Initialize the "m_input_is_static" vector as follows:
    // (1) create m_input_is_static with n+1 elements, where n is the max arg
    //     index
    // (2) for each _Arg node n, set m_input_is_static[n.index] to true if n
    //     is driving any static input; else set it to false.
    //

    // Create the vector.
    int32 max_arg_index = -1;
    std::vector<const Node*> arg_nodes;

    for (auto node : m_graph.nodes()) {
      if (node->type_string() == "_Arg") {
        arg_nodes.push_back(node);

        int32 index;
        OP_REQUIRES_OK(ctx, GetNodeAttr(node->attrs(), "index", &index));
        if (index > max_arg_index) max_arg_index = index;
      }
    }

    m_input_is_static = std::vector<bool>(max_arg_index + 1, false);

    // Fill the vector.
    for (auto node : arg_nodes) {
      int32 index;
      OP_REQUIRES_OK(ctx, GetNodeAttr(node->attrs(), "index", &index));

      bool is_static = false;
      for (auto edge : node->out_edges()) {
        if (edge->IsControlEdge() || !edge->dst()->IsOp()) {
          continue;
        }

        NGRAPH_VLOG(5) << "For arg " << index << " checking edge "
                       << edge->DebugString();

        if (InputIsStatic(edge->dst(), edge->dst_input())) {
          NGRAPH_VLOG(5) << "Marking edge static: " << edge->DebugString();
          is_static = true;
          break;
        }
      }

      NGRAPH_VLOG(5) << "Marking arg " << index << " is_static: " << is_static;
      m_input_is_static[index] = is_static;
    }

    // Set the backend type for the op
    std::string backend_name;
    OP_REQUIRES_OK(ctx, ctx->GetAttr<string>("ngraph_backend", &backend_name));
    std::string device_id;
    OP_REQUIRES_OK(ctx, ctx->GetAttr<string>("ngraph_device_id", &device_id));
    // Get the optional attributes
    std::unordered_map<std::string, std::string> additional_attribute_map;
    auto node_def = ctx->def();
    auto additional_attributes = node_def.attr();
    for (auto itx : additional_attributes) {
      // Find the optional attributes to be sent to the backend.
      // The optional attributes have '_ngraph_' appended to the start
      // so we need to get rid of that and only send the remaining string
      // since the backend will only look for that.
      // '_ngraph_' is only appended for the bridge.
      // For e.g. _ngraph_ice_cores --> ice_cores
      if (itx.first.find("_ngraph_") != std::string::npos) {
        NGRAPH_VLOG(4) << "Attribute: " << itx.first.substr(strlen("_ngraph_"))
                       << " Value: " << itx.second.s();
        additional_attribute_map.insert(
            {itx.first.substr(strlen("_ngraph_")), itx.second.s()});
      }
    }

    // Concatenate the backend_name:device_id
    try {
      m_op_backend_name =
          BackendManager::GetBackendCreationString(backend_name, device_id);
    } catch (const std::exception& exp) {
      Status status = errors::Internal(
          "Caught exception while creating backend string ", exp.what(), "\n");
      OP_REQUIRES_OK(ctx, status);
    }
    NGRAPH_VLOG(4) << "NGraphEncapsulateOp::Create backend " << def().name();
    BackendManager::CreateBackend(m_op_backend_name);
    // SetConfig will be called for each EncapsulateOp
    BackendManager::SetConfig(m_op_backend_name, additional_attribute_map);
    event.Stop();
    ngraph::Event::write_trace(event);
  }

  //---------------------------------------------------------------------------
  //  ~NGraphEncapsulateOp()
  //---------------------------------------------------------------------------
  ~NGraphEncapsulateOp() override {
    std::ostringstream oss;
    oss << "Destroy Encapsulate_" << my_instance_id << ": " << name();
    ngraph::Event event(oss.str(), name(), "");
    NGRAPH_VLOG(2) << "~NGraphEncapsulateOp::" << name();

    // If the kernel goes away, we must de-register all of its cached
    // functions
    // from the freshness tracker.
    if (m_freshness_tracker != nullptr) {
      for (auto kv : m_ng_exec_map) {
        m_freshness_tracker->RemoveUser(kv.second);
      }

      // TODO(amprocte): We should be able to unref the tracker here, but it
      // seems to screw things up in the C++ unit tests.
      // m_freshness_tracker->Unref();
    }

#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
    // Remove Entries from Catalog
    // Remove entries related to outputs
    for (int i = 0; i < m_number_outputs; i++) {
      string key = NGraphCatalog::CreateNodeKey(m_graph_id, name(), i);
      if (NGraphCatalog::ExistsInEncapOutputInfoMap(key)) {
        NGraphCatalog::DeleteFromEncapOutputInfoMap(key);
        NGRAPH_VLOG(2) << "Deleting from output info map " << key;
      }
    }

    NGRAPH_VLOG(2) << "Deleting from Output Copy Index map " << name();
    NGraphCatalog::DeleteFromEncapOutputCopyIndexesMap(m_graph_id, name());

    // Remove entries related to inputs
    for (int i = 0; i < m_number_inputs; i++) {
      string key = NGraphCatalog::CreateNodeKey(m_graph_id, name(), i);
      if (NGraphCatalog::ExistsInInputVariableSharedNameMap(key)) {
        NGraphCatalog::DeleteFromInputVariableSharedNameMap(key);
        NGRAPH_VLOG(2) << "Deleting from input variable shared name map "
                       << key;
      }
    }

#endif
    m_ng_exec_input_cache_map.clear();
    m_ng_exec_output_cache_map.clear();
    m_ng_exec_map.clear();
    m_ng_function_map.clear();

    // Release the backend
    NGRAPH_VLOG(2) << "~NGraphEncapsulateOp():: ReleaseBackend";
    BackendManager::ReleaseBackend(m_op_backend_name);
    event.Stop();
    ngraph::Event::write_trace(event);
  }

  template <typename T>
  static void TensorDataToStream(std::ostream& ostream, int64 n_elements,
                                 const char* data) {
    const T* data_T = reinterpret_cast<const T*>(data);
    for (int i = 0; i < n_elements; i++) {
      ostream << data_T[i] << ",";
    }
  }

  //---------------------------------------------------------------------------
  //  TensorToStream
  //---------------------------------------------------------------------------
  static Status TensorToStream(std::ostream& ostream, const Tensor& tensor) {
    const char* data = tensor.tensor_data().data();
    int64 n_elements = tensor.NumElements();
    switch (tensor.dtype()) {
      case DT_HALF:
        TensorDataToStream<Eigen::half>(ostream, n_elements, data);
        break;
      case DT_FLOAT:
        TensorDataToStream<float>(ostream, n_elements, data);
        break;
      case DT_DOUBLE:
        TensorDataToStream<double>(ostream, n_elements, data);
        break;
      case DT_UINT32:
        TensorDataToStream<uint32>(ostream, n_elements, data);
        break;
      case DT_INT32:
        TensorDataToStream<int32>(ostream, n_elements, data);
        break;
      case DT_UINT8:
      case DT_QUINT8:
        TensorDataToStream<uint8>(ostream, n_elements, data);
        break;
      case DT_UINT16:
      case DT_QUINT16:
        TensorDataToStream<uint16>(ostream, n_elements, data);
        break;
      case DT_INT8:
      case DT_QINT8:
        TensorDataToStream<int8>(ostream, n_elements, data);
        break;
      case DT_INT16:
      case DT_QINT16:
        TensorDataToStream<int16>(ostream, n_elements, data);
        break;
      case DT_UINT64:
        TensorDataToStream<uint64>(ostream, n_elements, data);
        break;
      case DT_INT64:
        TensorDataToStream<int64>(ostream, n_elements, data);
        break;
      case DT_BOOL:
        TensorDataToStream<bool>(ostream, n_elements, data);
        break;
      default:
        return errors::Internal("TensorToStream got unsupported data type ",
                                DataType_Name(tensor.dtype()));
        break;
    }
    return Status::OK();
  }

  Status ComputeSignature(OpKernelContext* ctx, std::stringstream& signature_ss,
                          std::vector<TensorShape>& input_shapes,
                          std::vector<const Tensor*>& static_input_map) {
    // Get the inputs
    for (int i = 0; i < ctx->num_inputs(); i++) {
      const Tensor& input_tensor = ctx->input(i);
      input_shapes.push_back(input_tensor.shape());
      for (const auto& x : input_tensor.shape()) {
        signature_ss << x.size << ",";
      }
      signature_ss << ";";
    }

    signature_ss << "/";

    static_input_map.resize(ctx->num_inputs());
    for (int i = 0; i < ctx->num_inputs(); i++) {
      const Tensor& input_tensor = ctx->input(i);
      if (m_input_is_static[i]) {
        static_input_map[i] = &input_tensor;
        TF_RETURN_IF_ERROR(TensorToStream(signature_ss, input_tensor));
        signature_ss << ";";
      }
    }
    return Status::OK();
  }

  Status GetNgExec(OpKernelContext* ctx,
                   std::shared_ptr<ngraph::runtime::Executable>& ng_exec,
                   std::vector<TensorShape>& input_shapes,
                   std::vector<const Tensor*>& static_input_map,
                   ng::runtime::Backend*& op_backend) {
    std::stringstream signature_ss;
    string signature;

    std::shared_ptr<ngraph::Function> ng_function;
    std::shared_ptr<ngraph::runtime::Executable> evicted_ng_exec;

    NGRAPH_VLOG(4) << "GetNgExec: Got backend of type: " << m_op_backend_name;
    op_backend = BackendManager::GetBackend(m_op_backend_name);

    // Compute Signature
    TF_RETURN_IF_ERROR(
        ComputeSignature(ctx, signature_ss, input_shapes, static_input_map));
    signature = signature_ss.str();

    if (NGRAPH_VLOG_IS_ON(5)) {
      NGRAPH_VLOG(5) << "Computed signature: " << signature;
    }

    auto it = m_ng_exec_map.find(signature);

    NGRAPH_VLOG(4) << "NGraphEncapsulateOp::Compute got inputs for cluster "
                   << m_ngraph_cluster;

    // Translate the TensorFlow graph to nGraph.
    if (it == m_ng_exec_map.end()) {
      // Measure the current total memory usage
      long vm, rss, vm0, rss0;
      MemoryProfile(vm0, rss0);

      NGRAPH_VLOG(1) << "Compilation cache miss: " << ctx->op_kernel().name();
      TF_RETURN_IF_ERROR(Builder::TranslateGraph(input_shapes, static_input_map,
                                                 &m_graph, ng_function));
      ng_function->set_friendly_name(name());

      auto function_size = ng_function->get_graph_size() / 1024;  // kb unit

      // Serialize to nGraph if needed
      if (std::getenv("NGRAPH_ENABLE_SERIALIZE") != nullptr) {
        std::string file_name =
            "tf_function_" + ctx->op_kernel().name() + ".json";
        NgraphSerialize("tf_function_" + ctx->op_kernel().name() + ".json",
                        ng_function);
#if defined NGRAPH_DISTRIBUTED
        int rank_id;
        rank_id = ng::get_distributed_interface()->get_rank();
        NgraphSerialize("tf_function_" + ctx->op_kernel().name() + "_" +
                            to_string(rank_id) + ".json",
                        ng_function);
#endif
      }
      // Evict the cache if the number of elements exceeds the limit
      const char* cache_depth_specified =
          std::getenv("NGRAPH_TF_FUNCTION_CACHE_ITEM_DEPTH");
      if (cache_depth_specified != nullptr) {
        my_function_cache_depth_in_items = atoi(cache_depth_specified);
      }

      if (m_ng_exec_map.size() >= my_function_cache_depth_in_items) {
        int input_tensors_bytes_free = 0;
        evicted_ng_exec = m_ng_exec_map[m_lru.back()];
        m_ng_exec_map.erase(m_lru.back());
        m_ng_function_map.erase(evicted_ng_exec);

        // Call delete function here pf he erased func
        op_backend->remove_compiled_function(evicted_ng_exec);

        // Now clean the input cache
        std::vector<std::pair<void*, std::shared_ptr<ng::runtime::Tensor>>>&
            input_caches = m_ng_exec_input_cache_map[evicted_ng_exec];
        for (auto& next_input : input_caches) {
          input_tensors_bytes_free += next_input.second->get_size_in_bytes();
          next_input.second.reset();
        }
        m_ng_exec_input_cache_map.erase(evicted_ng_exec);

        // Clean the output cache
        std::vector<std::pair<void*, std::shared_ptr<ng::runtime::Tensor>>>&
            output_caches = m_ng_exec_output_cache_map[evicted_ng_exec];
        int output_tensors_bytes_free = 0;
        for (auto& next_output : output_caches) {
          output_tensors_bytes_free += next_output.second->get_size_in_bytes();
          next_output.second.reset();
        }
        m_ng_exec_output_cache_map.erase(evicted_ng_exec);
        m_lru.pop_back();
        NGRAPH_VLOG(1) << "NGRAPH_TF_MEM_PROFILE:  OP_ID: " << my_instance_id
                       << " Step_ID: " << ctx->step_id()
                       << " Cluster: " << ctx->op_kernel().name()
                       << " Input Tensors freed: "
                       << input_tensors_bytes_free / (1024 * 1024) << " MB"
                       << " Output Tensors freed: "
                       << output_tensors_bytes_free / (1024 * 1024) << " MB";
      }  // cache eviction if cache size greater than cache depth

      BackendManager::LockBackend(m_op_backend_name);

      ngraph::Event event_compile("Compile nGraph", name(), "");
      try {
        ng_exec = op_backend->compile(ng_function);

      } catch (const std::exception& exp) {
        BackendManager::UnlockBackend(m_op_backend_name);
        NgraphSerialize(
            "tf_function_error_" + ctx->op_kernel().name() + ".json",
            ng_function);
        return errors::Internal("Caught exception while compiling op_backend: ",
                                exp.what(), "\n");
      } catch (...) {
        BackendManager::UnlockBackend(m_op_backend_name);
        NgraphSerialize(
            "tf_function_error_" + ctx->op_kernel().name() + ".json",
            ng_function);
        return errors::Internal("Error in compiling op_backend\n");
      }
      BackendManager::UnlockBackend(m_op_backend_name);
      event_compile.Stop();
      ngraph::Event::write_trace(event_compile);

      m_ng_exec_map[signature] = ng_exec;
      // caching ng_function to serialize to ngraph if needed
      m_ng_function_map[ng_exec] = ng_function;

      m_lru.push_front(signature);
      // Memory after
      MemoryProfile(vm, rss);
      auto delta_vm_mem = vm - vm0;
      auto delta_res_mem = rss - rss0;
      NGRAPH_VLOG(1) << "NGRAPH_TF_CACHE_PROFILE: OP_ID: " << my_instance_id
                     << " Step_ID: " << ctx->step_id()
                     << " Cache length: " << m_ng_exec_map.size()
                     << "  Cluster: " << ctx->op_kernel().name()
                     << " Delta VM: " << delta_vm_mem
                     << "  Delta RSS: " << delta_res_mem
                     << "  Function size: " << function_size
                     << " KB Total RSS: " << rss / (1024 * 1024) << " GB "
                     << " VM: " << vm / (1024 * 1024) << " GB" << endl;
    }  // end of input signature not found in m_ng_exec_map
    else {
      // Found the input signature in m_ng_exec_map, use the cached executable
      // Update the m_lru
      if (signature != m_lru.front()) {
        m_lru.remove(signature);
        m_lru.push_front(signature);
      }
      ng_exec = it->second;
    }
    return Status::OK();
  }

  //---------------------------------------------------------------------------
  // OpKernel::Compute
  //---------------------------------------------------------------------------
  void Compute(OpKernelContext* ctx) override {
    std::ostringstream oss;
    oss << "Execute: Encapsulate_" << my_instance_id << ": " << name();
    ngraph::Event event(oss.str(), name(), "");

    Timer compute_time;
    std::lock_guard<std::mutex> lock(m_compute_lock);
    NGRAPH_VLOG(4) << "NGraphEncapsulateOp::Compute starting for cluster "
                   << m_ngraph_cluster;

    ngraph::Event event_func_maybe_create("FunctionMaybeCreate", name(), "");
    Timer function_lookup_or_create;

    std::vector<TensorShape> input_shapes;
    std::vector<const Tensor*> static_input_map;
    std::shared_ptr<ngraph::Function> ng_function;
    std::shared_ptr<ngraph::runtime::Executable> ng_exec;
    ng::runtime::Backend* op_backend;

    // Get ngraph executable and inputs information
    OP_REQUIRES_OK(ctx, GetNgExec(ctx, ng_exec, input_shapes, static_input_map,
                                  op_backend));

    NGRAPH_VLOG(4)
        << "NGraphEncapsulateOp::Compute got ngraph executable for cluster "
        << m_ngraph_cluster;

    int time_func_create_or_lookup = function_lookup_or_create.ElapsedInMS();
    event_func_maybe_create.Stop();

    NGRAPH_VLOG(4) << "NGraphEncapsulateOp::Compute got graph for cluster "
                   << m_ngraph_cluster;

    Timer create_or_lookup_tensors;

    if (m_freshness_tracker == nullptr) {
      auto creator = [](NGraphFreshnessTracker** tracker) {
        *tracker = new NGraphFreshnessTracker();
        return Status::OK();
      };
      OP_REQUIRES_OK(
          ctx, ctx->resource_manager()->LookupOrCreate<NGraphFreshnessTracker>(
                   ctx->resource_manager()->default_container(),
                   "ngraph_freshness_tracker", &m_freshness_tracker, creator));
    }

    NGRAPH_VLOG(4)
        << "NGraphEncapsulateOp::Compute got freshness tracker for cluster "
        << m_ngraph_cluster;

    // Allocate tensors for input arguments.
    ngraph::Event event_alloc_input("Input: maybe create", name(), "");
    vector<shared_ptr<ng::runtime::Tensor>> ng_inputs;
    int ng_input_tensor_size_in_bytes = 0;

    std::vector<std::pair<void*, std::shared_ptr<ng::runtime::Tensor>>>&
        input_caches = m_ng_exec_input_cache_map[ng_exec];
    input_caches.resize(input_shapes.size());

    std::vector<std::unique_ptr<ngraph::Event>> input_copy_events;
#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
    bool log_copies = false;
    OP_REQUIRES_OK(ctx,
                   IsNgraphTFLogTensorCopiesEnabled(m_graph_id, log_copies));
    std::stringstream copy_log_str;
    copy_log_str << "KERNEL[" << type_string() << "]: " << name()
                 << " ,GraphID " << m_graph_id << "\n";
    int number_of_copies = 0;
#endif

    for (size_t i = 0; i < input_shapes.size(); i++) {
#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
      bool ref_exists = NGraphCatalog::ExistsInInputVariableSharedNameMap(
          m_graph_id, def().name(), i);

      // If the input is from a Variable node, we are dealing with later
      // just add a nullptr to the ng_inputs vector.
      if (ref_exists) {
        NGRAPH_VLOG(4) << "NGraphEncapsulateOp:: Input from Variable Node";
        ng_inputs.push_back(nullptr);
        continue;
      }
      NGRAPH_VLOG(4) << "NGraphEncapsulateOp:: Input from non Variable Node";
#endif
      ng::Shape ng_shape(input_shapes[i].dims());
      for (int j = 0; j < input_shapes[i].dims(); ++j) {
        ng_shape[j] = input_shapes[i].dim_size(j);
      }
      ng::element::Type ng_element_type;
      OP_REQUIRES_OK(ctx, TFDataTypeToNGraphElementType(ctx->input(i).dtype(),
                                                        &ng_element_type));

      // At the first call of the ng_exec, both last_src_ptr and
      // last_ng_tensor shall point to null. Otherwise, they are retrived
      // from cache.
      void* last_src_ptr = input_caches[i].first;
      std::shared_ptr<ng::runtime::Tensor> last_ng_tensor =
          input_caches[i].second;
      void* current_src_ptr = (void*)DMAHelper::base(&ctx->input(i));
      std::shared_ptr<ng::runtime::Tensor> current_ng_tensor =
          GetCurrentNgTensor(current_src_ptr, last_src_ptr, last_ng_tensor,
                             false, ng_exec, op_backend, ng_element_type,
                             ng_shape);
      bool is_cpu = m_op_backend_name == "CPU";

      if (!is_cpu && current_ng_tensor->get_stale()) {
        // Fresh or stale, in case of CPU this step is never needed
        try {
#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
          number_of_copies++;
          copy_log_str << " COPY_INP_VAL[" << i << "]";
#endif
          size_t copy_size =
              current_ng_tensor->get_element_count() * ng_element_type.size();
          string event_name =
              "Input_" + to_string(i) + "_" + to_string(copy_size);
          std::unique_ptr<ngraph::Event> event_copy_input_next(
              new ngraph::Event(event_name, name(), ""));
          current_ng_tensor->write(
              current_src_ptr, 0,
              current_ng_tensor->get_element_count() * ng_element_type.size());

          event_copy_input_next->Stop();
          input_copy_events.push_back(std::move(event_copy_input_next));

        } catch (const std::exception& exp) {
          Status status = errors::Internal(
              "Caught exception while transferring tensor data to nGraph: ",
              exp.what(), "\n");
          OP_REQUIRES(ctx, false, status);
        } catch (...) {
          Status status =
              errors::Internal("Error in transferring tensor data to nGraph\n");
          OP_REQUIRES(ctx, false, status);
        }
      }
      input_caches[i] = std::make_pair(current_src_ptr, current_ng_tensor);
      ng_inputs.push_back(current_ng_tensor);
    }  // for (int i = 0; i < input_shapes.size(); i++)

    // Now write the events back
    for (auto& next : input_copy_events) {
      ngraph::Event::write_trace(*next.get());
    }
    event_alloc_input.Stop();

    NGRAPH_VLOG(4) << "NGraphEncapsulateOp::Compute allocated argument tensors "
                      "for cluster "
                   << m_ngraph_cluster;

    // Allocate tensors for the output results.
    ngraph::Event event_alloc_output("Output: maybe create", name(), "");
    vector<shared_ptr<ng::runtime::Tensor>> ng_outputs;
    int ng_output_tensor_size_in_bytes = 0;
    std::vector<std::pair<void*, std::shared_ptr<ng::runtime::Tensor>>>&
        output_caches = m_ng_exec_output_cache_map[ng_exec];
    output_caches.resize(ng_exec->get_results().size());
    // ngraph executable returns get_results, using that to get the tensor shape
    // and element type.
    for (size_t i = 0; i < ng_exec->get_results().size(); i++) {
      auto ng_element = ng_exec->get_results()[i];
      auto ng_shape = ng_element->get_shape();
      auto ng_element_type = ng_element->get_element_type();

      // Create the TF output tensor
      vector<int64> dims;
      for (auto dim : ng_shape) {
        dims.push_back(dim);
      }
      TensorShape tf_shape(dims);
      Tensor* output_tensor = nullptr;
      OP_REQUIRES_OK(ctx, ctx->allocate_output(i, tf_shape, &output_tensor));

      // Make sure the nGraph-inferred element type agrees with what TensorFlow
      // expected.
      ng::element::Type expected_elem_type;
      OP_REQUIRES_OK(
          ctx, TFDataTypeToNGraphElementType(ctx->expected_output_dtype(i),
                                             &expected_elem_type));
      OP_REQUIRES(
          ctx, ng_element_type == expected_elem_type,
          errors::Internal("Element type inferred by nGraph does not match "
                           "the element type expected by TensorFlow"));

      void* last_dst_ptr = output_caches[i].first;
      std::shared_ptr<ng::runtime::Tensor> last_ng_tensor =
          output_caches[i].second;

      void* current_dst_ptr = DMAHelper::base(output_tensor);
      std::shared_ptr<ng::runtime::Tensor> current_ng_tensor = nullptr;
// if the output tensor is going to be assigned to a variable
// we ask nGraph to provide the output directly in the variable tensor
#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
      if (NGraphCatalog::ExistsInEncapOutputInfoMap(m_graph_id, name(), i)) {
        string output_key = NGraphCatalog::CreateNodeKey(m_graph_id, name(), i);
        string ref_var_name =
            NGraphCatalog::GetVariableSharedNameFromEncapOutputInfoMap(
                output_key);
        NGraphVar* var;
        OP_REQUIRES_OK(ctx, ctx->resource_manager()->Lookup<NGraphVar>(
                                ctx->resource_manager()->default_container(),
                                ref_var_name, &var));
        current_ng_tensor = var->ng_tensor();

        // There might be scenarios where the input and output tensors are the
        // same.The staleness determined for the input tensor should be the
        // final staleness for the given tensor. The staleness of output
        // tensor should not matter as this tensor is meant to be
        // overwritten with the computed value.
        // So not setting staleness here.
        output_caches[i] = std::make_pair(current_dst_ptr, current_ng_tensor);
        var->Unref();
        ng_outputs.push_back(current_ng_tensor);
        continue;
      }
#endif
      current_ng_tensor = GetCurrentNgTensor(
          current_dst_ptr, last_dst_ptr, last_ng_tensor, true, ng_exec,
          op_backend, ng_element_type, ng_shape);

      current_ng_tensor->set_stale(true);
      output_caches[i] = std::make_pair(current_dst_ptr, current_ng_tensor);

      ng_outputs.push_back(current_ng_tensor);
    }

    event_alloc_output.Stop();
    NGRAPH_VLOG(4)
        << "NGraphEncapsulateOp::Compute allocated result tensors for cluster "
        << m_ngraph_cluster;

#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
    NGRAPH_VLOG(4) << "NGraphEncapsulateOp::Compute getting input variables "
                      "from resource manager "
                   << m_ngraph_cluster;

    ngraph::Event event_input_check_in_catalog(
        "Get Variable Inputs from Resource Manager", name(), "");

    // Dealing with the input from Variable nodes here
    for (int input_index = 0; input_index < input_shapes.size();
         input_index++) {
      bool ref_exists = NGraphCatalog::ExistsInInputVariableSharedNameMap(
          m_graph_id, def().name(), input_index);

      if (!ref_exists) {
        OP_REQUIRES(ctx, ng_inputs[input_index] != nullptr,
                    errors::Internal("Input ", input_index,
                                     " is not in Catalog nor was set from TF"));
        continue;
      }

      string ref_var_name = NGraphCatalog::GetInputVariableSharedName(
          m_graph_id, def().name(), input_index);
      NGraphVar* var;
      OP_REQUIRES_OK(ctx, ctx->resource_manager()->Lookup<NGraphVar>(
                              ctx->resource_manager()->default_container(),
                              ref_var_name, &var));

      if (var->sync_ng_tensor()) {
        number_of_copies++;
        copy_log_str << "Var_Sync[" << input_index << "] ";
      }

      void* current_tf_ptr = (void*)DMAHelper::base(&ctx->input(input_index));
      bool is_stale = !m_freshness_tracker->IsFresh(current_tf_ptr, ng_exec);
      var->ng_tensor()->set_stale(is_stale);
      ng_inputs[input_index] = var->ng_tensor();

      var->Unref();
    }

    event_input_check_in_catalog.Stop();
    ngraph::Event::write_trace(event_input_check_in_catalog);
#endif

    int time_create_or_lookup_tensors = create_or_lookup_tensors.ElapsedInMS();

    // Execute the nGraph function.
    ngraph::Event event_execute_function("Execute nGraph", name(), "");
    Timer execute_function;
    {
      BackendManager::LockBackend(m_op_backend_name);
      NGRAPH_VLOG(4)
          << "NGraphEncapsulateOp::Compute call starting for cluster "
          << m_ngraph_cluster;
      try {
        ng_exec->call(ng_outputs, ng_inputs);
      } catch (const std::exception& exp) {
        ng_function = m_ng_function_map[ng_exec];
        BackendManager::UnlockBackend(m_op_backend_name);
        NgraphSerialize(
            "tf_function_error_" + ctx->op_kernel().name() + ".json",
            ng_function);
        OP_REQUIRES(ctx, false,
                    errors::Internal(
                        "Caught exception while executing nGraph computation: ",
                        exp.what(), "\n"));
      } catch (...) {
        ng_function = m_ng_function_map[ng_exec];
        BackendManager::UnlockBackend(m_op_backend_name);
        NgraphSerialize(
            "tf_function_error_" + ctx->op_kernel().name() + ".json",
            ng_function);
        OP_REQUIRES(
            ctx, false,
            errors::Internal("Error in executing the nGraph computation\n"));
      }
      BackendManager::UnlockBackend(m_op_backend_name);
    }
    int time_execute_function = execute_function.ElapsedInMS();
    event_execute_function.Stop();

    long vm, rss;
    MemoryProfile(vm, rss);
    NGRAPH_VLOG(1) << "NGRAPH_TF_MEM_PROFILE:  OP_ID: " << my_instance_id
                   << " Step_ID: " << ctx->step_id()
                   << " Cluster: " << ctx->op_kernel().name()
                   << " Input Tensors created: "
                   << ng_input_tensor_size_in_bytes / (1024 * 1024) << " MB"
                   << " Output Tensors created: "
                   << ng_output_tensor_size_in_bytes / (1024 * 1024) << " MB"
                   << " Total process memory: " << rss / (1024 * 1024) << " GB";

    NGRAPH_VLOG(4) << "NGraphEncapsulateOp::Compute call done for cluster "
                   << m_ngraph_cluster;

    // Copy value to host if backend is not CPU
    ngraph::Event event_copy_output("Output - copy back", name(), "");
    Timer copy_output_tensors_to_host;

    try {
      size_t output_tensor_count = output_caches.size();
      std::vector<std::unique_ptr<ngraph::Event>> output_copy_events;
#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
      if (m_number_outputs == -1) {
        NGRAPH_VLOG(4) << "Settig number of outputs for " << def().name();
        m_number_outputs = ng_outputs.size();
        NGRAPH_VLOG(4) << "Setting number of inputs for " << def().name();
        m_number_inputs = ng_inputs.size();
      }
      for (size_t i = 0; i < output_tensor_count; ++i) {
        // Sync the Var Tensor if required
        string output_key =
            NGraphCatalog::CreateNodeKey(m_graph_id, def().name(), i);
        bool ref_exists = NGraphCatalog::ExistsInEncapOutputInfoMap(output_key);

        if (ref_exists) {
          NGRAPH_VLOG(4) << "Syncing the output var tensor " << output_key;

          // Get var
          string ref_var_name =
              NGraphCatalog::GetVariableSharedNameFromEncapOutputInfoMap(
                  output_key);
          NGraphVar* var;
          OP_REQUIRES_OK(ctx, ctx->resource_manager()->Lookup<NGraphVar>(
                                  ctx->resource_manager()->default_container(),
                                  ref_var_name, &var));

          if (NGraphCatalog::GetCopyToTFFromEncapOutputInfoMap(output_key)) {
            if (var->copy_ng_to_tf()) {
              number_of_copies++;
              copy_log_str << " COPY_TO_TF ";
            }
            if (!NGraphCatalog::GetIsTFJustLookingFromEncapOutputInfoMap(
                    output_key)) {
              // Some tf op might update the ng-tensor value so mark it stale
              copy_log_str << " SET_SYNC ";
              var->set_sync_ng_tensor(true);
            }
          }
          var->Unref();
        }

        std::shared_ptr<ng::runtime::Tensor> dst_ng_tensor;
        void* dst_ptr;
        std::tie(dst_ptr, dst_ng_tensor) = output_caches[i];

        if (m_op_backend_name != "CPU" &&
            NGraphCatalog::EncapOutputIndexNeedsCopy(m_graph_id, def().name(),
                                                     i)) {
          number_of_copies++;
          copy_log_str << " COPY_OP_VAL[" << i << "]";

          NGRAPH_VLOG(4) << "Copying Output " << def().name()
                         << " ,index: " << i;
          auto ng_element_type = dst_ng_tensor->get_element_type();
          size_t copy_size =
              dst_ng_tensor->get_element_count() * ng_element_type.size();
          string event_name =
              "Output_" + to_string(i) + "_" + to_string(copy_size);
          std::unique_ptr<ngraph::Event> event_copy_output_next(
              new ngraph::Event(event_name, name(), ""));
          dst_ng_tensor->read(dst_ptr, 0, dst_ng_tensor->get_element_count() *
                                              ng_element_type.size());
          event_copy_output_next->Stop();
          output_copy_events.push_back(std::move(event_copy_output_next));
        }
      }
#else
      if (m_op_backend_name != "CPU") {
        for (size_t i = 0; i < output_tensor_count; ++i) {
          void* dst_ptr;
          std::shared_ptr<ng::runtime::Tensor> dst_ng_tensor;
          std::tie(dst_ptr, dst_ng_tensor) = output_caches[i];
          auto ng_element_type = dst_ng_tensor->get_element_type();
          std::unique_ptr<ngraph::Event> event_copy_output_next(
              new ngraph::Event(
                  ("Output_" + std::to_string(i) + "_" +
                   std::to_string(dst_ng_tensor->get_element_count() *
                                  ng_element_type.size())),
                  name(), ""));
          dst_ng_tensor->read(dst_ptr, 0, dst_ng_tensor->get_element_count() *
                                              ng_element_type.size());
          event_copy_output_next->Stop();
          output_copy_events.push_back(std::move(event_copy_output_next));
        }
      }
#endif
      // Now write the events back
      for (auto& next : output_copy_events) {
        ngraph::Event::write_trace(*next.get());
      }
    } catch (const std::exception& exp) {
      Status status = errors::Internal(
          "Caught exception while transferring tensor data to host: ",
          exp.what(), "\n");
      OP_REQUIRES(ctx, false, status);
    } catch (...) {
      Status status =
          errors::Internal("Error in transferring tensor data to host\n");
      OP_REQUIRES(ctx, false, status);
    }
    event_copy_output.Stop();

#if defined(NGRAPH_TF_ENABLE_VARIABLES_AND_OPTIMIZERS)
    copy_log_str << " Number of copies " << number_of_copies << "\n";
    if (log_copies) {
      cout << copy_log_str.str();
    }
#endif

    // Mark input tensors as fresh for the next time around.
    // Note: these ng_tensors are being marked fresh so that in the next
    // iteration if this encapsulate finds the tensor fresh, then it will use it
    for (size_t i = 0; i < input_shapes.size(); i++) {
      void* src_ptr = (void*)DMAHelper::base(&ctx->input(i));
      m_freshness_tracker->MarkFresh(src_ptr, ng_exec);
    }
    int time_copy_output_tensors_to_host =
        copy_output_tensors_to_host.ElapsedInMS();

    NGRAPH_VLOG(4)
        << "NGraphEncapsulateOp::Compute done marking fresh for cluster "
        << m_ngraph_cluster;
    NGRAPH_VLOG(1) << "NGRAPH_TF_TIMING_PROFILE: OP_ID: " << my_instance_id
                   << " Step_ID: " << ctx->step_id()
                   << " Cluster: " << ctx->op_kernel().name()
                   << " Time-Compute: " << compute_time.ElapsedInMS()
                   << " Function-Create-or-Lookup: "
                   << time_func_create_or_lookup << " Create-and-copy-tensors: "
                   << time_create_or_lookup_tensors
                   << " Execute: " << time_execute_function
                   << " Copy-outputs-to-host: "
                   << time_copy_output_tensors_to_host;
    event.Stop();
    ngraph::Event::write_trace(event_func_maybe_create);
    ngraph::Event::write_trace(event_alloc_output);
    ngraph::Event::write_trace(event_alloc_input);
    ngraph::Event::write_trace(event_execute_function);
    ngraph::Event::write_trace(event_copy_output);
    ngraph::Event::write_trace(event);

  }  // end compute

 private:
  // TF Graph for the cluster
  Graph m_graph;

  std::unordered_map<std::string, std::shared_ptr<ngraph::runtime::Executable>>
      m_ng_exec_map;
  std::unordered_map<std::shared_ptr<ngraph::runtime::Executable>,
                     std::shared_ptr<ngraph::Function>>
      m_ng_function_map;

  NgFunctionIOCache m_ng_exec_input_cache_map;
  NgFunctionIOCache m_ng_exec_output_cache_map;

  // Freshness tracker maintains a set of ng::functions using a particular base
  // pointer(for Tensor)
  // A single instance of freshness_tracker is used across all
  // nGraphEncapsulateOp and nGraphVariable op
  NGraphFreshnessTracker* m_freshness_tracker;
  int m_ngraph_cluster{-1};
  int m_graph_id{-1};
  std::vector<bool> m_input_is_static;
  std::mutex m_compute_lock;
  string m_op_backend_name;

  std::shared_ptr<ng::runtime::Tensor> GetCurrentNgTensor(
      void* current_tf_ptr, void* last_tf_ptr,
      const std::shared_ptr<ng::runtime::Tensor>& last_ng_tensor,
      const bool& output_tensor,
      const std::shared_ptr<ngraph::runtime::Executable>& ng_exec,
      ng::runtime::Backend* op_backend,
      const ng::element::Type& ng_element_type, const ng::Shape& ng_shape) {
    // NOTE: we assume that TF's pointers WILL change if it actually changes
    // values. ie, it will not reuse the same space if its rewritten it
    bool tf_tensor_has_changed = current_tf_ptr != last_tf_ptr;
    bool no_ng_tensor_found = last_ng_tensor == nullptr;
    bool is_cpu = m_op_backend_name == "CPU";

    // We need to check last_ng_tensor != nullptr, since there are cases where
    // at the first call to the ng_exec, both current_dst_ptr (when the
    // output is a 0-sized tensor) and last_dst_ptr (uninitialized at the
    // first call) are nullptr
    // A new tensor needs to be created for sure if no_ng_tensor_found
    // Additionally for CPU, it needs to be created if tf_tensor_has_changed,
    // for others, we do not create
    bool need_new_tensor_creation;
    if (is_cpu) {
      need_new_tensor_creation = no_ng_tensor_found || tf_tensor_has_changed;
    } else {
      need_new_tensor_creation = no_ng_tensor_found;
    }

    // It is stale if a new tensor was created OR the tf tensor has changed OR
    // (tf tensor has not changed, but freshness tracker says its stale)
    bool is_stale;
    if (output_tensor) {
      is_stale = true;  // For output tensors, it is always set stale to true
    } else {
      is_stale = need_new_tensor_creation || tf_tensor_has_changed ||
                 (!tf_tensor_has_changed &&
                  !m_freshness_tracker->IsFresh(current_tf_ptr, ng_exec));
    }
    // create a new ng tensor or use the last one
    std::shared_ptr<ng::runtime::Tensor> current_ng_tensor;
    if (need_new_tensor_creation) {
      if (is_cpu) {
        current_ng_tensor = op_backend->create_tensor(ng_element_type, ng_shape,
                                                      current_tf_ptr);
      } else {
        current_ng_tensor =
            op_backend->create_tensor(ng_element_type, ng_shape);
      }
    } else {
      current_ng_tensor = last_ng_tensor;
    }
    current_ng_tensor->set_stale(is_stale);
    return current_ng_tensor;
  }
  std::list<std::string> m_lru;
  size_t my_function_cache_depth_in_items = 16;
  static int s_instance_count;
  int my_instance_id{0};
  int m_number_outputs = -1;
  int m_number_inputs = -1;
};

int NGraphEncapsulateOp::s_instance_count = 0;

}  // namespace ngraph_bridge

REGISTER_KERNEL_BUILDER(Name("NGraphEncapsulate").Device(DEVICE_CPU),
                        ngraph_bridge::NGraphEncapsulateOp);

}  // namespace tensorflow
