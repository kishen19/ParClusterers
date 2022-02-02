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

#ifndef PARALLEL_CLUSTERING_CLUSTERERS_METRIC_EXAMPLE_CLUSTERER_H_
#define PARALLEL_CLUSTERING_CLUSTERERS_METRIC_EXAMPLE_CLUSTERER_H_

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "parcluster/api/config.pb.h"
#include "parcluster/api/datapoint.h"
#include "parcluster/api/in-memory-metric-clusterer-base.h"
#include "parcluster/api/status_macros.h"

namespace research_graph {
namespace in_memory {

class MetricExampleClusterer : public InMemoryMetricClusterer {
 public:
    absl::StatusOr<std::vector<int64_t>> Cluster(
      absl::Span<const DataPoint> datapoints,
      const MetricClustererConfig& config) const override;
};

}  // namespace in_memory
}  // namespace research_graph

#endif  // PARALLEL_CLUSTERING_CLUSTERERS_METRIC_EXAMPLE_CLUSTERER_H_
