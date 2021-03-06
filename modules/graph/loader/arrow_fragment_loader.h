/** Copyright 2020 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef MODULES_GRAPH_LOADER_ARROW_FRAGMENT_LOADER_H_
#define MODULES_GRAPH_LOADER_ARROW_FRAGMENT_LOADER_H_

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "client/client.h"
#include "grape/worker/comm_spec.h"
#include "io/io/io_factory.h"
#include "io/io/local_io_adaptor.h"

#include "graph/fragment/arrow_fragment.h"
#include "graph/fragment/arrow_fragment_group.h"
#include "graph/fragment/graph_schema.h"
#include "graph/fragment/property_graph_types.h"
#include "graph/loader/basic_arrow_fragment_loader.h"
#include "graph/utils/error.h"
#include "graph/utils/partitioner.h"
#include "graph/vertex_map/arrow_vertex_map.h"

#define HASH_PARTITION

namespace vineyard {
template <typename OID_T = property_graph_types::OID_TYPE,
          typename VID_T = property_graph_types::VID_TYPE>
class ArrowFragmentLoader {
  using oid_t = OID_T;
  using vid_t = VID_T;
  using label_id_t = property_graph_types::LABEL_ID_TYPE;
  using internal_oid_t = typename InternalType<oid_t>::type;
  using oid_array_t = typename vineyard::ConvertToArrowType<oid_t>::ArrayType;
  using vertex_map_t = ArrowVertexMap<internal_oid_t, vid_t>;
  const int id_column = 0;
  const int src_column = 0;
  const int dst_column = 1;
#ifdef HASH_PARTITION
  using partitioner_t = HashPartitioner<oid_t>;
#else
  using partitioner_t = SegmentedPartitioner<oid_t>;
#endif
  using basic_loader_t = BasicArrowFragmentLoader<oid_t, vid_t, partitioner_t>;

 public:
  ArrowFragmentLoader(vineyard::Client& client,
                      const grape::CommSpec& comm_spec,
                      label_id_t vertex_label_num, label_id_t edge_label_num,
                      std::string efile, std::string vfile,
                      bool directed = true)
      : client_(client),
        comm_spec_(comm_spec),
        efile_(std::move(efile)),
        vfile_(std::move(vfile)),
        vertex_label_num_(vertex_label_num),
        edge_label_num_(edge_label_num),
        directed_(directed),
        basic_arrow_fragment_loader_(comm_spec) {}

  ArrowFragmentLoader(
      vineyard::Client& client, const grape::CommSpec& comm_spec,
      label_id_t vertex_label_num, label_id_t edge_label_num,
      std::vector<std::shared_ptr<arrow::Table>> const& partial_v_tables,
      std::vector<std::shared_ptr<arrow::Table>> const& partial_e_tables,
      bool directed = true)
      : client_(client),
        comm_spec_(comm_spec),
        vertex_label_num_(vertex_label_num),
        edge_label_num_(edge_label_num),
        partial_v_tables_(partial_v_tables),
        partial_e_tables_(partial_e_tables),
        directed_(directed),
        basic_arrow_fragment_loader_(comm_spec) {}

  ~ArrowFragmentLoader() = default;

  boost::leaf::result<vineyard::ObjectID> LoadFragment() {
    BOOST_LEAF_CHECK(initPartitioner());
    BOOST_LEAF_CHECK(initBasicLoader());

    BOOST_LEAF_AUTO(frag_id, shuffleAndBuild());
    return frag_id;
  }

  boost::leaf::result<vineyard::ObjectID> LoadFragmentAsFragmentGroup() {
    BOOST_LEAF_AUTO(frag_id, LoadFragment());
    BOOST_LEAF_AUTO(group_id,
                    constructFragmentGroup(client_, frag_id, comm_spec_,
                                           vertex_label_num_, edge_label_num_));
    return group_id;
  }

 private:
  void SerializeSchema(const std::shared_ptr<arrow::Schema>& schema,
                       std::shared_ptr<arrow::Buffer>* out) {
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
    CHECK_ARROW_ERROR(arrow::ipc::SerializeSchema(
        *schema, nullptr, arrow::default_memory_pool(), out));
#elif defined(ARROW_VERSION) && ARROW_VERSION < 2000000
    CHECK_ARROW_ERROR_AND_ASSIGN(
        *out, arrow::ipc::SerializeSchema(*schema, nullptr,
                                          arrow::default_memory_pool()));
#else
    CHECK_ARROW_ERROR_AND_ASSIGN(
        *out,
        arrow::ipc::SerializeSchema(*schema, arrow::default_memory_pool()));
#endif
  }

  void DeserializeSchema(const std::shared_ptr<arrow::Buffer>& buffer,
                         std::shared_ptr<arrow::Schema>* out) {
    arrow::io::BufferReader reader(buffer);
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
    CHECK_ARROW_ERROR(arrow::ipc::ReadSchema(&reader, nullptr, out));
#else
    CHECK_ARROW_ERROR_AND_ASSIGN(*out,
                                 arrow::ipc::ReadSchema(&reader, nullptr));
#endif
  }

  std::shared_ptr<arrow::Schema> FindMostCommonSchema(
      const std::vector<std::shared_ptr<arrow::Schema>>& schemas) {
    size_t field_num = 0;
    for (size_t i = 0; i < schemas.size(); ++i) {
      if (schemas[i] != nullptr) {
        field_num = schemas[i]->num_fields();
        break;
      }
    }
    // Find most common fields.
    std::vector<std::vector<std::shared_ptr<arrow::Field>>> fields(field_num);
    for (size_t i = 0; i < field_num; ++i) {
      for (size_t j = 0; j < schemas.size(); ++j) {
        if (schemas[j] == nullptr) {
          continue;
        }
        fields[i].push_back(schemas[j]->field(i));
      }
    }
    std::vector<std::shared_ptr<arrow::Field>> most_common_fields(field_num);
    for (size_t i = 0; i < field_num; ++i) {
      std::sort(fields[i].begin(), fields[i].end(),
                [](const std::shared_ptr<arrow::Field>& lhs,
                   const std::shared_ptr<arrow::Field>& rhs) {
                  return lhs->type()->ToString() < rhs->type()->ToString();
                });

      // find the max frequency using linear traversal
      int max_count = 1, curr_count = 1;
      auto res = fields[i][0];
      for (size_t j = 1; j < fields[i].size(); j++) {
        if (fields[i][j]->type()->Equals(fields[i][j - 1]->type())) {
          curr_count++;
        } else {
          if (curr_count > max_count) {
            max_count = curr_count;
            res = fields[i][j - 1];
          }
          curr_count = 1;
        }
      }

      // If last element is most frequent
      if (curr_count > max_count) {
        max_count = curr_count;
        res = fields[i].back();
      }
      most_common_fields[i] = res;
    }
    auto final_schema = std::make_shared<arrow::Schema>(most_common_fields);
    return final_schema;
  }

  boost::leaf::result<void> SyncSchema(std::shared_ptr<arrow::Table>& table,
                                       grape::CommSpec comm_spec) {
    if (comm_spec.worker_num() == 1) {
      return boost::leaf::result<void>();
    }
    std::shared_ptr<arrow::Schema> final_schema;
    int final_serialized_schema_size;
    std::shared_ptr<arrow::Buffer> schema_buffer;
    int size = 0;
    if (table != nullptr) {
      SerializeSchema(table->schema(), &schema_buffer);
      size = static_cast<int>(schema_buffer->size());
    }
    if (comm_spec.worker_id() == 0) {
      std::vector<int> recvcounts(comm_spec.worker_num());

      MPI_Gather(&size, sizeof(int), MPI_CHAR, &recvcounts[0], sizeof(int),
                 MPI_CHAR, 0, comm_spec.comm());
      std::vector<int> displs(comm_spec.worker_num());
      int total_len = 0;
      displs[0] = 0;
      total_len += recvcounts[0];

      for (size_t i = 1; i < recvcounts.size(); i++) {
        total_len += recvcounts[i];
        displs[i] = displs[i - 1] + recvcounts[i - 1];
      }
      if (total_len == 0) {
        RETURN_GS_ERROR(ErrorCode::kIOError, "All schema is empty");
      }
      char* total_string = static_cast<char*>(malloc(total_len * sizeof(char)));
      if (size == 0) {
        MPI_Gatherv(NULL, 0, MPI_CHAR, total_string, &recvcounts[0], &displs[0],
                    MPI_CHAR, 0, comm_spec.comm());

      } else {
        MPI_Gatherv(schema_buffer->data(), schema_buffer->size(), MPI_CHAR,
                    total_string, &recvcounts[0], &displs[0], MPI_CHAR, 0,
                    comm_spec.comm());
      }
      std::vector<std::shared_ptr<arrow::Buffer>> buffers(
          comm_spec.worker_num());
      for (size_t i = 0; i < buffers.size(); ++i) {
        buffers[i] = std::make_shared<arrow::Buffer>(
            reinterpret_cast<unsigned char*>(total_string + displs[i]),
            recvcounts[i]);
      }
      std::vector<std::shared_ptr<arrow::Schema>> schemas(
          comm_spec.worker_num());
      for (size_t i = 0; i < schemas.size(); ++i) {
        if (recvcounts[i] == 0) {
          continue;
        }
        DeserializeSchema(buffers[i], &schemas[i]);
      }

      final_schema = FindMostCommonSchema(schemas);

      SerializeSchema(final_schema, &schema_buffer);
      final_serialized_schema_size = static_cast<int>(schema_buffer->size());

      MPI_Bcast(&final_serialized_schema_size, sizeof(int), MPI_CHAR, 0,
                comm_spec.comm());
      MPI_Bcast(const_cast<char*>(
                    reinterpret_cast<const char*>(schema_buffer->data())),
                final_serialized_schema_size, MPI_CHAR, 0, comm_spec.comm());
      free(total_string);
    } else {
      MPI_Gather(&size, sizeof(int), MPI_CHAR, 0, sizeof(int), MPI_CHAR, 0,
                 comm_spec.comm());
      if (size == 0) {
        MPI_Gatherv(NULL, 0, MPI_CHAR, NULL, NULL, NULL, MPI_CHAR, 0,
                    comm_spec.comm());
      } else {
        MPI_Gatherv(schema_buffer->data(), size, MPI_CHAR, NULL, NULL, NULL,
                    MPI_CHAR, 0, comm_spec.comm());
      }

      MPI_Bcast(&final_serialized_schema_size, sizeof(int), MPI_CHAR, 0,
                comm_spec.comm());
      char* recv_buf = static_cast<char*>(
          malloc(final_serialized_schema_size * sizeof(char)));
      MPI_Bcast(recv_buf, final_serialized_schema_size, MPI_CHAR, 0,
                comm_spec.comm());
      auto buffer = std::make_shared<arrow::Buffer>(
          reinterpret_cast<unsigned char*>(recv_buf),
          final_serialized_schema_size);
      DeserializeSchema(buffer, &final_schema);
      free(recv_buf);
    }
    if (table == nullptr) {
      VY_OK_OR_RAISE(vineyard::EmptyTableBuilder::Build(final_schema, table));
    } else {
      CHECK_ARROW_ERROR_AND_ASSIGN(table,
                                   PromoteTableToSchema(table, final_schema));
    }
    return boost::leaf::result<void>();
  }

  boost::leaf::result<void> initPartitioner() {
#ifdef HASH_PARTITION
    partitioner_.Init(comm_spec_.fnum());
#else
    std::vector<std::shared_ptr<arrow::Table>> vtables;
    {
      BOOST_LEAF_AUTO(tmp, loadVertexTables(vfile_, vertex_label_num_, 0, 1));
      vtables = tmp;
    }
    std::vector<oid_t> oid_list;

    for (auto& table : vtables) {
      std::shared_ptr<arrow::ChunkedArray> oid_array_chunks =
          table->column(id_column);
      size_t chunk_num = oid_array_chunks->num_chunks();

      for (size_t chunk_i = 0; chunk_i != chunk_num; ++chunk_i) {
        std::shared_ptr<oid_array_t> array =
            std::dynamic_pointer_cast<oid_array_t>(
                oid_array_chunks->chunk(chunk_i));
        int64_t length = array->length();
        for (int64_t i = 0; i < length; ++i) {
          oid_list.emplace_back(oid_t(array->GetView(i)));
        }
      }
    }

    partitioner_.Init(comm_spec_.fnum(), oid_list);
#endif
    return boost::leaf::result<void>();
  }

  boost::leaf::result<void> initBasicLoader() {
    std::vector<std::shared_ptr<arrow::Table>> partial_v_tables;
    std::vector<std::shared_ptr<arrow::Table>> partial_e_tables;
    if (!partial_v_tables_.empty() && !partial_e_tables_.empty()) {
      partial_v_tables = partial_v_tables_;
      partial_e_tables = partial_e_tables_;
    } else {
      BOOST_LEAF_AUTO(tmp_v, loadVertexTables(vfile_, vertex_label_num_,
                                              comm_spec_.worker_id(),
                                              comm_spec_.worker_num()));
      BOOST_LEAF_AUTO(
          tmp_e, loadEdgeTables(efile_, edge_label_num_, comm_spec_.worker_id(),
                                comm_spec_.worker_num()));
      partial_v_tables = tmp_v;
      partial_e_tables = tmp_e;
    }
    basic_arrow_fragment_loader_.Init(partial_v_tables, partial_e_tables);
    basic_arrow_fragment_loader_.SetPartitioner(partitioner_);

    return boost::leaf::result<void>();
  }

  boost::leaf::result<vineyard::ObjectID> shuffleAndBuild() {
    BOOST_LEAF_AUTO(local_v_tables,
                    basic_arrow_fragment_loader_.ShuffleVertexTables());
    auto oid_lists = basic_arrow_fragment_loader_.GetOidLists();

    BasicArrowVertexMapBuilder<typename InternalType<oid_t>::type, vid_t>
        vm_builder(client_, comm_spec_.fnum(), vertex_label_num_, oid_lists);
    auto vm = vm_builder.Seal(client_);
    auto vm_ptr =
        std::dynamic_pointer_cast<vertex_map_t>(client_.GetObject(vm->id()));
    auto mapper = [&vm_ptr](fid_t fid, internal_oid_t oid, vid_t& gid) {
      return vm_ptr->GetGid(fid, oid, gid);
    };
    BOOST_LEAF_AUTO(local_e_tables,
                    basic_arrow_fragment_loader_.ShuffleEdgeTables(mapper));
    BasicArrowFragmentBuilder<oid_t, vid_t> frag_builder(client_, vm_ptr);

    {
      // Make sure the sequence of tables in local_v_tables and local_e_tables
      // are correspond to their label_index.
      std::vector<std::shared_ptr<arrow::Table>> rearranged_v_tables;
      rearranged_v_tables.resize(local_v_tables.size());
      for (auto table : local_v_tables) {
        auto meta = table->schema()->metadata();
        auto label_index_field = meta->FindKey("label_index");
        CHECK_NE(label_index_field, -1);
        auto label_index = std::stoi(meta->value(label_index_field));
        CHECK_LT(label_index, rearranged_v_tables.size());
        rearranged_v_tables[label_index] = table;
      }
      local_v_tables = rearranged_v_tables;

      std::vector<std::shared_ptr<arrow::Table>> rearranged_e_tables;
      rearranged_e_tables.resize(local_e_tables.size());
      for (auto table : local_e_tables) {
        auto meta = table->schema()->metadata();
        auto label_index_field = meta->FindKey("label_index");
        CHECK_NE(label_index_field, -1);
        auto label_index = std::stoi(meta->value(label_index_field));
        CHECK_LT(label_index, rearranged_e_tables.size());
        rearranged_e_tables[label_index] = table;
      }
      local_e_tables = rearranged_e_tables;
    }

    PropertyGraphSchema schema;
    schema.set_fnum(comm_spec_.fnum());

    for (auto table : local_v_tables) {
      std::unordered_map<std::string, std::string> kvs;
      table->schema()->metadata()->ToUnorderedMap(&kvs);
      std::string type = kvs["type"];
      std::string label = kvs["label"];

      auto entry = schema.CreateEntry(label, type);
      // entry->add_primary_keys(1, table->schema()->field_names());

      // N.B. ID column is already been removed.
      for (int64_t i = 0; i < table->num_columns(); ++i) {
        entry->AddProperty(table->schema()->field(i)->name(),
                           table->schema()->field(i)->type());
      }
    }
    for (auto table : local_e_tables) {
      std::unordered_map<std::string, std::string> kvs;
      table->schema()->metadata()->ToUnorderedMap(&kvs);
      std::string type = kvs["type"];
      std::string label = kvs["label"];

      auto entry = schema.CreateEntry(label, type);

      std::string sub_label = kvs["sub_label_num"];
      if (!sub_label.empty()) {
        int sub_label_num = std::stoi(sub_label);
        for (int i = 0; i < sub_label_num; ++i) {
          std::string src_label = kvs["src_label_" + std::to_string(i)];
          std::string dst_label = kvs["dst_label_" + std::to_string(i)];

          if (!src_label.empty() && !dst_label.empty()) {
            entry->AddRelation(src_label, dst_label);
          }
        }
      }
      // N.B. Skip first two ID columns.
      for (int64_t i = 2; i < table->num_columns(); ++i) {
        entry->AddProperty(table->schema()->field(i)->name(),
                           table->schema()->field(i)->type());
      }
    }
    frag_builder.SetPropertyGraphSchema(std::move(schema));

    BOOST_LEAF_CHECK(frag_builder.Init(comm_spec_.fid(), comm_spec_.fnum(),
                                       std::move(local_v_tables),
                                       std::move(local_e_tables), directed_));
    auto frag = std::dynamic_pointer_cast<ArrowFragment<oid_t, vid_t>>(
        frag_builder.Seal(client_));

    VINEYARD_CHECK_OK(client_.Persist(frag->id()));
    return frag->id();
  }

  boost::leaf::result<vineyard::ObjectID> constructFragmentGroup(
      vineyard::Client& client, vineyard::ObjectID frag_id,
      grape::CommSpec comm_spec, label_id_t v_label_num,
      label_id_t e_label_num) {
    vineyard::ObjectID group_object_id;
    uint64_t instance_id = client.instance_id();

    if (comm_spec.worker_id() == 0) {
      std::vector<uint64_t> gathered_instance_ids(comm_spec.worker_num());
      std::vector<vineyard::ObjectID> gathered_object_ids(
          comm_spec.worker_num());

      MPI_Gather(&instance_id, sizeof(uint64_t), MPI_CHAR,
                 &gathered_instance_ids[0], sizeof(uint64_t), MPI_CHAR, 0,
                 comm_spec.comm());

      MPI_Gather(&frag_id, sizeof(vineyard::ObjectID), MPI_CHAR,
                 &gathered_object_ids[0], sizeof(vineyard::ObjectID), MPI_CHAR,
                 0, comm_spec.comm());

      ArrowFragmentGroupBuilder builder;
      builder.set_total_frag_num(comm_spec.fnum());
      builder.set_vertex_label_num(v_label_num);
      builder.set_edge_label_num(e_label_num);
      for (fid_t i = 0; i < comm_spec.fnum(); ++i) {
        builder.AddFragmentObject(
            i, gathered_object_ids[comm_spec.FragToWorker(i)],
            gathered_instance_ids[comm_spec.FragToWorker(i)]);
      }

      auto group_object =
          std::dynamic_pointer_cast<ArrowFragmentGroup>(builder.Seal(client));
      group_object_id = group_object->id();
      VY_OK_OR_RAISE(client.Persist(group_object_id));

      MPI_Bcast(&group_object_id, sizeof(vineyard::ObjectID), MPI_CHAR, 0,
                comm_spec.comm());

    } else {
      MPI_Gather(&instance_id, sizeof(uint64_t), MPI_CHAR, NULL,
                 sizeof(uint64_t), MPI_CHAR, 0, comm_spec.comm());
      MPI_Gather(&frag_id, sizeof(vineyard::ObjectID), MPI_CHAR, NULL,
                 sizeof(vineyard::ObjectID), MPI_CHAR, 0, comm_spec.comm());

      MPI_Bcast(&group_object_id, sizeof(vineyard::ObjectID), MPI_CHAR, 0,
                comm_spec.comm());
    }
    return group_object_id;
  }

  boost::leaf::result<std::vector<std::shared_ptr<arrow::Table>>>
  loadVertexTables(const std::string& file, label_id_t label_num, int index,
                   int total_parts) {
    std::vector<std::string> file_list;
    boost::split(file_list, file, boost::is_any_of(";"));

    std::vector<std::shared_ptr<arrow::Table>> tables(file_list.size() *
                                                      label_num);

    auto io_deleter = [](vineyard::LocalIOAdaptor* adaptor) {
      VINEYARD_CHECK_OK(adaptor->Close());
      delete adaptor;
    };
    try {
      for (size_t i = 0; i < file_list.size(); ++i) {
        for (label_id_t label = 0; label < label_num; ++label) {
          std::unique_ptr<vineyard::LocalIOAdaptor,
                          std::function<void(vineyard::LocalIOAdaptor*)>>
              io_adaptor(new vineyard::LocalIOAdaptor(file_list[i] + "_" +
                                                      std::to_string(label) +
                                                      "#header_row=true"),
                         io_deleter);
          VY_OK_OR_RAISE(io_adaptor->SetPartialRead(index, total_parts));
          VY_OK_OR_RAISE(io_adaptor->Open());
          std::shared_ptr<arrow::Table> table;
          VY_OK_OR_RAISE(io_adaptor->ReadTable(&table));
          SyncSchema(table, comm_spec_);
          auto meta = std::make_shared<arrow::KeyValueMetadata>();
          meta->Append("type", "VERTEX");
          meta->Append("label_index", std::to_string(label));
          meta->Append("label", "_");
          meta->Append(basic_loader_t::ID_COLUMN, "0");
          tables[i * label_num + label] = table->ReplaceSchemaMetadata(meta);
        }
      }
    } catch (std::exception& e) {
      return boost::leaf::new_error(ErrorCode::kIOError, std::string(e.what()));
    }
    return tables;
  }

  boost::leaf::result<std::vector<std::shared_ptr<arrow::Table>>>
  loadEdgeTables(const std::string& file, label_id_t label_num, int index,
                 int total_parts) {
    std::vector<std::string> file_list;
    boost::split(file_list, file, boost::is_any_of(";"));

    std::vector<std::shared_ptr<arrow::Table>> tables(file_list.size() *
                                                      label_num);

    auto io_deleter = [](vineyard::LocalIOAdaptor* adaptor) {
      VINEYARD_CHECK_OK(adaptor->Close());
      delete adaptor;
    };
    try {
      for (size_t i = 0; i < file_list.size(); ++i) {
        for (label_id_t label = 0; label < label_num; ++label) {
          std::unique_ptr<vineyard::LocalIOAdaptor,
                          std::function<void(vineyard::LocalIOAdaptor*)>>
              io_adaptor(new vineyard::LocalIOAdaptor(file_list[i] + "_" +
                                                      std::to_string(label) +
                                                      "#header_row=true"),
                         io_deleter);
          VY_OK_OR_RAISE(io_adaptor->SetPartialRead(index, total_parts));
          VY_OK_OR_RAISE(io_adaptor->Open());
          std::shared_ptr<arrow::Table> table;
          VY_OK_OR_RAISE(io_adaptor->ReadTable(&table));
          SyncSchema(table, comm_spec_);
          auto meta = std::make_shared<arrow::KeyValueMetadata>();
          meta->Append("type", "EDGE");
          meta->Append("label", "_");
          meta->Append("label_index", std::to_string(label));
          meta->Append("sub_label_num", "1");
          meta->Append(basic_loader_t::SRC_COLUMN, std::to_string(src_column));
          meta->Append(basic_loader_t::DST_COLUMN, std::to_string(dst_column));
          tables[i * label_num + label] = table->ReplaceSchemaMetadata(meta);
        }
      }
    } catch (std::exception& e) {
      return boost::leaf::new_error(ErrorCode::kIOError, std::string(e.what()));
    }
    return tables;
  }

  arrow::Status swapColumn(std::shared_ptr<arrow::Table> in, int lhs_index,
                           int rhs_index, std::shared_ptr<arrow::Table>* out) {
    if (lhs_index == rhs_index) {
      out = &in;
      return arrow::Status::OK();
    }
    if (lhs_index > rhs_index) {
      return arrow::Status::Invalid("lhs index must smaller than rhs index.");
    }
    auto field = in->schema()->field(rhs_index);
    auto column = in->column(rhs_index);
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
    CHECK_ARROW_ERROR(in->RemoveColumn(rhs_index, &in));
    CHECK_ARROW_ERROR(in->AddColumn(lhs_index, field, column, out));
#else
    CHECK_ARROW_ERROR_AND_ASSIGN(in, in->RemoveColumn(rhs_index));
    CHECK_ARROW_ERROR_AND_ASSIGN(*out, in->AddColumn(lhs_index, field, column));
#endif
    return arrow::Status::OK();
  }

  vineyard::Client& client_;
  grape::CommSpec comm_spec_;
  std::string efile_, vfile_;

  std::vector<std::shared_ptr<arrow::Table>> partial_v_tables_,
      partial_e_tables_;

  label_id_t vertex_label_num_, edge_label_num_;
  partitioner_t partitioner_;

  bool directed_;
  basic_loader_t basic_arrow_fragment_loader_;
};

}  // namespace vineyard

#endif  // MODULES_GRAPH_LOADER_ARROW_FRAGMENT_LOADER_H_
