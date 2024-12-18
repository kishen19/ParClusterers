#include "clusterers/kcore_clusterer/kcore-clusterer.h"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include "clusterers/kcore_clusterer/kcore_config.pb.h"
#include "absl/status/statusor.h"
#include "benchmarks/Connectivity/SimpleUnionAsync/Connectivity.h"
#include "benchmarks/KCore/JulienneDBS17/KCore.h"
#include "parcluster/api/config.pb.h"
#include "parcluster/api/gbbs-graph.h"
#include "parcluster/api/in-memory-clusterer-base.h"
#include "parcluster/api/parallel/parallel-graph-utils.h"
#include "parcluster/api/status_macros.h"

// TODO(jeshi): Temporarily necessary for k-core hierarchy code.
#include "gbbs/gbbs.h"
#include "gbbs/julienne.h"


// TODO(jeshi): This is a temporary location for the hierarchical k-core code,
// which should be integrated more robustly into GBBS instead of left here.
namespace gbbs {
namespace kcore_hierarchical {

parlay::sequence<uintE> GetBoundaryIndices(
    std::size_t num_keys,
    const std::function<bool(std::size_t, std::size_t)>& key_eq_func) {
      uintE null_key = UINT_E_MAX;
  parlay::sequence<uintE> mark_keys(num_keys + 1);
  parlay::parallel_for(0, num_keys, [&](std::size_t i) {
    if (i != 0 && key_eq_func(i, i - 1))
      mark_keys[i] = null_key;
    else
      mark_keys[i] = i;
  });
  mark_keys[num_keys] = num_keys;
  auto vert_buckets = 
      parlay::filter(mark_keys, [&](uintE x) -> bool { return x != null_key; });
  return vert_buckets;
}

class EfficientConnectWhilePeeling {
  public:
    EfficientConnectWhilePeeling() {}
    EfficientConnectWhilePeeling(size_t _n) {
      n = _n;
      uf = gbbs::simple_union_find::SimpleUnionAsyncStruct(n);
      links = parlay::sequence<uintE>::from_function(n, [&](size_t s) { return UINT_E_MAX; });
    }
    
    void initialize(size_t _n);

    template<class X, class Y, class F>
    void link(X a, Y b, F& cores);

    template<class X, class Y, class F>
    void check_equal_for_merge(X a, Y b, F& cores);

    template<class bucket_t>
    void init(bucket_t cur_bkt);

    gbbs::simple_union_find::SimpleUnionAsyncStruct uf =  gbbs::simple_union_find::SimpleUnionAsyncStruct(0);
    parlay::sequence<uintE> links;
    size_t n; // table size
};

void EfficientConnectWhilePeeling::initialize(size_t _n)  {
  this->n = _n;
  this->uf = gbbs::simple_union_find::SimpleUnionAsyncStruct(this->n);
  this->links = parlay::sequence<uintE>::from_function(this->n, [&](size_t s) { return UINT_E_MAX; });
}

template<class X, class Y, class F>
void EfficientConnectWhilePeeling::check_equal_for_merge(X a, Y b, F& cores) {
  if (cores(a) == cores(b)) {
    this->uf.unite(a, b);
  } else {
    auto link_b = links[b];
    if (link_b != UINT_E_MAX && cores(link_b) >= cores(a)) this->check_equal_for_merge(a, link_b, cores);
  }
}

template<class X, class Y, class F>
void EfficientConnectWhilePeeling::link(X a, Y b, F& cores) {
  a = simple_union_find::find_compress(a, this->uf.parents.data());
  b = simple_union_find::find_compress(b, this->uf.parents.data());

  if (cores(a) == cores(b)) {
    this->uf.unite(a, b);
    uintE parent = simple_union_find::find_compress(a, this->uf.parents.data());
    auto link_a = links[a]; auto link_b = links[b];
    if (link_a != UINT_E_MAX && parent != a) this->link(link_a, parent, cores);
    if (link_b != UINT_E_MAX && parent != b) this->link(link_b, parent, cores);
  }
  else if (cores(a) < cores(b)) {
      gbbs::uintE c = links[b];
      while (true) {
        c = links[b];
        if (c == UINT_E_MAX) {
          if (gbbs::atomic_compare_and_swap<uintE>(&(links[b]), UINT_E_MAX, a)) break;
        } else if (cores(c) < cores(a)) { // || (cores(c) == cores(a) && a < c)
          if (gbbs::atomic_compare_and_swap<uintE>(&(links[b]), c, a)) {
            auto parent_b = simple_union_find::find_compress(b, this->uf.parents.data());
            if (b != parent_b) this->link(a, parent_b, cores);
            this->link(a, c, cores);
            break;
          }
        } else {
          this->link(a, c, cores);
          break;
        }
      }
  }
  else {
    this->link(b, a, cores);
  }
}

template <class bucket_t>
void EfficientConnectWhilePeeling::init(bucket_t cur_bkt) {
}


class ConnectWhilePeeling {
  public:
    ConnectWhilePeeling() {}
    ConnectWhilePeeling(size_t _n){
      n = _n;
    }

    void initialize(size_t _n);

    template<class X, class Y, class F>
    void link(X x, Y index, F& cores);

    template<class bucket_t>
    void init(bucket_t cur_bkt);
    size_t n; // table size
    std::vector<gbbs::simple_union_find::SimpleUnionAsyncStruct> set_uf;
    std::vector<uintE> set_core;
};

void ConnectWhilePeeling::initialize(size_t _n) { this->n = _n; }

template<class X, class Y, class F>
void ConnectWhilePeeling::link(X x, Y index, F& cores) {
  parallel_for(0, set_uf.size(), [&](size_t idx){
    if (cores(index) >= set_core[idx]) set_uf[idx].unite(x, index);
  });
}

template <class bucket_t>
void ConnectWhilePeeling::init(bucket_t cur_bkt) {
  set_uf.push_back(gbbs::simple_union_find::SimpleUnionAsyncStruct(n));
  set_core.push_back(cur_bkt);
}

std::vector<uintE> construct_nd_connectivity_from_connect(uintE n, EfficientConnectWhilePeeling& cwp){
  auto parents = cwp.uf.finish();

  // Sort vertices from highest core # to lowest
  auto sort_by_parent = [&](uintE p, uintE q) {
      return parents[p] < parents[q];
  };
  auto sorted_vert = parlay::sequence<uintE>::from_function(n, [&](size_t i) { return i; });
  parlay::sample_sort_inplace(make_slice(sorted_vert), sort_by_parent);

  auto parent_eq_func = [&](size_t i, size_t j) {return parents[sorted_vert[i]] == parents[sorted_vert[j]];};
  auto vert_buckets = GetBoundaryIndices(n, parent_eq_func);


  std::vector<uintE> connectivity_tree(n);
  parallel_for(0, n, [&](std::size_t i){connectivity_tree[i] = UINT_E_MAX;});
  uintE prev_max_parent = n;
  parallel_for (0, vert_buckets.size()-1, [&](size_t i) {
    size_t start_index = vert_buckets[i];
    size_t end_index = vert_buckets[i + 1];

    //auto first_x = sorted_vert[start_index];
    parallel_for(0, end_index - start_index, [&](size_t a){
      connectivity_tree[sorted_vert[start_index + a]] = prev_max_parent + i;
    });
  });
  prev_max_parent += vert_buckets.size() - 1;
  //std::cout << "Finish first pass" << std::endl; fflush(stdout);
  connectivity_tree.resize(prev_max_parent);
  parallel_for(n, prev_max_parent, [&](std::size_t i){connectivity_tree[i] = UINT_E_MAX;});

  for (size_t i = 0; i < cwp.links.size(); i++) {
    if (cwp.links[i] == UINT_E_MAX) continue;
    if (i == parents[i]) {
      connectivity_tree[connectivity_tree[i]] = connectivity_tree[cwp.links[i]];
    }
  }
  return connectivity_tree;
}

std::vector<uintE> construct_nd_connectivity_from_connect(uintE n, ConnectWhilePeeling& connect_with_peeling){
  std::vector<uintE> connectivity_tree(n);
  parlay::sequence<uintE> prev_parent = parlay::sequence<uintE>::from_function(n, [&](size_t i){ return i; });
  uintE prev_max_parent = n;
  parallel_for(0, n, [&](std::size_t i){connectivity_tree[i] = UINT_E_MAX;});
  for (long idx = connect_with_peeling.set_uf.size() - 1; idx >= 0; idx--) {
    connectivity_tree.resize(prev_max_parent, UINT_E_MAX);
    parallel_for(0, n, [&](uintE l) { gbbs::simple_union_find::find_compress(l, connect_with_peeling.set_uf[idx].parents.data()); });

    parallel_for(0, n, [&](size_t l){
      connectivity_tree[prev_parent[l]] = prev_max_parent + connect_with_peeling.set_uf[idx].parents[l];
      // Update previous parent
      prev_parent[l] = connectivity_tree[prev_parent[l]];
    });
    // Update previous max parent
    prev_max_parent += n;
  }
  return connectivity_tree;
}

// Sort vertices from highest core # to lowest core #
// Take each "bucket" of vertices in each core #
// Maintain connectivity UF throughout all rounds
// Run connectivity considering only vertices previously considered, or vertices
// in the current bucket
// k and r here should be the same as that used in cliqueUpdate
template <class Graph>
std::vector<uintE> construct_nd_connectivity(Graph& GA, parlay::sequence<uintE>& cores){
  using W = typename Graph::weight_type;
  auto n = GA.n;

  // Sort vertices from highest core # to lowest core #
  auto get_core = [&](uintE p, uintE q){
    return cores[p] > cores[q];
  };
  auto sorted_vert = parlay::sequence<uintE>::from_function(n, [&](size_t i) { return i; });
  parlay::sample_sort_inplace(make_slice(sorted_vert), get_core);

  parlay::sequence<uintE> mark_keys(n + 1);
  parallel_for(0, n, [&](std::size_t i) {
    if (i != 0 && cores[sorted_vert[i]] == cores[sorted_vert[i-1]])
      mark_keys[i] = UINT_E_MAX;
    else
      mark_keys[i] = i;
  });
  mark_keys[n] = n;
  auto vert_buckets = 
      parlay::filter(mark_keys, [&](uintE x) -> bool { return x != UINT_E_MAX; });

  auto uf = gbbs::simple_union_find::SimpleUnionAsyncStruct(n);
  // TODO(jeshi): This isn't parallel, but I'd just like easy resizing atm
  std::vector<uintE> connectivity_tree(n);
  parlay::sequence<uintE> prev_parent = parlay::sequence<uintE>::from_function(n, [&](size_t i){ return i; });
  uintE prev_max_parent = n;
  //uintE prev_offset = 0;
  parallel_for(0, n, [&](std::size_t i){connectivity_tree[i] = UINT_E_MAX;});

  for (size_t i = 0; i < vert_buckets.size()-1; i++) {
    size_t start_index = vert_buckets[i];
    size_t end_index = vert_buckets[i + 1];

    auto first_x = sorted_vert[start_index];
    auto first_current_core = cores[first_x];
    // TODO: to get rid of this, gotta check for valid indices
    if (first_current_core != UINT_E_MAX && first_current_core != 0) {
      auto is_inactive = [&](size_t index) {
        return cores[index] < first_current_core;
      };

      parallel_for(start_index, end_index, [&](size_t j){
        // Vertices are those given by sorted_vert[j]
        auto x = sorted_vert[j];
  
        auto current_core = cores[x];
        assert(current_core == first_current_core);

        // Steps:
        // 1. Extract vertices given by index -- these will be the r-clique X
        // 2. Find all s-clique containing the r-clique X
        // 3. For each s-clique containing X, iterate over all combinations of
        // r vertices in that s-clique; these form r-clique X'
        // 4. Union X and X'
        auto map_f = [&](uintE __u, uintE __v, const W& w) {  
          if (!is_inactive(__v)) uf.unite(x, __v);
        };
        GA.get_vertex(x).out_neighbors().map(map_f, false);
      }, 1, true); // granularity
    }

    connectivity_tree.resize(prev_max_parent, UINT_E_MAX);

    parallel_for(0, n, [&](uintE l) { gbbs::simple_union_find::find_compress(l, uf.parents.data()); });

    parlay::sequence<uintE> map_parents = parlay::sequence<uintE>::from_function(n, [&](std::size_t l){return 0;});
    parallel_for(0, n, [&](size_t l){
      if (cores[l] != UINT_E_MAX && cores[l] >= first_current_core) map_parents[uf.parents[l]] = 1;
    });
    auto max_parent = parlay::scan_inplace(make_slice(map_parents));

    parallel_for(0, n, [&](size_t l){
      if (cores[l] != UINT_E_MAX && cores[l] >= first_current_core) {
        assert(prev_parent[l] < prev_max_parent);
        connectivity_tree[prev_parent[l]] = prev_max_parent + map_parents[uf.parents[l]];  //***: uf.parents[l];
        // Update previous parent
        prev_parent[l] = connectivity_tree[prev_parent[l]];
      }
    });
    prev_max_parent += max_parent;
  }

  return connectivity_tree;
}

template <class Graph, class CWP>
parlay::sequence<uintE> KCore(Graph& G, CWP& connect_while_peeling, size_t num_buckets, bool inline_hierarchy) {
  using W = typename Graph::weight_type;
  parlay::internal::timer t2; t2.start();
  const size_t n = G.n;
  auto D = parlay::sequence<uintE>::from_function(
      n, [&](size_t i) { return G.get_vertex(i).out_degree(); });

  auto em = hist_table<uintE, uintE>(std::make_tuple(UINT_E_MAX, 0),
                                     (size_t)G.m / 50);
  auto b = make_vertex_buckets(n, D, increasing, num_buckets);
  uintE prev_bkt = 0;
  parlay::internal::timer bt;

  size_t finished = 0, rho = 0, k_max = 0;
  while (finished != n) {
    bt.start();
    auto bkt = b.next_bucket();
    bt.stop();
    auto active = vertexSubset(n, std::move(bkt.identifiers));
    uintE k = bkt.id;
    finished += active.size();
    k_max = std::max(k_max, bkt.id);

    if (inline_hierarchy && prev_bkt != k && k != 0) {
      connect_while_peeling.init(k);
    }

    auto cores_func = [&](size_t a) -> uintE {
      if (D[a] > k) return n + 1;
      return D[a];
    };

    auto link_func = [&](uintE u) {
      auto map_f = [&](uintE __u, uintE v, const W& w) {  
        if (u != v && D[v] <= k) connect_while_peeling.link(u, v, cores_func);
      };
      G.get_vertex(u).out_neighbors().map(map_f, false);
    };

    vertexMap(active, link_func);

    auto apply_f = [&](const std::tuple<uintE, uintE>& p)
        -> const std::optional<std::tuple<uintE, uintE> > {
          uintE v = std::get<0>(p), edgesRemoved = std::get<1>(p);
          uintE deg = D[v];
          if (deg > k) {
            uintE new_deg = std::max(deg - edgesRemoved, k);
            D[v] = new_deg;
            return wrap(v, b.get_bucket(new_deg));
          }
          return std::nullopt;
        };

    auto cond_f = [](const uintE& u) { return true; };
    vertexSubsetData<uintE> moved =
        nghCount(G, active, cond_f, apply_f, em, no_dense);

    bt.start();
    b.update_buckets(moved);
    bt.stop();
    rho++;
    prev_bkt = k;
  }
  double tt2 = t2.stop();
  std::cout << "### Peel Running Time: " << tt2 << std::endl;
  std::cout << "### rho = " << rho << " k_{max} = " << k_max << "\n";
  // debug(bt.next("bucket time"););

  return D;
}

template <class Graph>
std::vector<uintE> KCore_connect(Graph& GA, size_t num_buckets, bool inline_hierarchy, bool efficient_inline_hierarchy) {
  if (efficient_inline_hierarchy) inline_hierarchy = true;

  parlay::sequence<uintE> D;
  
  EfficientConnectWhilePeeling ecwp;
  ConnectWhilePeeling connect_with_peeling;

  if (!efficient_inline_hierarchy) {
    if (inline_hierarchy) connect_with_peeling = ConnectWhilePeeling(GA.n);
    D = KCore(GA, connect_with_peeling, num_buckets, inline_hierarchy);
  } else {
    ecwp = EfficientConnectWhilePeeling(GA.n);
    D = KCore(GA, ecwp, num_buckets, inline_hierarchy);
  }

  std::vector<uintE> connect;
  if (!inline_hierarchy) {
    std::cout << "Running Connectivity" << std::endl;
    parlay::internal::timer t3; t3.start();
    connect = construct_nd_connectivity(GA, D);
    double tt3 = t3.stop();
    std::cout << "### Connectivity Running Time: " << tt3 << std::endl;
  } else {
    std::cout << "Constructing tree" << std::endl;
    parlay::internal::timer t3; t3.start();
    if (!efficient_inline_hierarchy) connect = construct_nd_connectivity_from_connect(GA.n, connect_with_peeling);
    else connect = construct_nd_connectivity_from_connect(GA.n, ecwp);
    double tt3 = t3.stop();
    std::cout << "### Connectivity Tree Running Time: " << tt3 << std::endl;
  }
  return connect;
}

}  // namespace kcore_hierarchical
}  // namespace gbbs

namespace research_graph {
namespace in_memory {

absl::StatusOr<KCoreClusterer::Clustering>
KCoreClusterer::Cluster(const ClustererConfig& config) const {
  KCoreClustererConfig kcore_config;
  config.any_config().UnpackTo(&kcore_config);

  std::size_t n = graph_.Graph()->n;
  int threshold = kcore_config.threshold();
  auto cores = gbbs::KCore(*(graph_.Graph()));

  std::cout << " threshold = " << threshold << std::endl;

  auto clusters = parlay::sequence<gbbs::uintE>::from_function(n, [&] (size_t i) { return i; });
  parlay::parallel_for(0, n, [&] (size_t i) {
    auto map_f = [&] (const auto& u, const auto& v, const auto& wgh) {
      if (cores[u] >= threshold && cores[v] >= threshold)
        gbbs::simple_union_find::unite_impl(u, v, clusters.data());
    };
    graph_.Graph()->get_vertex(i).out_neighbors().map(map_f);
  });

  parlay::parallel_for(0, n, [&] (gbbs::uintE i) {
    gbbs::simple_union_find::find_compress(i, clusters.data());
  });

  auto ret = research_graph::DenseClusteringToNestedClustering<gbbs::uintE>(clusters);
  std::cout << "Num clusters = " << ret.size() << std::endl;
  return ret;
}

absl::StatusOr<KCoreClusterer::Dendrogram>
KCoreClusterer::HierarchicalCluster(const ClustererConfig& config) const {
  KCoreClustererConfig kcore_config;
  config.any_config().UnpackTo(&kcore_config);

  bool inline_hierarchy = false;
  bool efficient_inline_hierarchy = false;

  const auto connectivity_method = kcore_config.connectivity_method();
  if (connectivity_method == KCoreClustererConfig::INLINE) inline_hierarchy = true;
  else if (connectivity_method == KCoreClustererConfig::EFFICIENT_INLINE) efficient_inline_hierarchy = true;

  return gbbs::kcore_hierarchical::KCore_connect(*(graph_.Graph()),
    kcore_config.num_buckets(), 
    inline_hierarchy, efficient_inline_hierarchy);
}

}  // namespace in_memory
}  // namespace research_graph
