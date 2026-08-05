//===- Passes/StaticBranchProbabilities.cpp - Infered Branch Probabilities -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/StaticBranchProbabilities.h"
#include "bolt/Passes/DataflowInfoManager.h"
#include "bolt/Passes/RegAnalysis.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/Error.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
// #include "llvm/Support/Options.h"

#undef DEBUG_TYPE
#define DEBUG_TYPE "bolt-branch-prob"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory InferenceCategory;


cl::opt<bool> MLBased("ml-based",
                      cl::desc("reads probabilities based on ML model."),
                      cl::ZeroOrMore, cl::cat(InferenceCategory));
} // namespace opts

namespace llvm {
namespace bolt {

double
StaticBranchProbabilities::getCFGBackEdgeProbability(BinaryBasicBlock &SrcBB,
                                                     BinaryBasicBlock &DstBB) {
  Edge CFGEdge = std::make_pair(SrcBB.getLabel(), DstBB.getLabel());
  auto It = CFGBackEdgeProbabilities.find(CFGEdge);
  if (It != CFGBackEdgeProbabilities.end()) {
    if (static_cast<int64_t>(It->second) < 0 ||
        static_cast<int64_t>(It->second) == INT64_MAX)
      return 0.0;
    return It->second;
  }

  auto Function = SrcBB.getFunction();
  return getCFGEdgeProbability(CFGEdge, Function);
}

void StaticBranchProbabilities::setCFGBackEdgeProbability(Edge &CFGEdge,
                                                          double Prob) {
  if (static_cast<int64_t>(Prob) < 0 || static_cast<int64_t>(Prob) == INT64_MAX)
    CFGBackEdgeProbabilities[CFGEdge] = 0.0;
  CFGBackEdgeProbabilities[CFGEdge] = Prob;
}

double
StaticBranchProbabilities::getCFGEdgeProbability(Edge &CFGEdge,
                                                 BinaryFunction *Function) {
 
  auto *BB = Function->getBasicBlockForLabel(CFGEdge.first);
  auto &BC = Function->getBinaryContext();
  auto LastInst = BB->getLastNonPseudoInstr();
 
  auto It = CFGEdgeProbabilities.find(CFGEdge);
  if (It != CFGEdgeProbabilities.end()) {
    if (static_cast<int64_t>(It->second) < 0 ||
        static_cast<int64_t>(It->second) == INT64_MAX){
          return 0;
        }
    return It->second;
  }

  if (LastInst && BC.MIB->isConditionalBranch(*LastInst)){	  
      return 0.5;
  }

  return 1.0;
}

double
StaticBranchProbabilities::getCFGEdgeProbability(BinaryBasicBlock &SrcBB,
                                                 BinaryBasicBlock &DstBB) {
  Edge CFGEdge = std::make_pair(SrcBB.getLabel(), DstBB.getLabel());

  auto Function = SrcBB.getFunction();

  return getCFGEdgeProbability(CFGEdge, Function);
}

int64_t 
StaticBranchProbabilities::getFunctionFrequency(uint64_t FunAddress){
  auto It = OriginalFunctionsFrequency.find(FunAddress);
  if (It != OriginalFunctionsFrequency.end()) {
    return It->second;
  }
  return 1;
}

void StaticBranchProbabilities::clear() {
  BSI->clear();
  CFGBackEdgeProbabilities.clear();
  CFGEdgeProbabilities.clear();
}

void StaticBranchProbabilities::parseProbabilitiesFile(
    std::unique_ptr<MemoryBuffer> MemBuf, BinaryContext &BC) {
  errs() << "BOLT-INFO: Starting passing.\n";
  std::vector<BasicBlockOffset> BasicBlockOffsets;
  auto populateBasicBlockOffsets =
      [&](BinaryFunction &Function,
          std::vector<BasicBlockOffset> &BasicBlockOffsets) {
        for (auto &BB : Function) {
          BasicBlockOffsets.emplace_back(
              std::make_pair(BB.getInputOffset(), &BB));
        }
      };

  auto getBasicBlockAtOffset = [&](uint64_t Offset) -> BinaryBasicBlock * {
    if (BasicBlockOffsets.empty())
      return nullptr;

    auto It = std::upper_bound(
        BasicBlockOffsets.begin(), BasicBlockOffsets.end(),
        BasicBlockOffset(Offset, nullptr), CompareBasicBlockOffsets());
    assert(It != BasicBlockOffsets.begin() &&
           "first basic block not at offset 0");
    --It;
    auto *BB = It->second;
    return (Offset == BB->getInputOffset()) ? BB : nullptr;
  };

  auto ParsingBuf = MemBuf.get()->getBuffer();
  BinaryFunction *Function = nullptr;
  while (ParsingBuf.size() > 0) {
    auto LineEnd = ParsingBuf.find_first_of("\n");
    if (LineEnd == StringRef::npos) {
      errs() << "BOLT-ERROR: File not in the correct format.\n";
      exit(EXIT_FAILURE);
    }

    StringRef Line = ParsingBuf.substr(0, LineEnd);
    auto Type = Line.split(" ");
    if (!Type.first.equals("EDGE") && !Type.first.equals("FUNCTION") &&
        !Line.equals("END")) {
      errs() << "BOLT-ERROR: File not in the correct format, found: " << Line
             << "\n";
      exit(EXIT_FAILURE);
    }

    if (Type.first.equals("FUNCTION")) {
      clear();
      BasicBlockOffsets.clear();
      auto FunLine = Type.second.split(" ");
      //the first substring is the function's name
      StringRef NumStr = FunLine.second.split(" ").first;
      uint64_t FunctionAddress;
      if (NumStr.getAsInteger(16, FunctionAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      Function = BC.getBinaryFunctionAtAddress(FunctionAddress);
      if (Function){
        populateBasicBlockOffsets(*Function, BasicBlockOffsets);
      }
    }

    if (!Function){
      ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
      continue;
    }
    
    if (Type.first.equals("EDGE")) {
      auto EdgeLine = Type.second.split(" ");

      StringRef SrcBBAddressStr = EdgeLine.first;
      uint64_t SrcBBAddress;
      if (SrcBBAddressStr.getAsInteger(16, SrcBBAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      auto EdgeInfo = EdgeLine.second.split(" ");
      StringRef DstBBAddressStr = EdgeInfo.first;
      uint64_t DstBBAddress;
      if (DstBBAddressStr.getAsInteger(16, DstBBAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      auto SrcBB = getBasicBlockAtOffset(SrcBBAddress);
      auto DstBB = getBasicBlockAtOffset(DstBBAddress);
      if (SrcBB && DstBB) {
        uint64_t Prob;
        StringRef ProbStr = EdgeInfo.second;
        if (ProbStr.getAsInteger(10, Prob)) {
          errs() << "BOLT-ERROR: File not in the correct format.\n";
          exit(EXIT_FAILURE);
        }
        SrcBB->setSuccessorBranchInfo(*DstBB, Prob, 0);
      }
    } else if (Line.equals("END")) {
      BasicBlockOffsets.clear();
      Function->setExecutionCount(1);
    }
    
    ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
  }
}

void StaticBranchProbabilities::parseProbabilitiesFile(
    std::unique_ptr<MemoryBuffer> MemBuf, BinaryContext &BC,
    std::set<uint64_t> &StaleFunctionsAddresses,
    std::set<uint64_t> &MatchedStaleFuncAddresses) {

  std::vector<BasicBlockOffset> BasicBlockOffsets;
  auto populateBasicBlockOffsets =
      [&](BinaryFunction &Function,
          std::vector<BasicBlockOffset> &BasicBlockOffsets) {
        for (auto &BB : Function) {
          BasicBlockOffsets.emplace_back(
              std::make_pair(BB.getInputOffset(), &BB));
        }
      };

  auto getBasicBlockAtOffset = [&](uint64_t Offset) -> BinaryBasicBlock * {
    if (BasicBlockOffsets.empty())
      return nullptr;

    auto It = std::upper_bound(
        BasicBlockOffsets.begin(), BasicBlockOffsets.end(),
        BasicBlockOffset(Offset, nullptr), CompareBasicBlockOffsets());
    assert(It != BasicBlockOffsets.begin() &&
           "first basic block not at offset 0");
    --It;
    auto *BB = It->second;
    return (Offset == BB->getInputOffset()) ? BB : nullptr;
  };

  auto ParsingBuf = MemBuf.get()->getBuffer();
  BinaryFunction *Function = nullptr;
  while (ParsingBuf.size() > 0) {
    auto LineEnd = ParsingBuf.find_first_of("\n");
    if (LineEnd == StringRef::npos) {
      errs() << "BOLT-ERROR: File not in the correct format.\n";
      exit(EXIT_FAILURE);
    }

    StringRef Line = ParsingBuf.substr(0, LineEnd);
    auto Type = Line.split(" ");
    if (!Type.first.equals("EDGE") && !Type.first.equals("FUNCTION") &&
        !Line.equals("END")) {
      errs() << "BOLT-ERROR: File not in the correct format, found: " << Line
             << "\n";
      exit(EXIT_FAILURE);
    }

    if (Type.first.equals("FUNCTION")) {
      clear();
      BasicBlockOffsets.clear();
      auto FunLine = Type.second.split(" ");
      // the first substring is the function's name
      StringRef NumStr = FunLine.second.split(" ").first;
      uint64_t FunctionAddress = 0;
      if (NumStr.getAsInteger(16, FunctionAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      auto it = StaleFunctionsAddresses.find(FunctionAddress);
      if (it != StaleFunctionsAddresses.end()) {
        Function = BC.getBinaryFunctionAtAddress(FunctionAddress);
        if(Function){
          Function->clearProfile();
          Function->setExecutionCount(1);
          Function->markProfiled(BinaryFunction::PF_STATIC);
          MatchedStaleFuncAddresses.insert(FunctionAddress);
          populateBasicBlockOffsets(*Function, BasicBlockOffsets);
        }
      } 
    }

    if (!Function){
      ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
      continue;
    }
    
    if (Type.first.equals("EDGE")) {
      auto EdgeLine = Type.second.split(" ");

      StringRef SrcBBAddressStr = EdgeLine.first;
      uint64_t SrcBBAddress;
      if (SrcBBAddressStr.getAsInteger(16, SrcBBAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      auto EdgeInfo = EdgeLine.second.split(" ");
      StringRef DstBBAddressStr = EdgeInfo.first;
      uint64_t DstBBAddress;
      if (DstBBAddressStr.getAsInteger(16, DstBBAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      auto SrcBB = getBasicBlockAtOffset(SrcBBAddress);
      auto DstBB = getBasicBlockAtOffset(DstBBAddress);
      if (SrcBB && DstBB && SrcBB->isSuccessor(DstBB)) {
        uint64_t Prob;
        StringRef ProbStr = EdgeInfo.second;
        if (ProbStr.getAsInteger(10, Prob)) {
          errs() << "BOLT-ERROR: File not in the correct format.\n";
          exit(EXIT_FAILURE);
        }

        SrcBB->setSuccessorBranchInfo(*DstBB, Prob, 0);
      }
    } else if (Line.equals("END")) {
      BasicBlockOffsets.clear();
    }
    
    ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
  }
}

void StaticBranchProbabilities::parseBeetleProfileFile(
    std::unique_ptr<MemoryBuffer> MemBuf, BinaryContext &BC,
    std::set<uint64_t> &StaleFunctionsAddresses) {

  std::vector<BasicBlockOffset> BasicBlockOffsets;
  auto populateBasicBlockOffsets =
      [&](BinaryFunction &Function,
          std::vector<BasicBlockOffset> &BasicBlockOffsets) {
        for (auto &BB : Function) {
          BasicBlockOffsets.emplace_back(
              std::make_pair(BB.getInputOffset(), &BB));
        }
      };

  auto getBasicBlockAtOffset = [&](uint64_t Offset) -> BinaryBasicBlock * {
    if (BasicBlockOffsets.empty())
      return nullptr;

    auto It = std::upper_bound(
        BasicBlockOffsets.begin(), BasicBlockOffsets.end(),
        BasicBlockOffset(Offset, nullptr), CompareBasicBlockOffsets());
    assert(It != BasicBlockOffsets.begin() &&
           "first basic block not at offset 0");
    --It;
    auto *BB = It->second;
    return (Offset == BB->getInputOffset()) ? BB : nullptr;
  };

  auto ParsingBuf = MemBuf.get()->getBuffer();
  BinaryFunction *Function = nullptr;
  while (ParsingBuf.size() > 0) {
    auto LineEnd = ParsingBuf.find_first_of("\n");
    if (LineEnd == StringRef::npos) {
      errs() << "BOLT-ERROR: File not in the correct format.\n";
      exit(EXIT_FAILURE);
    }

    StringRef Line = ParsingBuf.substr(0, LineEnd);
    auto Type = Line.split(" ");
    if (!Type.first.equals("EDGE") && !Type.first.equals("FUNCTION") &&
        !Line.equals("END")) {
      errs() << "BOLT-ERROR: File not in the correct format, found: " << Line
             << "\n";
      exit(EXIT_FAILURE);
    }

    if (Type.first.equals("FUNCTION")) {
      clear();
      BasicBlockOffsets.clear();
      auto FunLine = Type.second.split(" ");
      // the first substring is the function's name
      StringRef NumStr = FunLine.second.split(" ").first;
      uint64_t FunctionAddress = 0;
      if (NumStr.getAsInteger(16, FunctionAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      StringRef FunFreqStr = FunLine.second.split(" ").second;
      uint64_t FunFreq = 0;
      if (FunFreqStr.getAsInteger(10, FunFreq)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }
      auto it = StaleFunctionsAddresses.find(FunctionAddress);
      if (it != StaleFunctionsAddresses.end()) {
        Function = BC.getBinaryFunctionAtAddress(FunctionAddress);
        if(Function){
          Function->setExecutionCount(FunFreq);
          Function->markProfiled(BinaryFunction::PF_STATIC);
          StaleFunctionsAddresses.erase(it);
          populateBasicBlockOffsets(*Function, BasicBlockOffsets);
          OriginalFunctionsFrequency[FunctionAddress] = FunFreq;
        }
      } 
    }

    if (!Function){
      ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
      continue;
    }
    
    if (Type.first.equals("EDGE")) {
      auto EdgeLine = Type.second.split(" ");

      StringRef SrcBBAddressStr = EdgeLine.first;
      uint64_t SrcBBAddress;
      if (SrcBBAddressStr.getAsInteger(16, SrcBBAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      auto EdgeInfo = EdgeLine.second.split(" ");
      StringRef DstBBAddressStr = EdgeInfo.first;
      uint64_t DstBBAddress;
      if (DstBBAddressStr.getAsInteger(16, DstBBAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(EXIT_FAILURE);
      }

      auto SrcBB = getBasicBlockAtOffset(SrcBBAddress);
      auto DstBB = getBasicBlockAtOffset(DstBBAddress);
      if (SrcBB && DstBB && SrcBB->isSuccessor(DstBB)) {
        uint64_t Freq;
        StringRef FreqStr = EdgeInfo.second;

        if (FreqStr.getAsInteger(10, Freq)) {
          errs() << "BOLT-ERROR: File not in the correct format.\n";
          exit(EXIT_FAILURE);
        }

        SrcBB->setSuccessorBranchInfo(*DstBB, Freq, 0);
        uint64_t newBBFreq = DstBB->getKnownExecutionCount() + Freq;
        DstBB->setExecutionCount(newBBFreq);
      }
    } else if (Line.equals("END")) {
      BasicBlockOffsets.clear();
    }
    
    ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
  }
}


void StaticBranchProbabilities::computeProbabilities(BinaryFunction &Function) {
  Function.setExecutionCount(1);

  for (auto &BB : Function) {
    BB.setExecutionCount(0);
    unsigned NumSucc = BB.succ_size();
    if (NumSucc == 0)
      continue;

    if (!opts::MLBased)
      continue;

    double total_prob = 0.0;
    for (BinaryBasicBlock *SuccBB : BB.successors()) {
      uint64_t Frequency = BB.getBranchInfo(*SuccBB).Count;
      double EdgeProb = (Frequency == UINT64_MAX) ? 0.0 : Frequency / DIVISOR;

      Edge CFGEdge = std::make_pair(BB.getLabel(), SuccBB->getLabel());
      CFGEdgeProbabilities[CFGEdge] = EdgeProb;
      total_prob += EdgeProb;
      BB.setSuccessorBranchInfo(*SuccBB, 0.0, 0.0);
    }

    // fallback: no ML data → assign default fallback probabilities
    if (total_prob == 0.0) {
      BinaryBasicBlock *TakenSuccBB = BB.getConditionalSuccessor(true);
      BinaryBasicBlock *NotTakenSuccBB = BB.getConditionalSuccessor(false);

      if (TakenSuccBB && NotTakenSuccBB) {
        CFGEdgeProbabilities[{BB.getLabel(), TakenSuccBB->getLabel()}] = 0.5;
        CFGEdgeProbabilities[{BB.getLabel(), NotTakenSuccBB->getLabel()}] = 0.5;
      } else if (TakenSuccBB) {
        CFGEdgeProbabilities[{BB.getLabel(), TakenSuccBB->getLabel()}] = 1.0;
      } else if (NotTakenSuccBB) {
        CFGEdgeProbabilities[{BB.getLabel(), NotTakenSuccBB->getLabel()}] = 1.0;
      }
    }
  }
}



} // namespace bolt

} // namespace llvm

