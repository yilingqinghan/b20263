#include "bolt/Passes/RISCV/RISCVConstLUIReuse.h"
#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryData.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <map>

#define DEBUG_TYPE "riscv-const-lui-reuse"

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltOptCategory;

static cl::opt<bool> RISCVConstLUIReuseOpt(
    "riscv-const-lui-reuse",
    cl::desc("reuse nearby RISC-V constants and addresses materialized by LUI"),
    cl::init(true), cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

namespace {

struct KnownValue {
  int64_t Value = 0;
  const MCSymbol *Symbol = nullptr;
  int64_t Addend = 0;
  const BinarySection *Section = nullptr;

  static KnownValue absolute(int64_t Value) {
    return {Value, nullptr, 0, nullptr};
  }
  bool isSymbolic() const { return Symbol != nullptr; }
};

using RegValueMap = std::map<MCPhysReg, KnownValue>;

struct SymbolAddend {
  const MCSymbol *Symbol = nullptr;
  int64_t Addend = 0;
};

struct MaterializedPair {
  BinaryBasicBlock::iterator Sink;
  MCPhysReg LUIReg = RISCV::NoRegister;
  MCPhysReg DstReg = RISCV::NoRegister;
  KnownValue Target;
  KnownValue HighValue;
  bool HasKnownHigh = false;
};

struct MemoryPair {
  BinaryBasicBlock::iterator Sink;
  MCPhysReg LUIReg = RISCV::NoRegister;
  KnownValue Target;
  KnownValue HighValue;
  bool HasKnownHigh = false;
};

static bool isNonZeroGPR(MCPhysReg Reg) { return Reg != RISCV::X0; }

static int64_t sext32(int64_t Value) {
  return SignExtend64<32>(static_cast<uint64_t>(Value));
}

static int64_t add32(int64_t LHS, int64_t RHS) { return sext32(LHS + RHS); }

static int64_t sub32(int64_t LHS, int64_t RHS) { return sext32(LHS - RHS); }

static int64_t getLUIValue(int64_t Imm) {
  return sext32((Imm & 0xfffff) << 12);
}

static int64_t getHI20Value(int64_t Target) {
  return sext32(((Target + 0x800) >> 12) << 12);
}

static bool isLUI(const MCInst &Inst) {
  return Inst.getOpcode() == RISCV::LUI && Inst.getNumOperands() >= 2 &&
         Inst.getOperand(0).isReg() &&
         (Inst.getOperand(1).isImm() || Inst.getOperand(1).isExpr());
}

static bool isADDI(const MCInst &Inst) {
  return Inst.getOpcode() == RISCV::ADDI && Inst.getNumOperands() >= 3 &&
         Inst.getOperand(0).isReg() && Inst.getOperand(1).isReg();
}

static bool isADDIImm(const MCInst &Inst) {
  return isADDI(Inst) && Inst.getOperand(2).isImm();
}

static bool isIFormatMemorySink(const MCInst &Inst) {
  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::LB:
  case RISCV::LBU:
  case RISCV::LH:
  case RISCV::LHU:
  case RISCV::LW:
  case RISCV::LWU:
  case RISCV::LD:
  case RISCV::FLW:
  case RISCV::FLD:
    return Inst.getNumOperands() >= 3 && Inst.getOperand(0).isReg() &&
           Inst.getOperand(1).isReg();
  }
}

static bool isSFormatMemorySink(const MCInst &Inst) {
  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::SB:
  case RISCV::SH:
  case RISCV::SW:
  case RISCV::SD:
  case RISCV::FSW:
  case RISCV::FSD:
    return Inst.getNumOperands() >= 3 && Inst.getOperand(0).isReg() &&
           Inst.getOperand(1).isReg();
  }
}

static bool isMemorySink(const MCInst &Inst) {
  return isIFormatMemorySink(Inst) || isSFormatMemorySink(Inst);
}

static bool isAbsoluteHI(const MCOperand &Op) {
  if (!Op.isExpr())
    return false;
  const auto *RV = dyn_cast<RISCVMCExpr>(Op.getExpr());
  return RV && RV->getKind() == RISCVMCExpr::VK_RISCV_HI;
}

static bool isAbsoluteLO(const MCOperand &Op) {
  if (!Op.isExpr())
    return false;
  const auto *RV = dyn_cast<RISCVMCExpr>(Op.getExpr());
  return RV && RV->getKind() == RISCVMCExpr::VK_RISCV_LO;
}

static bool extractSymbolAddend(const MCExpr *Expr, SymbolAddend &Out,
                                int64_t Scale = 1) {
  if (const auto *RV = dyn_cast<RISCVMCExpr>(Expr))
    return extractSymbolAddend(RV->getSubExpr(), Out, Scale);

  if (const auto *Sym = dyn_cast<MCSymbolRefExpr>(Expr)) {
    if (Out.Symbol && Out.Symbol != &Sym->getSymbol())
      return false;
    Out.Symbol = &Sym->getSymbol();
    return true;
  }

  if (const auto *Const = dyn_cast<MCConstantExpr>(Expr)) {
    Out.Addend += Scale * Const->getValue();
    return true;
  }

  if (const auto *Bin = dyn_cast<MCBinaryExpr>(Expr)) {
    switch (Bin->getOpcode()) {
    case MCBinaryExpr::Add:
      return extractSymbolAddend(Bin->getLHS(), Out, Scale) &&
             extractSymbolAddend(Bin->getRHS(), Out, Scale);
    case MCBinaryExpr::Sub:
      return extractSymbolAddend(Bin->getLHS(), Out, Scale) &&
             extractSymbolAddend(Bin->getRHS(), Out, -Scale);
    default:
      return false;
    }
  }

  return false;
}

static bool getRISCVExprSymbolAddend(const MCOperand &Op, SymbolAddend &Out) {
  if (!Op.isExpr())
    return false;
  Out = SymbolAddend();
  return extractSymbolAddend(Op.getExpr(), Out) && Out.Symbol;
}

static bool sameSymbolAddend(const SymbolAddend &LHS, const SymbolAddend &RHS) {
  return LHS.Symbol == RHS.Symbol && LHS.Addend == RHS.Addend;
}

static bool getSymbolTarget(BinaryContext &BC, const SymbolAddend &Ref,
                            KnownValue &Target) {
  const BinaryData *BD = BC.getBinaryDataByName(Ref.Symbol->getName());
  if (!BD)
    return false;

  Target.Value = sext32(static_cast<int64_t>(BD->getAddress()) + Ref.Addend);
  Target.Symbol = Ref.Symbol;
  Target.Addend = Ref.Addend;
  Target.Section = &BD->getSection();
  return true;
}

static bool canUseFixedDiff(const KnownValue &Base, const KnownValue &Target) {
  if (!Target.isSymbolic())
    return !Base.isSymbolic();

  if (!Base.isSymbolic())
    return false;

  if (Base.Symbol == Target.Symbol)
    return true;
  return false;
}

static MCInst makeADDI(MCPhysReg Rd, MCPhysReg Rs1, int64_t Imm) {
  MCInst Out;
  Out.setOpcode(RISCV::ADDI);
  Out.addOperand(MCOperand::createReg(Rd));
  Out.addOperand(MCOperand::createReg(Rs1));
  Out.addOperand(MCOperand::createImm(Imm));
  return Out;
}

static MCInst makeMemoryWithBaseAndOffset(const MCInst &Inst, MCPhysReg Base,
                                          int64_t Offset) {
  MCInst Out = Inst;
  Out.getOperand(1) = MCOperand::createReg(Base);
  Out.getOperand(2) = MCOperand::createImm(Offset);
  return Out;
}

static void forgetReg(RegValueMap &Values, MCPhysReg Reg) {
  if (Reg != RISCV::X0)
    Values.erase(Reg);
}

static bool readsReg(const MCInst &Inst, MCPhysReg Reg,
                     const MCInstrInfo &MCII) {
  const MCInstrDesc &Desc = MCII.get(Inst.getOpcode());
  const unsigned NumDefs =
      std::min<unsigned>(Desc.getNumDefs(), Inst.getNumOperands());

  for (unsigned I = NumDefs, E = Inst.getNumOperands(); I != E; ++I)
    if (Inst.getOperand(I).isReg() && Inst.getOperand(I).getReg() == Reg)
      return true;

  return is_contained(Desc.implicit_uses(), Reg);
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

static bool isRegRedefinedBeforeUse(BinaryBasicBlock::iterator Begin,
                                    BinaryBasicBlock::iterator End,
                                    MCPhysReg Reg, const MCInstrInfo &MCII) {
  for (auto It = Begin; It != End; ++It) {
    if (readsReg(*It, Reg, MCII))
      return false;
    if (writesReg(*It, Reg, MCII))
      return true;
  }
  return false;
}

static bool canDeleteLUI(BinaryBasicBlock &BB, BinaryBasicBlock::iterator Sink,
                         MCPhysReg LUIReg, const MCInstrInfo &MCII) {
  if (writesReg(*Sink, LUIReg, MCII))
    return true;
  return isRegRedefinedBeforeUse(std::next(Sink), BB.end(), LUIReg, MCII);
}

static void forgetDefs(RegValueMap &Values, const MCInst &Inst,
                       const MCInstrInfo &MCII) {
  const MCInstrDesc &Desc = MCII.get(Inst.getOpcode());
  for (const MCPhysReg Reg : Desc.implicit_defs())
    forgetReg(Values, Reg);

  const unsigned NumDefs =
      std::min<unsigned>(Desc.getNumDefs(), Inst.getNumOperands());
  for (unsigned I = 0; I != NumDefs; ++I)
    if (Inst.getOperand(I).isReg())
      forgetReg(Values, Inst.getOperand(I).getReg());

  Values[RISCV::X0] = KnownValue::absolute(0);
}

static bool isBarrier(const MCInst &Inst, const MCInstrInfo &MCII) {
  const MCInstrDesc &Desc = MCII.get(Inst.getOpcode());
  // Some RISC-V tablegen descriptions do not mark direct JAL/JALR as calls.
  // They still clobber caller-saved values, so do not carry materialized
  // constants across them.
  const bool IsRISCVCall = Inst.getOpcode() == RISCV::JAL ||
                           Inst.getOpcode() == RISCV::JALR;
  return Desc.isCall() || IsRISCVCall || Desc.isReturn() || Desc.isBranch() ||
         Desc.hasUnmodeledSideEffects();
}

static int64_t getStableDiff(const KnownValue &Base, const KnownValue &Target) {
  if (Base.Symbol && Base.Symbol == Target.Symbol)
    return Target.Addend - Base.Addend;
  return sub32(Target.Value, Base.Value);
}

static bool findReusableBase(const RegValueMap &Values,
                             const KnownValue &Target, MCPhysReg &BaseReg,
                             int64_t &Diff) {
  bool Found = false;
  int64_t BestAbsDiff = 0;

  for (const auto &Entry : Values) {
    const KnownValue &Base = Entry.second;
    if (!canUseFixedDiff(Base, Target))
      continue;

    const int64_t CandidateDiff = getStableDiff(Base, Target);
    if (!isInt<12>(CandidateDiff) ||
        add32(Base.Value, CandidateDiff) != Target.Value)
      continue;

    const int64_t AbsDiff = CandidateDiff < 0 ? -CandidateDiff : CandidateDiff;
    if (!Found || AbsDiff < BestAbsDiff ||
        (AbsDiff == BestAbsDiff && Entry.first == BaseReg)) {
      Found = true;
      BestAbsDiff = AbsDiff;
      BaseReg = Entry.first;
      Diff = CandidateDiff;
    }
  }

  return Found;
}

static bool getLUIHighAndTarget(BinaryContext &BC, const MCInst &LUI,
                                const MCOperand &LOOp, KnownValue &HighValue,
                                bool &HasKnownHigh, KnownValue &Target) {
  const MCOperand &HIOp = LUI.getOperand(1);
  if (HIOp.isImm() && LOOp.isImm()) {
    HighValue = KnownValue::absolute(getLUIValue(HIOp.getImm()));
    HasKnownHigh = true;
    Target = KnownValue::absolute(add32(HighValue.Value, LOOp.getImm()));
    return true;
  }

  if (!isAbsoluteHI(HIOp) || !isAbsoluteLO(LOOp))
    return false;

  SymbolAddend HiRef;
  SymbolAddend LoRef;
  if (!getRISCVExprSymbolAddend(HIOp, HiRef) ||
      !getRISCVExprSymbolAddend(LOOp, LoRef) || !sameSymbolAddend(HiRef, LoRef))
    return false;

  if (!getSymbolTarget(BC, LoRef, Target))
    return false;

  HighValue = KnownValue::absolute(getHI20Value(Target.Value));
  HasKnownHigh = false;
  return true;
}

static bool getMaterializedPair(BinaryContext &BC, BinaryBasicBlock &BB,
                                BinaryBasicBlock::iterator LUIIt,
                                MaterializedPair &Pair) {
  if (!isLUI(*LUIIt))
    return false;

  auto ADDIIt = std::next(LUIIt);
  if (ADDIIt == BB.end() || !isADDI(*ADDIIt) ||
      (!ADDIIt->getOperand(2).isImm() && !ADDIIt->getOperand(2).isExpr()))
    return false;

  const MCPhysReg LUIReg = LUIIt->getOperand(0).getReg();
  if (ADDIIt->getOperand(1).getReg() != LUIReg)
    return false;

  KnownValue HighValue;
  KnownValue Target;
  bool HasKnownHigh = false;
  if (!getLUIHighAndTarget(BC, *LUIIt, ADDIIt->getOperand(2), HighValue,
                           HasKnownHigh, Target))
    return false;

  Pair.Sink = ADDIIt;
  Pair.LUIReg = LUIReg;
  Pair.DstReg = ADDIIt->getOperand(0).getReg();
  Pair.Target = Target;
  Pair.HighValue = HighValue;
  Pair.HasKnownHigh = HasKnownHigh;
  return true;
}

static bool getMemoryPair(BinaryContext &BC, BinaryBasicBlock &BB,
                          BinaryBasicBlock::iterator LUIIt, MemoryPair &Pair) {
  if (!isLUI(*LUIIt))
    return false;

  auto SinkIt = std::next(LUIIt);
  if (SinkIt == BB.end() || !isMemorySink(*SinkIt) ||
      (!SinkIt->getOperand(2).isImm() && !SinkIt->getOperand(2).isExpr()))
    return false;

  const MCPhysReg LUIReg = LUIIt->getOperand(0).getReg();
  if (SinkIt->getOperand(1).getReg() != LUIReg)
    return false;

  if (isSFormatMemorySink(*SinkIt) && SinkIt->getOperand(0).isReg() &&
      SinkIt->getOperand(0).getReg() == LUIReg)
    return false;

  KnownValue HighValue;
  KnownValue Target;
  bool HasKnownHigh = false;
  if (!getLUIHighAndTarget(BC, *LUIIt, SinkIt->getOperand(2), HighValue,
                           HasKnownHigh, Target))
    return false;

  Pair.Sink = SinkIt;
  Pair.LUIReg = LUIReg;
  Pair.Target = Target;
  Pair.HighValue = HighValue;
  Pair.HasKnownHigh = HasKnownHigh;
  return true;
}

static bool canCarryValuesToBlock(const BinaryFunction &BF,
                                  const BinaryBasicBlock *Prev,
                                  const BinaryBasicBlock &Cur) {
  if (!Prev || !BF.hasCFG())
    return false;
  if (Cur.pred_size() != 1 || Prev->succ_size() != 1)
    return false;
  return *Cur.pred_begin() == Prev && *Prev->succ_begin() == &Cur;
}

static void setKnownValue(RegValueMap &Values, MCPhysReg Reg,
                          const KnownValue &Value) {
  forgetReg(Values, Reg);
  if (isNonZeroGPR(Reg))
    Values[Reg] = Value;
}

static void learnMaterializedPair(RegValueMap &Values,
                                  const MaterializedPair &Pair) {
  if (Pair.DstReg == Pair.LUIReg) {
    setKnownValue(Values, Pair.DstReg, Pair.Target);
    return;
  }

  forgetReg(Values, Pair.LUIReg);
  if (Pair.HasKnownHigh)
    setKnownValue(Values, Pair.LUIReg, Pair.HighValue);
  setKnownValue(Values, Pair.DstReg, Pair.Target);
}

static void learnMemoryPair(RegValueMap &Values, const MemoryPair &Pair) {
  forgetReg(Values, Pair.LUIReg);
  if (Pair.HasKnownHigh)
    setKnownValue(Values, Pair.LUIReg, Pair.HighValue);
}

static void learnSimpleValue(RegValueMap &Values, const MCInst &Inst,
                             const MCInstrInfo &MCII) {
  if (isBarrier(Inst, MCII)) {
    Values.clear();
    Values[RISCV::X0] = KnownValue::absolute(0);
    return;
  }

  switch (Inst.getOpcode()) {
  default:
    forgetDefs(Values, Inst, MCII);
    return;
  case RISCV::LUI: {
    if (!isLUI(Inst) || !Inst.getOperand(1).isImm()) {
      forgetDefs(Values, Inst, MCII);
      return;
    }
    const MCPhysReg Rd = Inst.getOperand(0).getReg();
    forgetDefs(Values, Inst, MCII);
    if (isNonZeroGPR(Rd))
      Values[Rd] =
          KnownValue::absolute(getLUIValue(Inst.getOperand(1).getImm()));
    return;
  }
  case RISCV::ADDI: {
    if (!isADDIImm(Inst)) {
      forgetDefs(Values, Inst, MCII);
      return;
    }
    const MCPhysReg Rd = Inst.getOperand(0).getReg();
    const MCPhysReg Rs1 = Inst.getOperand(1).getReg();
    auto It = Values.find(Rs1);
    const bool Known = It != Values.end() && !It->second.isSymbolic();
    const KnownValue NewValue =
        Known ? KnownValue::absolute(
                    add32(It->second.Value, Inst.getOperand(2).getImm()))
              : KnownValue();
    forgetDefs(Values, Inst, MCII);
    if (Known && isNonZeroGPR(Rd))
      Values[Rd] = NewValue;
    return;
  }
  case RISCV::ADD: {
    if (Inst.getNumOperands() < 3 || !Inst.getOperand(0).isReg() ||
        !Inst.getOperand(1).isReg() || !Inst.getOperand(2).isReg()) {
      forgetDefs(Values, Inst, MCII);
      return;
    }
    const MCPhysReg Rd = Inst.getOperand(0).getReg();
    auto LHS = Values.find(Inst.getOperand(1).getReg());
    auto RHS = Values.find(Inst.getOperand(2).getReg());
    const bool Known = LHS != Values.end() && RHS != Values.end() &&
                       !LHS->second.isSymbolic() && !RHS->second.isSymbolic();
    const KnownValue NewValue =
        Known
            ? KnownValue::absolute(add32(LHS->second.Value, RHS->second.Value))
            : KnownValue();
    forgetDefs(Values, Inst, MCII);
    if (Known && isNonZeroGPR(Rd))
      Values[Rd] = NewValue;
    return;
  }
  case RISCV::C_LI: {
    if (Inst.getNumOperands() < 2 || !Inst.getOperand(0).isReg() ||
        !Inst.getOperand(1).isImm()) {
      forgetDefs(Values, Inst, MCII);
      return;
    }
    const MCPhysReg Rd = Inst.getOperand(0).getReg();
    forgetDefs(Values, Inst, MCII);
    if (isNonZeroGPR(Rd))
      Values[Rd] = KnownValue::absolute(sext32(Inst.getOperand(1).getImm()));
    return;
  }
  case RISCV::C_LUI: {
    if (Inst.getNumOperands() < 2 || !Inst.getOperand(0).isReg() ||
        !Inst.getOperand(1).isImm()) {
      forgetDefs(Values, Inst, MCII);
      return;
    }
    const MCPhysReg Rd = Inst.getOperand(0).getReg();
    forgetDefs(Values, Inst, MCII);
    if (isNonZeroGPR(Rd))
      Values[Rd] =
          KnownValue::absolute(getLUIValue(Inst.getOperand(1).getImm()));
    return;
  }
  case RISCV::C_ADDI: {
    if (Inst.getNumOperands() < 2 || !Inst.getOperand(0).isReg() ||
        !Inst.getOperand(1).isImm()) {
      forgetDefs(Values, Inst, MCII);
      return;
    }
    const MCPhysReg Rd = Inst.getOperand(0).getReg();
    auto It = Values.find(Rd);
    const bool Known = It != Values.end() && !It->second.isSymbolic();
    const KnownValue NewValue =
        Known ? KnownValue::absolute(
                    add32(It->second.Value, Inst.getOperand(1).getImm()))
              : KnownValue();
    forgetDefs(Values, Inst, MCII);
    if (Known && isNonZeroGPR(Rd))
      Values[Rd] = NewValue;
    return;
  }
  case RISCV::C_MV: {
    if (Inst.getNumOperands() < 2 || !Inst.getOperand(0).isReg() ||
        !Inst.getOperand(1).isReg()) {
      forgetDefs(Values, Inst, MCII);
      return;
    }
    const MCPhysReg Rd = Inst.getOperand(0).getReg();
    auto It = Values.find(Inst.getOperand(1).getReg());
    const bool Known = It != Values.end();
    const KnownValue NewValue = Known ? It->second : KnownValue();
    forgetDefs(Values, Inst, MCII);
    if (Known && isNonZeroGPR(Rd))
      Values[Rd] = NewValue;
    return;
  }
  }
}

} // namespace

void RISCVConstLUIReuse::runOnFunction(BinaryFunction &BF,
                                       uint64_t &ReusedMaterializations,
                                       uint64_t &ReusedMemory,
                                       uint64_t &ReusedLUIs) {
  BinaryContext &BC = BF.getBinaryContext();
  const MCInstrInfo &MCII = *BC.MII;

  RegValueMap Values;
  const BinaryBasicBlock *PrevBB = nullptr;

  for (BinaryBasicBlock &BB : BF) {
    if (!canCarryValuesToBlock(BF, PrevBB, BB)) {
      Values.clear();
      Values[RISCV::X0] = KnownValue::absolute(0);
    }
    PrevBB = &BB;

    for (auto II = BB.begin(); II != BB.end();) {
      if (isBarrier(*II, MCII)) {
        Values.clear();
        Values[RISCV::X0] = KnownValue::absolute(0);
        ++II;
        continue;
      }

      MaterializedPair MatPair;
      if (getMaterializedPair(BC, BB, II, MatPair)) {
        MCPhysReg BaseReg = RISCV::NoRegister;
        int64_t Diff = 0;
        if (isNonZeroGPR(MatPair.DstReg) &&
            canDeleteLUI(BB, MatPair.Sink, MatPair.LUIReg, MCII) &&
            findReusableBase(Values, MatPair.Target, BaseReg, Diff)) {
          *MatPair.Sink = makeADDI(MatPair.DstReg, BaseReg, Diff);
          II = BB.eraseInstruction(II);
          setKnownValue(Values, MatPair.DstReg, MatPair.Target);
          ++ReusedMaterializations;
          ++II;
          continue;
        }

        learnMaterializedPair(Values, MatPair);
        II = std::next(MatPair.Sink);
        continue;
      }

      MemoryPair MemPair;
      if (getMemoryPair(BC, BB, II, MemPair)) {
        MCPhysReg BaseReg = RISCV::NoRegister;
        int64_t Diff = 0;
        if (canDeleteLUI(BB, MemPair.Sink, MemPair.LUIReg, MCII) &&
            findReusableBase(Values, MemPair.Target, BaseReg, Diff)) {
          *MemPair.Sink =
              makeMemoryWithBaseAndOffset(*MemPair.Sink, BaseReg, Diff);
          II = BB.eraseInstruction(II);
          learnSimpleValue(Values, *MemPair.Sink, MCII);
          ++ReusedMemory;
          ++II;
          continue;
        }

        learnMemoryPair(Values, MemPair);
        II = std::next(MemPair.Sink);
        continue;
      }

      if (isLUI(*II) && II->getOperand(1).isImm()) {
        const MCPhysReg Rd = II->getOperand(0).getReg();
        if (!isNonZeroGPR(Rd)) {
          learnSimpleValue(Values, *II, MCII);
          ++II;
          continue;
        }
        const KnownValue Target =
            KnownValue::absolute(getLUIValue(II->getOperand(1).getImm()));
        MCPhysReg BaseReg = RISCV::NoRegister;
        int64_t Diff = 0;
        if (findReusableBase(Values, Target, BaseReg, Diff) &&
            ((BaseReg == Rd && isInt<6>(Diff)) ||
             (Diff == 0 && isNonZeroGPR(BaseReg)))) {
          *II = makeADDI(Rd, BaseReg, Diff);
          setKnownValue(Values, Rd, Target);
          ++ReusedLUIs;
          ++II;
          continue;
        }
      }

      learnSimpleValue(Values, *II, MCII);
      ++II;
    }
  }
}

void RISCVConstLUIReuse::runOnFunctions(BinaryContext &BC) {
  if (!opts::RISCVConstLUIReuseOpt)
    return;

  uint64_t ReusedMaterializations = 0;
  uint64_t ReusedMemory = 0;
  uint64_t ReusedLUIs = 0;
  for (auto &BFIt : BC.getBinaryFunctions())
    runOnFunction(BFIt.second, ReusedMaterializations, ReusedMemory,
                  ReusedLUIs);

  if (opts::Verbosity > 0 &&
      (ReusedMaterializations || ReusedMemory || ReusedLUIs))
    outs() << "BOLT-INFO: RISC-V const LUI reused " << ReusedMaterializations
           << " materializations, " << ReusedMemory << " memory bases and "
           << ReusedLUIs << " standalone LUIs\n";
}

} // namespace bolt
} // namespace llvm
