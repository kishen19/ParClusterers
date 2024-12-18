// Copyright 2020 The Google Research Authors.
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

#include "clusterers/affinity/parallel-affinity-internal.h"

#include <stdlib.h>

#include <array>
#include <tuple>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "parcluster/api/config.pb.h"
#include "benchmarks/Connectivity/WorkEfficientSDB14/Connectivity.h"
#include "gbbs/bridge.h"
#include "gbbs/gbbs.h"
#include "gbbs/macros.h"
#include "parcluster/api/parallel/parallel-graph-utils.h"
#include "parcluster/api/parallel/parallel-sequence-ops.h"

namespace {

struct PerVertexClusterStats {
  gbbs::uintE cluster_id;
  float volume;
  float intra_cluster_weight;
  float inter_cluster_weight;
};

}  // namespace

namespace research_graph {
namespace in_memory {

namespace internal {

std::vector<ClusterStats> ComputeFinishedClusterStats(
    gbbs::symmetric_ptr_graph<gbbs::symmetric_vertex, float>& G,
    std::vector<gbbs::uintE>& cluster_ids,
    gbbs::uintE num_compressed_vertices) {
  std::size_t n = G.n;
  std::vector<ClusterStats> aggregate_cluster_stats(num_compressed_vertices,
                                                    {0, 0});
  std::vector<PerVertexClusterStats> cluster_stats(n);

  // Compute cluster statistics contributions of each vertex
  auto sum_map_f = [&](gbbs::uintE u, gbbs::uintE v, float weight) -> float {
    return weight;
  };
  parlay::parallel_for(0, n, [&](std::size_t i) {
    gbbs::uintE cluster_id_i = cluster_ids[i];
    auto add_m = parlay::addm<float>();
    auto volume = G.get_vertex(i).out_neighbors().reduce(sum_map_f, add_m);
    if (cluster_id_i == UINT_E_MAX) {
      cluster_stats[i] =
          PerVertexClusterStats{cluster_id_i, volume, float{0}, float{0}};
    } else {
      auto intra_cluster_sum_map_f = [&](gbbs::uintE u, gbbs::uintE v,
                                         float weight) -> float {
        if (cluster_id_i == cluster_ids[v] && v <= i) return weight;
        return 0;
      };
      auto inter_cluster_sum_map_f = [&](gbbs::uintE u, gbbs::uintE v,
                                         float weight) -> float {
        if (cluster_id_i != cluster_ids[v]) return weight;
        return 0;
      };
      auto intra_cluster_weight = G.get_vertex(i).out_neighbors().reduce(
          intra_cluster_sum_map_f, add_m);
      auto inter_cluster_weight = G.get_vertex(i).out_neighbors().reduce(
          inter_cluster_sum_map_f, add_m);
      cluster_stats[i] = PerVertexClusterStats{
          cluster_id_i, volume, intra_cluster_weight, inter_cluster_weight};
    }
  });

  // Compute total graph volume
  auto graph_volume_stats = parallel::Reduce<PerVertexClusterStats>(
      absl::Span<PerVertexClusterStats>(cluster_stats.data(),
                                        cluster_stats.size()),
      [&](PerVertexClusterStats a,
          PerVertexClusterStats b) -> PerVertexClusterStats {
        return PerVertexClusterStats{0, a.volume + b.volume, 0, 0};
      },
      PerVertexClusterStats{0, 0, 0, 0});
  float graph_volume = graph_volume_stats.volume;

  // Cluster statistics must now be aggregated per cluster id
  // Sort cluster statistics by cluster id
  auto cluster_stats_sort = parallel::ParallelSampleSort<PerVertexClusterStats>(
      absl::Span<PerVertexClusterStats>(cluster_stats.data(), n),
      [&](PerVertexClusterStats a, PerVertexClusterStats b) {
        return a.cluster_id < b.cluster_id;
      });

  // Obtain the boundary indices where statistics differ by cluster id
  // These indices are stored in filtered_mark_ids
  std::vector<gbbs::uintE> filtered_mark_ids =
      research_graph::parallel::GetBoundaryIndices<gbbs::uintE>(
          n, [&cluster_stats_sort](std::size_t i, std::size_t j) {
            return cluster_stats_sort[i].cluster_id ==
                   cluster_stats_sort[j].cluster_id;
          });
  std::size_t num_filtered_mark_ids = filtered_mark_ids.size() - 1;

  // Compute aggregate statistics by cluster id
  parlay::parallel_for(0, num_filtered_mark_ids, [&](size_t i) {
    // Combine cluster statistics from start_id_index to end_id_index
    gbbs::uintE start_id_index = filtered_mark_ids[i];
    gbbs::uintE end_id_index = filtered_mark_ids[i + 1];
    auto cluster_id = cluster_stats_sort[start_id_index].cluster_id;
    if (cluster_id != UINT_E_MAX) {
      gbbs::uintE cluster_size = end_id_index - start_id_index;
      auto stats_sum = parallel::Reduce<PerVertexClusterStats>(
          absl::Span<const PerVertexClusterStats>(
              cluster_stats_sort.begin() + start_id_index,
              end_id_index - start_id_index),
          [&](PerVertexClusterStats a, PerVertexClusterStats b) {
            return PerVertexClusterStats{
                0, a.volume + b.volume,
                a.intra_cluster_weight + b.intra_cluster_weight,
                a.inter_cluster_weight + b.inter_cluster_weight};
          },
          PerVertexClusterStats{0, 0, 0, 0});
      float density = (cluster_size >= 2)
                          ? stats_sum.intra_cluster_weight /
                                (static_cast<float>(cluster_size) *
                                 (cluster_size - 1) / 2.0)
                          : 0.0;
      float volume = stats_sum.volume;
      float denominator = std::min(volume, graph_volume - volume);
      float inter_cluster_weight =
          (denominator < 1e-6) ? 1.0
                               : stats_sum.inter_cluster_weight / denominator;
      aggregate_cluster_stats[cluster_id] =
          ClusterStats(density, inter_cluster_weight);
    }
  });

  return aggregate_cluster_stats;
}

}  // namespace internal

absl::StatusOr<std::vector<gbbs::uintE>> NearestNeighborLinkage(
    gbbs::symmetric_ptr_graph<gbbs::symmetric_vertex, float>& G,
    float weight_threshold) {
  std::size_t n = G.n;

  // Each vertex marks the neighbor to save, and stores the edge in
  // marked_neighbors
  std::vector<std::tuple<gbbs::uintE, gbbs::uintE>> marked_neighbors(2 * n);
  const gbbs::uintE undefined_neighbor = n;
  parlay::parallel_for(0, n, [&](std::size_t i) {
    auto vertex = G.get_vertex(i);
    float max_weight = weight_threshold;
    gbbs::uintE max_neighbor = undefined_neighbor;
    auto find_max_neighbor_func = [&](gbbs::uintE u, gbbs::uintE v,
                                      float weight) {
      if (std::tie(weight, v) > std::tie(max_weight, max_neighbor) ||
          (weight == weight_threshold && max_neighbor == undefined_neighbor)) {
        max_weight = weight;
        max_neighbor = v;
      }
    };
    vertex.out_neighbors().map(find_max_neighbor_func, false);
    marked_neighbors[i] = std::make_tuple(i, max_neighbor);
    marked_neighbors[i + n] = std::make_tuple(max_neighbor, i);
  });

  // Retrieve all valid edges and sort
  // Valid edges are stored in filtered_edges
  std::vector<std::tuple<gbbs::uintE, gbbs::uintE>> filtered_edges =
      parallel::FilterOut<std::tuple<gbbs::uintE, gbbs::uintE>>(
          absl::Span<const std::tuple<gbbs::uintE, gbbs::uintE>>(
              marked_neighbors.data(), 2 * n),
          [&](std::tuple<gbbs::uintE, gbbs::uintE> x) {
            return std::get<0>(x) != undefined_neighbor &&
                   std::get<1>(x) != undefined_neighbor;
          });
  std::size_t num_filtered_edges = filtered_edges.size();

  if (num_filtered_edges == 0) {
    std::vector<gbbs::uintE> labels(n);
    parlay::parallel_for(0, n, [&](std::size_t i) { labels[i] = i; });
    return labels;
  }

  // Compute offsets from filtered_edges
  auto filtered_edges_sort =
      parallel::ParallelSampleSort<std::tuple<gbbs::uintE, gbbs::uintE>>(
          absl::Span<std::tuple<gbbs::uintE, gbbs::uintE>>(
              filtered_edges.data(), filtered_edges.size()),
          [](std::tuple<gbbs::uintE, gbbs::uintE> a,
             std::tuple<gbbs::uintE, gbbs::uintE> b) {
            return std::get<0>(a) < std::get<0>(b);
          });

  auto cc_offsets = GetOffsets(
      [&filtered_edges_sort](std::size_t i) -> gbbs::uintE {
        return std::get<0>(filtered_edges_sort[i]);
      },
      num_filtered_edges, n);

  // Compute edges array from filtered_edges
  std::unique_ptr<std::tuple<gbbs::uintE, parlay::empty>[]> cc_edges(
      new std::tuple<gbbs::uintE, parlay::empty>[num_filtered_edges]);

  parlay::parallel_for(0, num_filtered_edges, [&](std::size_t i) {
    cc_edges[i] =
        std::make_tuple(std::get<1>(filtered_edges_sort[i]), parlay::empty());
  });

  // Construct an unweighted symmetric graph from cc_offsets and cc_edges
  auto G_cc = MakeGbbsGraph<parlay::empty>(cc_offsets, n, std::move(cc_edges),
                                         num_filtered_edges);

  // Perform connected components on G_cc
  parlay::sequence<gbbs::uintE> labels = gbbs::workefficient_cc::CC(*G_cc);

  auto labels_vector = std::vector<gbbs::uintE>(labels.begin(), labels.end());
  return labels_vector;
}

absl::StatusOr<
    GraphWithWeights<gbbs::uintE>>
CompressGraph(
    gbbs::symmetric_ptr_graph<gbbs::symmetric_vertex, float>& original_graph,
    std::vector<gbbs::uintE>& original_node_weights,
    std::vector<gbbs::uintE>& cluster_ids,
    const AffinityClustererConfig& affinity_config) {
  const auto edge_aggregation = affinity_config.edge_aggregation_function();
  // TODO(jeshi): Support percentile edge aggregation
  if (edge_aggregation == AffinityClustererConfig::PERCENTILE)
    return absl::UnimplementedError(
        "PERCENTILE aggregation for parallel affinity clusterer is "
        "unimplemented.");
  std::size_t n = original_graph.n;
  // Obtain the number of vertices in the new graph
  gbbs::uintE num_compressed_vertices =
      1 + parallel::Reduce<gbbs::uintE>(
              absl::Span<gbbs::uintE>(cluster_ids.data(), cluster_ids.size()),
              [&](gbbs::uintE reduce, gbbs::uintE a) {
                return (reduce == UINT_E_MAX)
                           ? a
                           : (a == UINT_E_MAX ? reduce : std::max(reduce, a));
              },
              UINT_E_MAX);

  // Retrieve node weights
  std::vector<gbbs::uintE> node_weights(num_compressed_vertices,
                                        gbbs::uintE{0});
  // TODO(jeshi): This could be made parallel, although perhaps it would be
  // too much overhead.
  for (std::size_t i = 0; i < n; ++i) {
    if (cluster_ids[i] != UINT_E_MAX)
      node_weights[cluster_ids[i]] += original_node_weights.empty() ? 1 : original_node_weights[i];
  }

  // Compute new inter cluster edges using sorting
  // TODO(jeshi): Allow optionality to choose between aggregation methods
  std::function<float(float, float)> edge_aggregation_func;
  std::function<float(std::tuple<gbbs::uintE, gbbs::uintE, float>)> scale_func =
    [](std::tuple<gbbs::uintE, gbbs::uintE, float> v) {
      return std::get<2>(v);
    };
  switch (edge_aggregation) {
    case AffinityClustererConfig::MAX:
      edge_aggregation_func = [](float w1, float w2) { return std::max(w1, w2); };
      break;
    case AffinityClustererConfig::SUM:
      edge_aggregation_func = [](float w1, float w2) { return w1 + w2; };
      break;
    case AffinityClustererConfig::DEFAULT_AVERAGE:
      edge_aggregation_func = [](float w1, float w2) { return w1 + w2; };
      if (!original_node_weights.empty()) {
        scale_func = [&original_node_weights] (std::tuple<gbbs::uintE, gbbs::uintE, float> v) {
          float scaling_factor = original_node_weights[std::get<0>(v)] * original_node_weights[std::get<1>(v)];
          return std::get<2>(v) * scaling_factor;
        };
      }
      break;
    case AffinityClustererConfig::CUT_SPARSITY:
      edge_aggregation_func = [](float w1, float w2) { return w1 + w2; };
      if (!original_node_weights.empty()) {
        scale_func = [&original_node_weights] (std::tuple<gbbs::uintE, gbbs::uintE, float> v) {
          float scaling_factor = std::min(original_node_weights[std::get<0>(v)], original_node_weights[std::get<1>(v)]);
          return std::get<2>(v) * scaling_factor;
        };
      }
      break;
    default:
      std::cout << "Unkown edge aggregation method" << std::endl; exit(0);
  }

  OffsetsEdges offsets_edges = ComputeInterClusterEdgesSort(
      original_graph, cluster_ids, num_compressed_vertices,
      edge_aggregation_func, std::not_equal_to<gbbs::uintE>(), scale_func);
  std::vector<gbbs::uintE> offsets = offsets_edges.offsets;
  std::size_t num_edges = offsets_edges.num_edges;
  std::unique_ptr<std::tuple<gbbs::uintE, float>[]> edges =
      std::move(offsets_edges.edges);

  if (edge_aggregation == AffinityClustererConfig::SUM ||
      edge_aggregation == AffinityClustererConfig::MAX) {
    return GraphWithWeights<gbbs::uintE>(MakeGbbsGraph<float>(offsets, num_compressed_vertices,
                                std::move(edges), num_edges), node_weights);
  }

  // Scale edge weights
  parlay::parallel_for(0, num_compressed_vertices, [&](std::size_t i) {
    auto offset = offsets[i];
    auto degree = offsets[i + 1] - offset;
    for (std::size_t j = 0; j < degree; j++) {
      const auto& edge = edges[offset + j];
      float scaling_factor = 0;
      if (edge_aggregation == AffinityClustererConfig::DEFAULT_AVERAGE) {
        scaling_factor = node_weights[i] * node_weights[std::get<0>(edge)];
      } else if (edge_aggregation == AffinityClustererConfig::CUT_SPARSITY) {
        scaling_factor =
            std::min(node_weights[i], node_weights[std::get<0>(edge)]);
      }
      edges[offset + j] = std::make_tuple(std::get<0>(edge),
                                          std::get<1>(edge) / scaling_factor);
    }
  });

  return GraphWithWeights<gbbs::uintE>(MakeGbbsGraph<float>(offsets, num_compressed_vertices,
                              std::move(edges), num_edges), node_weights);
}

InMemoryClusterer::Clustering ComputeClusters(
    const std::vector<gbbs::uintE>& cluster_ids,
    std::unique_ptr<bool[]> finished_vertex) {
  // Pack out finished vertices from the boolean array
  // TODO(jeshi): Switch finished_vertex to vector
  auto finished_vertex_pack = parallel::PackIndex<InMemoryClusterer::NodeId>(
      [&](std::size_t i) { return finished_vertex[i]; }, cluster_ids.size());

  auto get_clusters =
      [&](InMemoryClusterer::NodeId i) -> InMemoryClusterer::NodeId {
    return finished_vertex_pack[i];
  };
  return parallel::OutputIndicesById<gbbs::uintE, InMemoryClusterer::NodeId>(
      cluster_ids, get_clusters, finished_vertex_pack.size());
}

InMemoryClusterer::Clustering FindFinishedClusters(
    gbbs::symmetric_ptr_graph<gbbs::symmetric_vertex, float>& G,
    const AffinityClustererConfig& affinity_config,
    std::vector<gbbs::uintE>& cluster_ids) {
  if (affinity_config.active_cluster_conditions().empty())
    return InMemoryClusterer::Clustering();
  std::size_t n = G.n;

  gbbs::uintE num_compressed_vertices =
      1 + parallel::Reduce<gbbs::uintE>(
              absl::Span<gbbs::uintE>(cluster_ids.data(), cluster_ids.size()),
              [&](gbbs::uintE reduce, gbbs::uintE a) {
                return (reduce == UINT_E_MAX)
                           ? a
                           : (a == UINT_E_MAX ? reduce : std::max(reduce, a));
              },
              UINT_E_MAX);

  std::vector<internal::ClusterStats> aggregate_cluster_stats =
      internal::ComputeFinishedClusterStats(G, cluster_ids,
                                            num_compressed_vertices);

  // Check for finished clusters
  // TODO(jeshi): Use a unique ptr here
  auto finished = parlay::sequence<bool>(num_compressed_vertices, true);
  parlay::parallel_for(0, num_compressed_vertices, [&](std::size_t i) {
    for (long j = 0; j < affinity_config.active_cluster_conditions().size(); j++) {
      bool satisfied = true;
      auto condition = affinity_config.active_cluster_conditions().Get(j);
      if (condition.has_min_density() &&
          aggregate_cluster_stats[i].density < condition.min_density())
        satisfied = false;
      if (condition.has_min_conductance() &&
          aggregate_cluster_stats[i].conductance < condition.min_conductance())
        satisfied = false;
      if (satisfied) {
        finished[i] = false;
        break;
      }
    }
  });

  // Mark vertices belonging to finished clusters
  std::unique_ptr<bool[]> finished_vertex(new bool[n]);
  parlay::parallel_for(0, n, [&](std::size_t i) {
    finished_vertex[i] =
        cluster_ids[i] == UINT_E_MAX ? false : finished[cluster_ids[i]];
  });

  // Compute finished clusters
  auto finished_clusters =
      ComputeClusters(cluster_ids, std::move(finished_vertex));

  // Update the cluster ids for vertices belonging to finished clusters
  parlay::parallel_for(0, n, [&](size_t i) {
    if (cluster_ids[i] != UINT_E_MAX && finished[cluster_ids[i]])
      cluster_ids[i] = UINT_E_MAX;
  });

  return finished_clusters;
}

}  // namespace in_memory
}  // namespace research_graph
