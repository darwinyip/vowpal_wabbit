// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.

#include "vw/core/reductions/cb/cb_explore_adf_large_action_space.h"

#include "details/large_action_space.h"
#include "qr_decomposition.h"
#include "vw/common/random.h"
#include "vw/config/options.h"
#include "vw/core/gd_predict.h"
#include "vw/core/global_data.h"
#include "vw/core/label_dictionary.h"
#include "vw/core/label_parser.h"
#include "vw/core/model_utils.h"
#include "vw/core/parser.h"
#include "vw/core/reductions/cb/cb_adf.h"
#include "vw/core/reductions/cb/cb_explore.h"
#include "vw/core/reductions/cb/cb_explore_adf_common.h"
#include "vw/core/setup_base.h"
#include "vw/explore/explore.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
using namespace VW::cb_explore_adf;

namespace VW
{
namespace cb_explore_adf
{
class A_triplet_constructor
{
public:
  A_triplet_constructor(
      uint64_t weights_mask, uint64_t row_index, std::vector<Eigen::Triplet<float>>& triplets, uint64_t& max_col)
      : _weights_mask(weights_mask), _row_index(row_index), _triplets(triplets), _max_col(max_col)
  {
  }

  void set(float feature_value, uint64_t index)
  {
    if (feature_value != 0.f)
    {
      _triplets.emplace_back(Eigen::Triplet<float>(_row_index, index & _weights_mask, feature_value));
      if ((index & _weights_mask) > _max_col) { _max_col = (index & _weights_mask); }
    }
  }

private:
  uint64_t _weights_mask;
  uint64_t _row_index;
  std::vector<Eigen::Triplet<float>>& _triplets;
  uint64_t& _max_col;
};

bool _test_only_generate_A(VW::workspace* _all, const multi_ex& examples, std::vector<Eigen::Triplet<float>>& _triplets,
    Eigen::SparseMatrix<float>& _A)
{
  uint64_t row_index = 0;
  uint64_t max_non_zero_col = 0;
  _triplets.clear();
  for (auto* ex : examples)
  {
    assert(!VW::ec_is_example_header_cb(*ex));

    auto& red_features = ex->ex_reduction_features.template get<VW::large_action_space::las_reduction_features>();
    auto* shared_example = red_features.shared_example;
    if (shared_example != nullptr) { VW::details::truncate_example_namespaces_from_example(*ex, *shared_example); }

    if (_all->weights.sparse)
    {
      A_triplet_constructor w(_all->weights.sparse_weights.mask(), row_index, _triplets, max_non_zero_col);
      VW::foreach_feature<A_triplet_constructor, uint64_t, triplet_construction, sparse_parameters>(
          _all->weights.sparse_weights, _all->ignore_some_linear, _all->ignore_linear,
          (red_features.generated_interactions ? *red_features.generated_interactions : *ex->interactions),
          (red_features.generated_extent_interactions ? *red_features.generated_extent_interactions
                                                      : *ex->extent_interactions),
          _all->permutations, *ex, w, _all->generate_interactions_object_cache_state);
    }
    else
    {
      A_triplet_constructor w(_all->weights.dense_weights.mask(), row_index, _triplets, max_non_zero_col);

      VW::foreach_feature<A_triplet_constructor, uint64_t, triplet_construction, dense_parameters>(
          _all->weights.dense_weights, _all->ignore_some_linear, _all->ignore_linear,
          (red_features.generated_interactions ? *red_features.generated_interactions : *ex->interactions),
          (red_features.generated_extent_interactions ? *red_features.generated_extent_interactions
                                                      : *ex->extent_interactions),
          _all->permutations, *ex, w, _all->generate_interactions_object_cache_state);
    }

    if (shared_example != nullptr) { VW::details::append_example_namespaces_from_example(*ex, *shared_example); }

    row_index++;
  }

  assert(row_index == examples[0]->pred.a_s.size());
  if (max_non_zero_col == 0)
  {
    // no non-zero columns were found for A, it is empty
    _A.resize(0, 0);
  }
  else
  {
    _A.resize(row_index, max_non_zero_col + 1);
    _A.setZero();
    _A.setFromTriplets(_triplets.begin(), _triplets.end());
  }

  return (_A.cols() != 0 && _A.rows() != 0);
}

template <typename randomized_svd_impl, typename spanner_impl>
void cb_explore_adf_large_action_space<randomized_svd_impl, spanner_impl>::predict(
    VW::LEARNER::multi_learner& base, multi_ex& examples)
{
  predict_or_learn_impl<false>(base, examples);
}

template <typename randomized_svd_impl, typename spanner_impl>
void cb_explore_adf_large_action_space<randomized_svd_impl, spanner_impl>::learn(
    VW::LEARNER::multi_learner& base, multi_ex& examples)
{
  predict_or_learn_impl<true>(base, examples);
}

template <typename randomized_svd_impl, typename spanner_impl>
void cb_explore_adf_large_action_space<randomized_svd_impl, spanner_impl>::save_load(io_buf& io, bool read, bool text)
{
  if (io.num_files() == 0) { return; }

  if (read) { model_utils::read_model_field(io, _counter); }
  else { model_utils::write_model_field(io, _counter, "cb large action space storing example counter", text); }
}

template <typename randomized_svd_impl, typename spanner_impl>
void cb_explore_adf_large_action_space<randomized_svd_impl, spanner_impl>::randomized_SVD(const multi_ex& examples)
{
  impl.run(examples, shrink_factors, U, S, _V);
}

template <typename randomized_svd_impl, typename spanner_impl>
size_t cb_explore_adf_large_action_space<randomized_svd_impl, spanner_impl>::number_of_non_degenerate_singular_values()
{
  _non_degenerate_singular_values = 0;
  if (S.size() > 0)
  {
    // sum the singular values
    auto sum_of_sv = S.sum();

    // how many singular values represent 99% of the total sum of the singular values
    float current_sum_sv = 0;
    for (auto val : S)
    {
      _non_degenerate_singular_values++;
      current_sum_sv += val;
      if (current_sum_sv > 0.99f * sum_of_sv) { break; }
    }
  }

  return _non_degenerate_singular_values;
}

template <typename randomized_svd_impl, typename spanner_impl>
void cb_explore_adf_large_action_space<randomized_svd_impl, spanner_impl>::update_example_prediction(
    VW::multi_ex& examples)
{
  auto& preds = examples[0]->pred.a_s;

  if (_d < preds.size())
  {
    shrink_fact_config.calculate_shrink_factor(_counter, _d, preds, shrink_factors);
    randomized_SVD(examples);

    // The U matrix is empty before learning anything.
    if (U.rows() == 0)
    {
      // Set uniform random probability for empty U.
      const float prob = 1.0f / preds.size();
      for (auto& pred : preds) { pred.score = prob; }
      return;
    }

    auto non_degen = std::min(_d, static_cast<uint64_t>(number_of_non_degenerate_singular_values()));
    spanner_state.compute_spanner(U, non_degen, shrink_factors);

    assert(spanner_state.spanner_size() == preds.size());
  }
  else
  {
    // When the number of actions is not larger than d, all actions are selected.
    return;
  }

  // Keep only the actions in the spanner so they can be fed into the e-greedy or squarecb reductions.
  // Removed actions will be added back with zero probabilities in the cb_actions_mask reduction later

  auto best_action = preds[0].action;

  auto it = preds.begin();
  while (it != preds.end())
  {
    if (!spanner_state.is_action_in_spanner(it->action) && it->action != best_action) { it = preds.erase(it); }
    else { it++; }
  }
}

template <typename randomized_svd_impl, typename spanner_impl>
template <bool is_learn>
void cb_explore_adf_large_action_space<randomized_svd_impl, spanner_impl>::predict_or_learn_impl(
    VW::LEARNER::multi_learner& base, multi_ex& examples)
{
  if (is_learn)
  {
    base.learn(examples);
    if (base.learn_returns_prediction) { update_example_prediction(examples); }
    ++_counter;
  }
  else
  {
    base.predict(examples);
    update_example_prediction(examples);
  }
}

void generate_Z(const multi_ex& examples, Eigen::MatrixXf& Z, Eigen::MatrixXf& B, uint64_t d, uint64_t seed)
{
  // create Z matrix with dimenstions Kxd where K = examples.size()
  // Z = B * P where P is a dxd gaussian matrix

  uint64_t num_actions = examples[0]->pred.a_s.size();
  Z.resize(num_actions, d);
  Z.setZero();

  for (Eigen::Index row = 0; row < B.rows(); row++)
  {
    for (uint64_t col = 0; col < d; col++)
    {
      for (uint64_t inner_index = 0; inner_index < d; inner_index++)
      {
        auto combined_index = inner_index + col + seed;
        auto dot_prod_prod = B(row, inner_index) * VW::details::merand48_boxmuller(combined_index);
        Z(row, col) += dot_prod_prod;
      }
    }
  }
  VW::gram_schmidt(Z);
}

template <typename T, typename S>
cb_explore_adf_large_action_space<T, S>::cb_explore_adf_large_action_space(uint64_t d, float gamma_scale,
    float gamma_exponent, float c, bool apply_shrink_factor, VW::workspace* all, uint64_t seed, size_t total_size,
    size_t thread_pool_size, size_t block_size, bool use_explicit_simd, implementation_type impl_type)
    : _d(d)
    , _all(all)
    , _counter(0)
    , _seed(seed)
    , _impl_type(impl_type)
    , spanner_state(c, d)
    , shrink_fact_config(gamma_scale, gamma_exponent, apply_shrink_factor)
    , impl(all, d, _seed, total_size, thread_pool_size, block_size, use_explicit_simd)
{
}

shrink_factor_config::shrink_factor_config(float gamma_scale, float gamma_exponent, bool apply_shrink_factor)
    : _gamma_scale(gamma_scale), _gamma_exponent(gamma_exponent), _apply_shrink_factor(apply_shrink_factor)
{
}

void shrink_factor_config::calculate_shrink_factor(
    size_t counter, size_t max_actions, const VW::action_scores& preds, std::vector<float>& shrink_factors)
{
  if (_apply_shrink_factor)
  {
    shrink_factors.clear();
    float min_ck = std::min_element(preds.begin(), preds.end())->score;
    float gamma = _gamma_scale * static_cast<float>(std::pow(counter, _gamma_exponent));
    for (size_t i = 0; i < preds.size(); i++)
    {
      shrink_factors.push_back(std::sqrt(1 + max_actions + gamma / (4.0f * max_actions) * (preds[i].score - min_ck)));
    }
  }
  else { shrink_factors.resize(preds.size(), 1.f); }
}

template class cb_explore_adf_large_action_space<one_pass_svd_impl, one_rank_spanner_state>;
template class cb_explore_adf_large_action_space<two_pass_svd_impl, one_rank_spanner_state>;
}  // namespace cb_explore_adf
}  // namespace VW

template <typename T, typename S>
VW::LEARNER::base_learner* make_las_with_impl(VW::setup_base_i& stack_builder, VW::LEARNER::multi_learner* base,
    implementation_type& impl_type, VW::workspace& all, bool with_metrics, uint64_t d, float gamma_scale,
    float gamma_exponent, float c, bool apply_shrink_factor, size_t thread_pool_size, size_t block_size,
    bool use_explicit_simd)
{
  using explore_type = cb_explore_adf_base<cb_explore_adf_large_action_space<T, S>>;

  size_t problem_multiplier = 1;

  float seed = (all.get_random_state()->get_random() + 1) * 10.f;

  auto data = VW::make_unique<explore_type>(with_metrics, d, gamma_scale, gamma_exponent, c, apply_shrink_factor, &all,
      seed, 1 << all.num_bits, thread_pool_size, block_size, use_explicit_simd, impl_type);

  auto* l = make_reduction_learner(std::move(data), base, explore_type::learn, explore_type::predict,
      stack_builder.get_setupfn_name(VW::reductions::cb_explore_adf_large_action_space_setup))
                .set_input_label_type(VW::label_type_t::CB)
                .set_output_label_type(VW::label_type_t::CB)
                .set_input_prediction_type(VW::prediction_type_t::ACTION_SCORES)
                .set_output_prediction_type(VW::prediction_type_t::ACTION_SCORES)
                .set_params_per_weight(problem_multiplier)
                .set_output_example_prediction(explore_type::output_example_prediction)
                .set_update_stats(explore_type::update_stats)
                .set_print_update(explore_type::print_update)
                .set_persist_metrics(explore_type::persist_metrics)
                .set_save_load(explore_type::save_load)
                .set_learn_returns_prediction(base->learn_returns_prediction)
                .build();
  return make_base(*l);
}

VW::LEARNER::base_learner* VW::reductions::cb_explore_adf_large_action_space_setup(VW::setup_base_i& stack_builder)
{
  VW::config::options_i& options = *stack_builder.get_options();
  VW::workspace& all = *stack_builder.get_all_pointer();
  using config::make_option;
  bool cb_explore_adf_option = false;
  bool large_action_space = false;
  uint64_t d;
  float gamma_scale = 1.f;
  float gamma_exponent = 0.f;
  float c;
  bool apply_shrink_factor = false;
  bool use_two_pass_svd_impl = false;
  bool use_simd_in_one_pass_svd_impl = false;
  // leave some resources available in the case of few core's (for parser)
  uint64_t thread_pool_size = (std::thread::hardware_concurrency() - 1) / 2;
  uint64_t block_size = 0;

  config::option_group_definition new_options(
      "[Reduction] Experimental: Contextual Bandit Exploration with ADF with large action space filtering");
  new_options
      .add(make_option("cb_explore_adf", cb_explore_adf_option)
               .keep()
               .necessary()
               .help("Online explore-exploit for a contextual bandit problem with multiline action dependent features"))
      .add(make_option("large_action_space", large_action_space)
               .necessary()
               .keep()
               .help("Large action space filtering")
               .experimental())
      .add(make_option("max_actions", d)
               .keep()
               .allow_override()
               .default_value(20)
               .help("Max number of actions to hold")
               .experimental())
      .add(make_option("spanner_c", c)
               .keep()
               .allow_override()
               .default_value(2)
               .help("Parameter for computing c-approximate spanner")
               .experimental())
      .add(make_option("thread_pool_size", thread_pool_size)
               .help("Number of threads in the thread pool that will be used when running with one pass svd "
                     "implementation (default svd implementation option). Default thread pool size will be half of the "
                     "available hardware threads"))
      .add(make_option("block_size", block_size)
               .default_value(0)
               .help("Number of actions in a block to be scheduled for multithreading when using one pass svd "
                     "implementation (by default, block_size = num_actions / thread_pool_size)"))
      .add(make_option("las_hint_explicit_simd", use_simd_in_one_pass_svd_impl)
               .experimental()
               .help("Use explicit simd implementation in one pass svd. Only works with quadratic interactions. "
                     "(x86 Linux only)"))
      .add(make_option("two_pass_svd", use_two_pass_svd_impl)
               .experimental()
               .help("A more accurate svd that is much slower than the default (one pass svd)"));

  auto enabled = options.add_parse_and_check_necessary(new_options) && large_action_space;
  if (!enabled) { return nullptr; }

  if (options.was_supplied("squarecb"))
  {
    apply_shrink_factor = true;
    gamma_scale = options.get_typed_option<float>("gamma_scale").value();
    gamma_exponent = options.get_typed_option<float>("gamma_exponent").value();
  }

  if (options.was_supplied("cb_type"))
  {
    auto cb_type = options.get_typed_option<std::string>("cb_type").value();
    if (cb_type != "mtr")
    {
      all.logger.err_warn(
          "Only cb_type 'mtr' is currently supported with large action spaces, resetting to mtr. Input was: '{}'",
          cb_type);
      options.replace("cb_type", "mtr");
    }
  }

  VW::LEARNER::multi_learner* base = as_multiline(stack_builder.setup_base_learner());
  all.example_parser->lbl_parser = VW::cb_label_parser_global;

  if (use_two_pass_svd_impl)
  {
    auto impl_type = implementation_type::two_pass_svd;
    return make_las_with_impl<two_pass_svd_impl, one_rank_spanner_state>(stack_builder, base, impl_type, all,
        all.global_metrics.are_metrics_enabled(), d, gamma_scale, gamma_exponent, c, apply_shrink_factor,
        thread_pool_size, block_size,
        /*use_explicit_simd=*/false);
  }
  else
  {
    auto impl_type = implementation_type::one_pass_svd;
    return make_las_with_impl<one_pass_svd_impl, one_rank_spanner_state>(stack_builder, base, impl_type, all,
        all.global_metrics.are_metrics_enabled(), d, gamma_scale, gamma_exponent, c, apply_shrink_factor,
        thread_pool_size, block_size, use_simd_in_one_pass_svd_impl);
  }
}
