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

#include <cassert>
#include <string>

#include "model/topology.hpp"

namespace model
{

std::ostream& operator<<(std::ostream& out, const Topology& topology)
{
  int level_id = 0;
  for (auto & level : topology.levels_)
  {
    out << "Level " << level_id << std::endl;
    out << "-------" << std::endl;
    level->Print(out);
    level_id++;
  }

  if (topology.is_evaluated_)
  {
    out << "Total topology energy: " << topology.Energy() << " pJ" << std::endl;
    out << "Total topology area: " << topology.Area() << " um^2" << std::endl;
    out << "Max topology cycles: " << topology.Cycles() << std::endl;
  }

  return out;
}

void Topology::Spec(const Topology::Specs& specs)
{
  specs_ = specs;
  for (auto& level : levels_)
  {
    level.reset();
  }
  levels_.clear();

  for (unsigned i = 0; i < specs.NumLevels(); i++)
  {
    auto level_specs = specs.GetLevel(i);
      
    // What type of level is this?
    if (level_specs->Type() == "BufferLevel")
    {
      BufferLevel::Specs& specs = *std::static_pointer_cast<BufferLevel::Specs>(level_specs);
      std::shared_ptr<BufferLevel> buffer_level = std::make_shared<BufferLevel>(specs);
      std::shared_ptr<Level> level = std::static_pointer_cast<Level>(buffer_level);
      levels_.push_back(level);
    }
    else if (level_specs->Type() == "ArithmeticUnits")
    {
      ArithmeticUnits::Specs& specs = *std::static_pointer_cast<ArithmeticUnits::Specs>(level_specs);
      std::shared_ptr<ArithmeticUnits> arithmetic_level = std::make_shared<ArithmeticUnits>(specs);
      std::shared_ptr<Level> level = std::static_pointer_cast<Level>(arithmetic_level);
      levels_.push_back(level);
    }
    else
    {
      std::cerr << "ERROR: illegal level specs type: " << level_specs->Type() << std::endl;
      exit(1);
    }
  }

  is_specced_ = true;
}

// The hierarchical ParseSpecs functions are static and do not
// affect the internal specs_ data structure, which is set by
// the dynamic Spec() call later.

// This function implements the "classic" hierarchical topology
// with arithmetic units at level 0 and storage units at level 1+.
Topology::Specs Topology::ParseSpecs(libconfig::Setting& storage,
                                     libconfig::Setting& arithmetic)
{
  Specs specs;
  
  assert(storage.isList());

  // Level 0: arithmetic.
  auto level_specs_p = std::make_shared<ArithmeticUnits::Specs>(ArithmeticUnits::ParseSpecs(arithmetic));
  specs.AddLevel(0, std::static_pointer_cast<LevelSpecs>(level_specs_p));

  // Storage levels.
  int num_storage_levels = storage.getLength();
  for (int i = 0; i < num_storage_levels; i++)
  {
    libconfig::Setting& level = storage[i];
    auto level_specs_p = std::make_shared<BufferLevel::Specs>(BufferLevel::ParseSpecs(level));
    specs.AddLevel(i, std::static_pointer_cast<LevelSpecs>(level_specs_p));
  }

  Validate(specs);

  return specs;
}

// Make sure the topology is consistent,
// and update unspecified parameters if they can
// be inferred from other specified parameters.
void Topology::Validate(Topology::Specs& specs)
{
  // Intra-level topology validation is carried out by the levels
  // themselves. We take care of inter-layer issues here. This
  // breaks abstraction since we will be poking at levels' private
  // specs. FIXME.

  // Assumption here is that level i always connects to level
  // i-1 via a 1:1 or fanout network. The network module will
  // eventually be factored out, at which point we can make the
  // interconnection more generic and specifiable.

  BufferLevel::Specs& inner = *specs.GetStorageLevel(0);
  ArithmeticUnits::Specs& arithmetic_specs = *specs.GetArithmeticLevel();
  unsigned inner_start_pvi, inner_end_pvi;
  if (inner.sharing_type == BufferLevel::DataTypeSharing::Shared)
  {
    inner_start_pvi = inner_end_pvi = unsigned(problem::DataType::Num);
  }
  else
  {
    inner_start_pvi = 0;
    inner_end_pvi = unsigned(problem::DataType::Num) - 1;
  }
  auto inner_start_pv = problem::DataType(inner_start_pvi);

  if (inner.Instances(inner_start_pv).Get() == arithmetic_specs.Instances().Get())
  {
    for (unsigned pvi = inner_start_pvi; pvi <= inner_end_pvi; pvi++)
    {
      inner.FanoutX(problem::DataType(pvi)) = 1;
      inner.FanoutY(problem::DataType(pvi)) = 1;
      inner.Fanout(problem::DataType(pvi)) = 1;
    }
  }
  else
  {
    // fanout
    assert(arithmetic_specs.Instances().Get() % inner.Instances(inner_start_pv).Get() == 0);
    unsigned fanout_in = arithmetic_specs.Instances().Get() / inner.Instances(inner_start_pv).Get();
    for (unsigned pvi = inner_start_pvi; pvi <= inner_end_pvi; pvi++)
      inner.Fanout(problem::DataType(pvi)) = fanout_in;
    // fanout x
    assert(arithmetic_specs.MeshX().IsSpecified());
    assert(arithmetic_specs.MeshX().Get() % inner.MeshX(inner_start_pv).Get() == 0);
    unsigned fanoutX_in = arithmetic_specs.MeshX().Get() / inner.MeshX(inner_start_pv).Get();
    for (unsigned pvi = inner_start_pvi; pvi <= inner_end_pvi; pvi++)
      inner.FanoutX(problem::DataType(pvi)) = fanoutX_in;
    // fanout y
    assert(arithmetic_specs.MeshY().IsSpecified());
    assert(arithmetic_specs.MeshY().Get() % inner.MeshY(inner_start_pv).Get() == 0);
    unsigned fanoutY_in = arithmetic_specs.MeshY().Get() / inner.MeshY(inner_start_pv).Get();
    for (unsigned pvi = inner_start_pvi; pvi <= inner_end_pvi; pvi++)
      inner.FanoutY(problem::DataType(pvi)) = fanoutY_in;
  }

  for (unsigned i = 0; i < specs.NumStorageLevels()-1; i++)
  {
    BufferLevel::Specs& inner = *specs.GetStorageLevel(i);
    BufferLevel::Specs& outer = *specs.GetStorageLevel(i+1);

    // FIXME: for partitioned levels, we're only going to look at the
    // pvi==0 partition. Our buffer.cpp's ParseSpecs function guarantees
    // that all partitions will have identical specs anyway.
    // HOWEVER, if we're deriving any specs, we need to set them for all
    // pvs for partitioned buffers.

    // All of this
    // will go away once we properly separate out partitions from
    // datatypes.
    unsigned inner_start_pvi, inner_end_pvi;
    if (inner.sharing_type == BufferLevel::DataTypeSharing::Shared)
    {
      inner_start_pvi = inner_end_pvi = unsigned(problem::DataType::Num);
    }
    else
    {
      inner_start_pvi = 0;
      inner_end_pvi = unsigned(problem::DataType::Num) - 1;
    }
    auto inner_start_pv = problem::DataType(inner_start_pvi);
    
    unsigned outer_start_pvi, outer_end_pvi;
    if (outer.sharing_type == BufferLevel::DataTypeSharing::Shared)
    {
      outer_start_pvi = outer_end_pvi = unsigned(problem::DataType::Num);
    }
    else
    {
      outer_start_pvi = 0;
      outer_end_pvi = unsigned(problem::DataType::Num) - 1;
    }
    auto outer_start_pv = problem::DataType(outer_start_pvi);
    
    assert(inner.Instances(inner_start_pv).Get() % outer.Instances(outer_start_pv).Get() == 0);
    unsigned fanout = inner.Instances(inner_start_pv).Get() / outer.Instances(outer_start_pv).Get();
    if (outer.Fanout(outer_start_pv).IsSpecified())
      assert(outer.Fanout(outer_start_pv).Get() == fanout);
    else
    {
      for (unsigned pvi = outer_start_pvi; pvi <= outer_end_pvi; pvi++)
        outer.Fanout(problem::DataType(pvi)) = fanout;
    }

    assert(inner.MeshX(inner_start_pv).Get() % outer.MeshX(outer_start_pv).Get() == 0);
    unsigned fanoutX = inner.MeshX(inner_start_pv).Get() / outer.MeshX(outer_start_pv).Get();
    if (outer.FanoutX(outer_start_pv).IsSpecified())
      assert(outer.FanoutX(outer_start_pv).Get() == fanoutX);
    else
    {
      for (unsigned pvi = outer_start_pvi; pvi <= outer_end_pvi; pvi++)
        outer.FanoutX(problem::DataType(pvi)) = fanoutX;
    }

    assert(inner.MeshY(inner_start_pv).Get() % outer.MeshY(outer_start_pv).Get() == 0);
    unsigned fanoutY = inner.MeshY(inner_start_pv).Get() / outer.MeshY(outer_start_pv).Get();
    if (outer.FanoutY(outer_start_pv).IsSpecified())
      assert(outer.FanoutY(outer_start_pv).Get() == fanoutY);
    else
    {
      for (unsigned pvi = outer_start_pvi; pvi <= outer_end_pvi; pvi++)
        outer.FanoutY(problem::DataType(pvi)) = fanoutY;
    }

    assert(outer.Fanout(outer_start_pv).Get() ==
           outer.FanoutX(outer_start_pv).Get() * outer.FanoutY(outer_start_pv).Get());
  }  
}

//
// Level accessors.
//

// Specs.
unsigned Topology::Specs::NumLevels() const
{
  return levels.size();
}

unsigned Topology::Specs::NumStorageLevels() const
{
  return storage_map.size();
}

std::shared_ptr<LevelSpecs> Topology::Specs::GetLevel(unsigned level_id) const
{
  return levels.at(level_id);
}

std::shared_ptr<BufferLevel::Specs> Topology::Specs::GetStorageLevel(unsigned storage_level_id) const
{
  auto level_id = storage_map.at(storage_level_id);
  return std::static_pointer_cast<BufferLevel::Specs>(levels.at(level_id));
}

std::shared_ptr<ArithmeticUnits::Specs> Topology::Specs::GetArithmeticLevel() const
{
  auto level_id = arithmetic_map;
  return std::static_pointer_cast<ArithmeticUnits::Specs>(levels.at(level_id));
}

// Topology class.
unsigned Topology::NumLevels() const
{
  assert(is_specced_);
  return levels_.size();
}

unsigned Topology::NumStorageLevels() const
{
  assert(is_specced_);
  return specs_.NumStorageLevels();
}

std::shared_ptr<Level> Topology::GetLevel(unsigned level_id) const
{
  return levels_.at(level_id);
}

std::shared_ptr<BufferLevel> Topology::GetStorageLevel(unsigned storage_level_id) const
{
  auto level_id = specs_.StorageMap(storage_level_id);
  return std::static_pointer_cast<BufferLevel>(levels_.at(level_id));
}

std::shared_ptr<ArithmeticUnits> Topology::GetArithmeticLevel() const
{
  auto level_id = specs_.ArithmeticMap();
  return std::static_pointer_cast<ArithmeticUnits>(levels_.at(level_id));
}


// PreEvaluationCheck(): allows for a very fast capacity-check
// based on given working-set sizes that can be trivially derived
// by the caller. The more powerful Evaluate() function also
// performs these checks, but computes both tile sizes and access counts
// and requires full tiling data that is generated by a very slow
// Nest::ComputeWorkingSets() algorithm. The PreEvaluationCheck()
// function is an optional call that extensive design-space searches
// can use to fail early.
// FIXME: integrate with Evaluate() and re-factor.
// FIXME: what about instances and fanout checks?
bool Topology::PreEvaluationCheck(const Mapping& mapping, analysis::NestAnalysis* analysis)
{
  auto masks = tiling::TransposeMasks(mapping.datatype_bypass_nest);
  auto working_set_sizes = analysis->GetWorkingSetSizes_LTW();

  bool success = true;
  for (unsigned storage_level = 0; storage_level < NumStorageLevels(); storage_level++)
  {
    success &= GetStorageLevel(storage_level)->PreEvaluationCheck(
      working_set_sizes.at(storage_level), masks.at(storage_level));
    if (!success)
    {
      break;
    }
  }
  return success;
}

bool Topology::Evaluate(Mapping& mapping, analysis::NestAnalysis* analysis,
                        const problem::WorkloadConfig& workload_config)
{
  assert(is_specced_);

  bool success = true;
  
  // Compute working-set tile hierarchy for the nest.
  auto ws_tiles = analysis->GetWorkingSets();

  // Ugh... FIXME.
  auto compute_cycles = analysis->GetBodyInfo().accesses;

  // Create a mask indicating which levels support distributed multicast.
  tiling::CompoundMaskNest distribution_supported;
  for (unsigned pv = 0; pv < unsigned(problem::DataType::Num); pv++)
  {
    distribution_supported[pv].reset();
    for (unsigned storage_level = 0; storage_level < NumStorageLevels(); storage_level++)
    {
      if (GetStorageLevel(storage_level)->DistributedMulticastSupported())
      {
        distribution_supported[pv].set(storage_level);
      }
    }
  }
  
  // Collapse tiles into a specified number of tiling levels. The solutions are
  // received in a set of per-problem::DataType arrays.
  auto collapsed_tiles = tiling::CollapseTiles(ws_tiles, specs_.NumStorageLevels(),
                                               mapping.datatype_bypass_nest,
                                               distribution_supported);

  // Transpose the tiles into level->datatype structure.
  auto tiles = tiling::TransposeTiles(collapsed_tiles);
  assert(tiles.size() == NumStorageLevels());

  // Transpose the datatype bypass nest into level->datatype structure.
  auto keep_masks = tiling::TransposeMasks(mapping.datatype_bypass_nest);
  assert(keep_masks.size() >= NumStorageLevels());
  
  // Area of all the compute + buffer elements in inner levels
  // (needed for wire energy calculation).
  // FIXME: Breaks abstraction by making assumptions about arithmetic
  // (multiplier) organization and querying multiplier area.
  double inner_tile_area = GetArithmeticLevel()->AreaPerInstance();

  for (unsigned storage_level_id = 0; storage_level_id < NumStorageLevels(); storage_level_id++)
  {
    auto storage_level = GetStorageLevel(storage_level_id);
    
    // Evaluate Loop Nest on hardware structures: calculate
    // primary statistics.
    success &= storage_level->Evaluate(tiles[storage_level_id],
                                       keep_masks[storage_level_id],
                                       inner_tile_area,
                                       compute_cycles);
    if (!success)
      break;
    
    // The inner tile area is the area of the local sub-level that I will
    // send data to. Note that it isn't the area of the entire sub-level
    // because I may only have reach into a part of the level, which will
    // reduce my wire energy costs. To determine this, we use the fanout
    // from this level inwards.
    // FIXME: We need a better model.
    double cur_level_area = storage_level->AreaPerInstance();
    inner_tile_area = cur_level_area + (inner_tile_area * storage_level->MaxFanout());
  }
  
  success &= GetArithmeticLevel()->HackEvaluate(analysis, workload_config);

  if (success)
    is_evaluated_ = true;

  return success;
}

double Topology::Energy() const
{
  double energy = 0;
  for (auto level : levels_)
  {
    assert(level->Energy() >= 0);
    energy += level->Energy();
  }  
  return energy;
}

double Topology::Area() const
{
  double area = 0;
  for (auto level : levels_)
  {
    assert(level->Area() >= 0);
    area += level->Area();
  }
  return area;
}

std::uint64_t Topology::Cycles() const
{
  std::uint64_t cycles = 0;
  for (auto level : levels_)
  {
    cycles = std::max(cycles, level->Cycles());
  }
  return cycles;
}

double Topology::Utilization() const
{
  // FIXME.
  return (GetArithmeticLevel()->IdealCycles() / Cycles());
}

std::uint64_t Topology::MACCs() const
{
  return GetArithmeticLevel()->MACCs();
}

}  // namespace model
