/* Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <iterator>
#include <unordered_set>
#include <fstream>
#include <iostream>

#include "mapping/mapping.hpp"
#include "mapspaces/mapspace-base.hpp"
#include "util/misc.hpp"
#include "search/search.hpp"

namespace search
{

class LinearPrunedSearch : public SearchAlgorithm
{
 private:
  enum class State
  {
    Ready,
    WaitingForStatus,
    Terminated
  };
  
 private:
  // Config.
  mapspace::MapSpace* mapspace_;
  unsigned id_;

  // Live state.
  State state_;
  std::array<uint128_t, unsigned(mapspace::Dimension::Num)> iterator_;
  uint128_t valid_mappings_;
  std::uint64_t eval_fail_count_;

  double best_cost_;
  std::ofstream best_cost_file_;

 public:
  LinearPrunedSearch(libconfig::Setting& config, mapspace::MapSpace* mapspace, unsigned id) :
      SearchAlgorithm(),
      mapspace_(mapspace),
      id_(id),
      state_(State::Ready),
      valid_mappings_(0),
      eval_fail_count_(0),
      best_cost_(0)
  {
    (void) config;
    
    for (unsigned i = 0; i < unsigned(mapspace::Dimension::Num); i++)
    {
      iterator_[i] = 0;
    }

    // Prune the mapspace for the first time.
    mapspace_->InitPruned(0);

#ifdef DUMP_COSTS
    // Dump best cost for each index factorization.
    best_cost_file_.open("/tmp/timeloop-if-cost.txt");
#endif
  }

  ~LinearPrunedSearch()
  {
    best_cost_file_.close();
  }

  // Order:
  //   DatatypeBypass <- Spatial <- LoopPermutation <- IndexFactorization.
  std::vector<mapspace::Dimension> dim_order_ =
  {
    mapspace::Dimension::DatatypeBypass,
    mapspace::Dimension::Spatial,
    mapspace::Dimension::LoopPermutation,
    mapspace::Dimension::IndexFactorization
  };
  
  bool IncrementRecursive_(int position = 0)
  {
    auto dim = dim_order_[position];
    if (iterator_[unsigned(dim)] < mapspace_->Size(dim) - 1)
    {
      // Move to next integer in this mapspace dimension.
      iterator_[unsigned(dim)]++;
      if (dim == mapspace::Dimension::IndexFactorization)
      {
        // We just changed the index factorization. Prune the sub-mapspace
        // for this specific factorization index.
        mapspace_->InitPruned(iterator_[unsigned(dim)]);

#ifdef DUMP_COSTS
        // Dump the best cost observed for this index factorization.
        // Note: best_cost_ == 0 implies this was a bad index factorization
        // that failed mapping. We can choose to not report these, or
        // grep them out in post-processing.
        best_cost_file_ << best_cost_ << std::endl;
#endif
        
        // Reset the best cost.
        best_cost_ = 0;
      }
      return true;
    }
    // Carry over to next higher-order mapspace dimension.
    else if (position < int(mapspace::Dimension::Num) - 1)
    {
      iterator_[unsigned(dim)] = 0;
      return IncrementRecursive_(position + 1);
    }
    else
    {
      // Overflow! We are done.
      return false;
    }
  }

  bool Next(mapspace::ID& mapping_id)
  {
    if (state_ == State::Terminated)
    {
      return false;
    }

    assert(state_ == State::Ready);

    mapping_id = mapspace::ID(mapspace_->AllSizes());
    for (unsigned i = 0; i < unsigned(mapspace::Dimension::Num); i++)
    {
      mapping_id.Set(i, iterator_[i]);
    }
    
    state_ = State::WaitingForStatus;
    
    return true;
  }

  void Report(Status status, double cost = 0)
  {
    assert(state_ == State::WaitingForStatus);

    if (status == Status::Success)
    {
      valid_mappings_++;

      if (best_cost_ == 0)
        best_cost_ = cost;
      else
        best_cost_ = std::min(best_cost_, cost);
    }
    else if (status == Status::MappingConstructionFailure)
    {
      // Accelerate search by invalidating bad spaces.
      // ConstructMapping failure =>
      //   Combination of (IF, LP, S) is bad.
      //   DB don't care.
    }
    else if (status == Status::EvalFailure)
    {
      // PreEval/Eval failure (capacity) =>
      //   Combination of (IF, DB) is bad.
      //   If all DBs cause Eval failure for an IF, then that IF is bad,
      //   no need to look at other LP, S combinations.
      eval_fail_count_++;
    }

    if (iterator_[unsigned(mapspace::Dimension::DatatypeBypass)] ==
        (mapspace_->Size(mapspace::Dimension::DatatypeBypass) - 1))
    {
      if (eval_fail_count_ == mapspace_->Size(mapspace::Dimension::DatatypeBypass))
      {
        // All DBs failed eval for this combination of IF*LP*S. This means
        // this IF is bad. Skip to the next IF by fast-forwarding to the end of
        // this IF.
        iterator_[unsigned(mapspace::Dimension::Spatial)] =
          mapspace_->Size(mapspace::Dimension::Spatial) - 1;
        iterator_[unsigned(mapspace::Dimension::LoopPermutation)] =
          mapspace_->Size(mapspace::Dimension::LoopPermutation) - 1;
      }
      eval_fail_count_ = 0;
    }

    bool mapspace_remaining = IncrementRecursive_();

    if (mapspace_remaining) //  && valid_mappings_ < search_size_)
    {
       state_ = State::Ready;
    }
    else
    {
      state_ = State::Terminated;
    }
  }
};

} // namespace search
