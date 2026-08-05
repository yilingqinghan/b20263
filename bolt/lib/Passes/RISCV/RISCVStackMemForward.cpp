#include "bolt/Passes/RISCV/RISCVStackMemForward.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Support/CommandLine.h"
#include <map>
#include <tuple>
#include <vector>

#define DEBUG_TYPE "riscv-stack-mem-forward"

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltOptCategory;

static cl::opt<bool> RISCVStackMemForwardOpt(
    "riscv-stack-mem-forward",
    cl::desc("forward safe RISC-V stack stores to later stack loads"),
    cl::init(true), cl::cat(BoltOptCategory));

static cl::opt<bool> RISCVStackMemDSEOpt(
    "riscv-stack-mem-dse",
    cl::desc("delete safe overwritten RISC-V stack stores"),
    cl::init(true), cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

namespace {

struct StackSlot {
  int64_t Offset = 0;
  unsigned Size = 0;

  bool operator<(const StackSlot &Other) const {
    return std::tie(Offset, Size) < std::tie(Other.Offset, Other.Size);
  }
};

struct StackAccess {
  StackSlot Slot;
  MCPhysReg Reg = RISCV::NoRegister;
  bool IsLoad = false;
  bool IsStore = false;
  bool CanForwardWord = false;
  bool IsFullWordLoad = false;
};

struct PendingLoad {
  BinaryBasicBlock::iterator Inst;
  MCPhysReg DstReg = RISCV::NoRegister;
};

struct StoredValue {
  BinaryBasicBlock::iterator Inst;
  MCPhysReg SrcReg = RISCV::NoRegister;
  bool SrcAvailable = false;
  std::vector<PendingLoad> Loads;
};

static bool rangesOverlap(StackSlot A, StackSlot B) {
  const int64_t AEnd = A.Offset + A.Size;
  const int64_t BEnd = B.Offset + B.Size;
  return A.Offset < BEnd && B.Offset < AEnd;
}

using StackBaseMap = std::map<MCPhysReg, int64_t>;

static bool getStackAccess(const MCInst &Inst, const StackBaseMap &StackBases,
                           StackAccess &Access) {
  unsigned Size = 0;
  bool IsLoad = false;
  bool IsStore = false;
  bool CanForwardWord = false;
  bool IsFullWordLoad = false;

  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::LB:
  case RISCV::LBU:
    Size = 1;
    IsLoad = true;
    break;
  case RISCV::LH:
  case RISCV::LHU:
    Size = 2;
    IsLoad = true;
    break;
  case RISCV::LW:
    Size = 4;
    IsLoad = true;
    CanForwardWord = true;
    IsFullWordLoad = true;
    break;
  case RISCV::C_LW:
  case RISCV::C_LWSP:
    Size = 4;
    IsLoad = true;
    CanForwardWord = true;
    break;
  case RISCV::SB:
    Size = 1;
    IsStore = true;
    break;
  case RISCV::SH:
    Size = 2;
    IsStore = true;
    break;
  case RISCV::SW:
  case RISCV::C_SW:
  case RISCV::C_SWSP:
    Size = 4;
    IsStore = true;
    CanForwardWord = true;
    break;
  }

  if (Inst.getNumOperands() < 3 || !Inst.getOperand(0).isReg() ||
      !Inst.getOperand(1).isReg() || !Inst.getOperand(2).isImm() ||
      !StackBases.count(Inst.getOperand(1).getReg()))
    return false;

  const auto BaseIt = StackBases.find(Inst.getOperand(1).getReg());
  Access.Slot = {BaseIt->second + Inst.getOperand(2).getImm(), Size};
  Access.Reg = Inst.getOperand(0).getReg();
  Access.IsLoad = IsLoad;
  Access.IsStore = IsStore;
  Access.CanForwardWord = CanForwardWord;
  Access.IsFullWordLoad = IsFullWordLoad;
  return true;
}

static MCInst makeMove(MCPhysReg Rd, MCPhysReg Rs) {
  MCInst Out;
  Out.setOpcode(RISCV::ADDI);
  Out.addOperand(MCOperand::createReg(Rd));
  Out.addOperand(MCOperand::createReg(Rs));
  Out.addOperand(MCOperand::createImm(0));
  return Out;
}

static bool writesReg(const MCInst &Inst, MCPhysReg Reg,
                      const MCInstrInfo &MCII) {
  const MCInstrDesc &Desc = MCII.get(Inst.getOpcode());
  const unsigned NumDefs =
      std::min<unsigned>(Desc.getNumDefs(), Inst.getNumOperands());

  for (unsigned I = 0; I != NumDefs; ++I)
    if (Inst.getOperand(I).isReg() && Inst.getOperand(I).getReg() == Reg)
      return true;

  return is_contained(Desc.implicit_defs(), Reg);
}

static bool isBarrier(const MCInst &Inst, const MCInstrInfo &MCII) {
  const MCInstrDesc &Desc = MCII.get(Inst.getOpcode());
  return Desc.isCall() || Desc.isReturn() || Desc.isBranch() ||
         Desc.hasUnmodeledSideEffects();
}

static bool getStackBaseDef(const MCInst &Inst, const StackBaseMap &StackBases,
                            MCPhysReg &Reg, int64_t &Offset) {
  if (Inst.getOpcode() != RISCV::ADDI || Inst.getNumOperands() < 3 ||
      !Inst.getOperand(0).isReg() || !Inst.getOperand(1).isReg() ||
      !Inst.getOperand(2).isImm())
    return false;

  auto BaseIt = StackBases.find(Inst.getOperand(1).getReg());
  if (BaseIt == StackBases.end())
    return false;

  Reg = Inst.getOperand(0).getReg();
  Offset = BaseIt->second + Inst.getOperand(2).getImm();
  return Reg != RISCV::X0;
}

static void invalidateSrcReg(std::map<StackSlot, StoredValue> &Stores,
                             MCPhysReg Reg) {
  if (Reg == RISCV::NoRegister)
    return;

  for (auto &Entry : Stores)
    if (Entry.second.SrcReg == Reg)
      Entry.second.SrcAvailable = false;
}

static void removeOverlappingReads(std::map<StackSlot, StoredValue> &Stores,
                                   StackSlot Slot) {
  for (auto It = Stores.begin(); It != Stores.end();) {
    if (rangesOverlap(It->first, Slot))
      It = Stores.erase(It);
    else
      ++It;
  }
}

static void forwardPendingLoads(BinaryBasicBlock &BB, StoredValue &Store,
                                uint64_t &ForwardedLoads) {
  for (const PendingLoad &Load : Store.Loads) {
    if (Load.DstReg == Store.SrcReg)
      BB.eraseInstruction(Load.Inst);
    else
      *Load.Inst = makeMove(Load.DstReg, Store.SrcReg);
    ++ForwardedLoads;
  }
  Store.Loads.clear();
}

} // namespace

void RISCVStackMemForward::runOnFunction(BinaryFunction &BF,
                                         uint64_t &ForwardedLoads,
                                         uint64_t &ErasedStores) {
  BinaryContext &BC = BF.getBinaryContext();
  const MCInstrInfo &MCII = *BC.MII;

  for (BinaryBasicBlock &BB : BF) {
    std::map<StackSlot, StoredValue> Stores;
    StackBaseMap StackBases;
    StackBases[RISCV::X2] = 0;

    for (auto II = BB.begin(); II != BB.end();) {
      StackAccess Access;
      const bool IsStackAccess = getStackAccess(*II, StackBases, Access);
      const bool Barrier = isBarrier(*II, MCII);
      const MCInstrDesc &Desc = MCII.get(II->getOpcode());
      MCPhysReg NewBaseReg = RISCV::NoRegister;
      int64_t NewBaseOffset = 0;
      const bool HasNewBase =
          getStackBaseDef(*II, StackBases, NewBaseReg, NewBaseOffset);

      if (Barrier || (!IsStackAccess && (Desc.mayLoad() || Desc.mayStore()))) {
        Stores.clear();
        StackBases.clear();
        StackBases[RISCV::X2] = 0;
        ++II;
        continue;
      }

      if (writesReg(*II, RISCV::X2, MCII)) {
        Stores.clear();
        StackBases.clear();
        StackBases[RISCV::X2] = 0;
        ++II;
        continue;
      }

      if (IsStackAccess && Access.IsLoad) {
        auto StoreIt = Stores.find(Access.Slot);
        if (opts::RISCVStackMemForwardOpt && Access.CanForwardWord &&
            StoreIt != Stores.end() && StoreIt->second.SrcAvailable) {
          if (Access.IsFullWordLoad) {
            const MCPhysReg SrcReg = StoreIt->second.SrcReg;
            if (Access.Reg == SrcReg)
              II = BB.eraseInstruction(II);
            else {
              *II = makeMove(Access.Reg, SrcReg);
              ++II;
            }
            ++ForwardedLoads;
            if (Access.Reg != SrcReg)
              invalidateSrcReg(Stores, Access.Reg);
            StackBases.erase(Access.Reg);
            continue;
          } else {
            StoreIt->second.Loads.push_back({II, Access.Reg});
            if (Access.Reg != StoreIt->second.SrcReg)
              invalidateSrcReg(Stores, Access.Reg);
          }
        } else {
          removeOverlappingReads(Stores, Access.Slot);
          invalidateSrcReg(Stores, Access.Reg);
        }
        StackBases.erase(Access.Reg);
        ++II;
        continue;
      }

      if (IsStackAccess && Access.IsStore) {
        for (auto It = Stores.begin(); It != Stores.end();) {
          if (!rangesOverlap(It->first, Access.Slot)) {
            ++It;
            continue;
          }

          if (opts::RISCVStackMemDSEOpt &&
              It->first.Offset == Access.Slot.Offset &&
              It->first.Size == Access.Slot.Size &&
              (It->second.Loads.empty() || opts::RISCVStackMemForwardOpt)) {
            forwardPendingLoads(BB, It->second, ForwardedLoads);
            BB.eraseInstruction(It->second.Inst);
            ++ErasedStores;
          }
          It = Stores.erase(It);
        }

        Stores[Access.Slot] = {II, Access.Reg, true, {}};
        ++II;
        continue;
      }

      if (!Stores.empty()) {
        for (const MCPhysReg Reg : MCII.get(II->getOpcode()).implicit_defs()) {
          invalidateSrcReg(Stores, Reg);
          StackBases.erase(Reg);
        }
        for (unsigned I = 0,
                      E = std::min<unsigned>(
                          MCII.get(II->getOpcode()).getNumDefs(),
                          II->getNumOperands());
             I != E; ++I) {
          if (II->getOperand(I).isReg()) {
            invalidateSrcReg(Stores, II->getOperand(I).getReg());
            StackBases.erase(II->getOperand(I).getReg());
          }
        }
      } else {
        for (const MCPhysReg Reg : MCII.get(II->getOpcode()).implicit_defs())
          StackBases.erase(Reg);
        for (unsigned I = 0,
                      E = std::min<unsigned>(
                          MCII.get(II->getOpcode()).getNumDefs(),
                          II->getNumOperands());
             I != E; ++I)
          if (II->getOperand(I).isReg())
            StackBases.erase(II->getOperand(I).getReg());
      }

      if (HasNewBase)
        StackBases[NewBaseReg] = NewBaseOffset;

      ++II;
    }
  }
}

void RISCVStackMemForward::runOnFunctions(BinaryContext &BC) {
  if (!opts::RISCVStackMemForwardOpt && !opts::RISCVStackMemDSEOpt)
    return;

  uint64_t ForwardedLoads = 0;
  uint64_t ErasedStores = 0;
  for (auto &BFIt : BC.getBinaryFunctions())
    runOnFunction(BFIt.second, ForwardedLoads, ErasedStores);

  if (opts::Verbosity > 0 && (ForwardedLoads || ErasedStores))
    outs() << "BOLT-INFO: RISC-V stack forwarded " << ForwardedLoads
           << " loads and erased " << ErasedStores << " stores\n";
}

} // namespace bolt
} // namespace llvm
