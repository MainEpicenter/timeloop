/* Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
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

#include <fstream>
#include <iomanip>
#include <iostream>

#include "compound-config/compound-config.hpp"

class ProblemSpaceNode
{
 public:
  std::string name_; //descriptive name
  YAML::Node yaml_; //text version of YAML

  ProblemSpaceNode() {}  
  ProblemSpaceNode(std::string n, YAML::Node p) : name_(n), yaml_(p) {}
};


class ProblemSpace
{
 protected:
  std::string name_;

  std::vector<ProblemSpaceNode> problems_;

 public:

  ProblemSpace() {}
  ProblemSpace(std::string n) : name_(n) {}

  
  void InitializeFromFile(std::string filename)
  {    
    std::ifstream fin;
    fin.open(filename);
    YAML::Node filecontents = YAML::Load(fin);

    ProblemSpaceNode new_problem = ProblemSpaceNode(filename, filecontents);
    problems_.push_back(new_problem);
  }

  void InitializeFromFileList(YAML::Node list_yaml)
  {    
    //traverse list, create new nodes and push_back
    for (std::size_t i = 0; i < list_yaml.size(); i++)
    {
      std::string filename = list_yaml[i].as<std::string>();

      std::ifstream fin;
      fin.open(filename);
      YAML::Node filecontents = YAML::Load(fin);
      std::cout << "Configuring YAML : " << filename << std::endl;
      std::cout << "  contents : " << filecontents << std::endl;
 
      ProblemSpaceNode new_problem = ProblemSpaceNode(filename, filecontents);
      problems_.push_back(new_problem);
    }
  }

  int GetSize() { return problems_.size(); } 

  ProblemSpaceNode& GetNode(int index) { return problems_[index]; }

};
