#include "bolt/Passes/RISCV/RISCVCompressPeephole.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Core/BinaryBasicBlock.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "riscv-compress-peephole"

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltOptCategory;

static cl::opt<bool>
    RISCVCompressPeepholeOpt("riscv-compress-peephole",
                             cl::desc("compress eligible RISC-V instructions "
                                      "to RVC forms after BOLT rewrites"),
                             cl::init(true), cl::cat(BoltOptCategory));

static cl::opt<bool> RISCVCompressPeepholeDryRun(
    "riscv-compress-peephole-dry-run",
    cl::desc("count eligible RISC-V RVC peephole compressions without applying "
             "them"),
    cl::init(false), cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

namespace {

static bool isX0(MCPhysReg Reg) { return Reg == RISCV::X0; }
static bool isSP(MCPhysReg Reg) { return Reg == RISCV::X2; }

static bool isNonZeroGPR(MCPhysReg Reg) { return Reg != RISCV::X0; }

static bool isGPRC(MCPhysReg Reg) {
  switch (Reg) {
  default:
    return false;
  case RISCV::X8:
  case RISCV::X9:
  case RISCV::X10:
  case RISCV::X11:
  case RISCV::X12:
  case RISCV::X13:
  case RISCV::X14:
  case RISCV::X15:
    return true;
  }
}

static bool isSImm6(int64_t Imm) { return Imm >= -32 && Imm <= 31; }

static bool isUImmScaled(int64_t Imm, int64_t Max, int64_t Scale) {
  return Imm >= 0 && Imm <= Max && Imm % Scale == 0;
}

static bool isCAddi16SPImm(int64_t Imm) {
  return Imm != 0 && Imm >= -512 && Imm <= 496 && Imm % 16 == 0;
}

static bool hasRegRegImm(const MCInst &Inst) {
  return Inst.getNumOperands() >= 3 && Inst.getOperand(0).isReg() &&
         Inst.getOperand(1).isReg() && Inst.getOperand(2).isImm();
}

static bool hasRegImm(const MCInst &Inst) {
  return Inst.getNumOperands() >= 2 && Inst.getOperand(0).isReg() &&
         Inst.getOperand(1).isImm();
}

static bool hasRegRegReg(const MCInst &Inst) {
  return Inst.getNumOperands() >= 3 && Inst.getOperand(0).isReg() &&
         Inst.getOperand(1).isReg() && Inst.getOperand(2).isReg();
}

static MCInst make0(unsigned Opcode) {
  MCInst Out;
  Out.setOpcode(Opcode);
  return Out;
}

static MCInst makeReg(unsigned Opcode, MCPhysReg Reg) {
  MCInst Out;
  Out.setOpcode(Opcode);
  Out.addOperand(MCOperand::createReg(Reg));
  return Out;
}

static MCInst makeRegImm(unsigned Opcode, MCPhysReg Reg, int64_t Imm) {
  MCInst Out;
  Out.setOpcode(Opcode);
  Out.addOperand(MCOperand::createReg(Reg));
  Out.addOperand(MCOperand::createImm(Imm));
  return Out;
}

static MCInst makeRegReg(unsigned Opcode, MCPhysReg Reg0, MCPhysReg Reg1) {
  MCInst Out;
  Out.setOpcode(Opcode);
  Out.addOperand(MCOperand::createReg(Reg0));
  Out.addOperand(MCOperand::createReg(Reg1));
  return Out;
}

static MCInst makeRegRegImm(unsigned Opcode, MCPhysReg Reg0, MCPhysReg Reg1,
                            int64_t Imm) {
  MCInst Out;
  Out.setOpcode(Opcode);
  Out.addOperand(MCOperand::createReg(Reg0));
  Out.addOperand(MCOperand::createReg(Reg1));
  Out.addOperand(MCOperand::createImm(Imm));
  return Out;
}

static bool compressADDI(const MCInst &Inst, MCInst &Out) {
  if (!hasRegRegImm(Inst))
    return false;

  const MCPhysReg Rd = Inst.getOperand(0).getReg();
  const MCPhysReg Rs1 = Inst.getOperand(1).getReg();
  const int64_t Imm = Inst.getOperand(2).getImm();

  if (isX0(Rd) && isX0(Rs1) && Imm == 0) {
    Out = make0(RISCV::C_NOP);
    return true;
  }

  if (isSP(Rd) && isSP(Rs1) && isCAddi16SPImm(Imm)) {
    Out = makeRegImm(RISCV::C_ADDI16SP, Rd, Imm);
    return true;
  }

  if (isGPRC(Rd) && isSP(Rs1) && isUImmScaled(Imm, 1020, 4) && Imm != 0) {
    Out = makeRegRegImm(RISCV::C_ADDI4SPN, Rd, Rs1, Imm);
    return true;
  }

  if (isNonZeroGPR(Rd) && isX0(Rs1) && isSImm6(Imm)) {
    Out = makeRegImm(RISCV::C_LI, Rd, Imm);
    return true;
  }

  if (isNonZeroGPR(Rd) && Rd == Rs1 && Imm != 0 && isSImm6(Imm)) {
    Out = makeRegImm(RISCV::C_ADDI, Rd, Imm);
    return true;
  }

  if (isNonZeroGPR(Rd) && isNonZeroGPR(Rs1) && Imm == 0) {
    Out = makeRegReg(RISCV::C_MV, Rd, Rs1);
    return true;
  }

  return false;
}

static bool compressLUI(const MCInst &Inst, MCInst &Out) {
  if (!hasRegImm(Inst))
    return false;

  const MCPhysReg Rd = Inst.getOperand(0).getReg();
  const int64_t Imm = Inst.getOperand(1).getImm();
  if (Rd == RISCV::X0 || Rd == RISCV::X2 || Imm == 0 || !isSImm6(Imm))
    return false;

  Out = makeRegImm(RISCV::C_LUI, Rd, Imm);
  return true;
}

static bool compressShiftOrAndI(const MCInst &Inst, MCInst &Out) {
  if (!hasRegRegImm(Inst))
    return false;

  const MCPhysReg Rd = Inst.getOperand(0).getReg();
  const MCPhysReg Rs1 = Inst.getOperand(1).getReg();
  const int64_t Imm = Inst.getOperand(2).getImm();
  if (Rd != Rs1)
    return false;

  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::SLLI:
    if (isNonZeroGPR(Rd) && Imm >= 1 && Imm <= 31) {
      Out = makeRegImm(RISCV::C_SLLI, Rd, Imm);
      return true;
    }
    return false;
  case RISCV::SRLI:
    if (isGPRC(Rd) && Imm >= 1 && Imm <= 31) {
      Out = makeRegImm(RISCV::C_SRLI, Rd, Imm);
      return true;
    }
    return false;
  case RISCV::SRAI:
    if (isGPRC(Rd) && Imm >= 1 && Imm <= 31) {
      Out = makeRegImm(RISCV::C_SRAI, Rd, Imm);
      return true;
    }
    return false;
  case RISCV::ANDI:
    if (isGPRC(Rd) && isSImm6(Imm)) {
      Out = makeRegImm(RISCV::C_ANDI, Rd, Imm);
      return true;
    }
    return false;
  }
}

static bool compressALU(const MCInst &Inst, MCInst &Out) {
  if (!hasRegRegReg(Inst))
    return false;

  const MCPhysReg Rd = Inst.getOperand(0).getReg();
  const MCPhysReg Rs1 = Inst.getOperand(1).getReg();
  const MCPhysReg Rs2 = Inst.getOperand(2).getReg();

  if (Inst.getOpcode() == RISCV::ADD) {
    if (isNonZeroGPR(Rd) && isX0(Rs1) && isX0(Rs2)) {
      Out = makeRegImm(RISCV::C_LI, Rd, 0);
      return true;
    }
    if (isNonZeroGPR(Rd) && isX0(Rs1) && isNonZeroGPR(Rs2)) {
      Out = makeRegReg(RISCV::C_MV, Rd, Rs2);
      return true;
    }
    if (isNonZeroGPR(Rd) && isNonZeroGPR(Rs1) && isX0(Rs2)) {
      Out = makeRegReg(RISCV::C_MV, Rd, Rs1);
      return true;
    }
    if (isNonZeroGPR(Rd) && Rd == Rs1 && isNonZeroGPR(Rs2)) {
      Out = makeRegReg(RISCV::C_ADD, Rd, Rs2);
      return true;
    }
    if (isNonZeroGPR(Rd) && Rd == Rs2 && isNonZeroGPR(Rs1)) {
      Out = makeRegReg(RISCV::C_ADD, Rd, Rs1);
      return true;
    }
    return false;
  }

  auto TryCA = [&](unsigned CompressedOpcode, bool Commutative) -> bool {
    if (isGPRC(Rd) && Rd == Rs1 && isGPRC(Rs2)) {
      Out = makeRegReg(CompressedOpcode, Rd, Rs2);
      return true;
    }
    if (Commutative && isGPRC(Rd) && Rd == Rs2 && isGPRC(Rs1)) {
      Out = makeRegReg(CompressedOpcode, Rd, Rs1);
      return true;
    }
    return false;
  };

  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::SUB:
    return TryCA(RISCV::C_SUB, false);
  case RISCV::XOR:
    return TryCA(RISCV::C_XOR, true);
  case RISCV::OR:
    return TryCA(RISCV::C_OR, true);
  case RISCV::AND:
    return TryCA(RISCV::C_AND, true);
  }
}

static bool compressLoadStore(const MCInst &Inst, MCInst &Out) {
  if (!hasRegRegImm(Inst))
    return false;

  const MCPhysReg Reg0 = Inst.getOperand(0).getReg();
  const MCPhysReg Base = Inst.getOperand(1).getReg();
  const int64_t Imm = Inst.getOperand(2).getImm();

  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::LW:
    if (isNonZeroGPR(Reg0) && isSP(Base) && isUImmScaled(Imm, 252, 4)) {
      Out = makeRegRegImm(RISCV::C_LWSP, Reg0, Base, Imm);
      return true;
    }
    if (isGPRC(Reg0) && isGPRC(Base) && isUImmScaled(Imm, 124, 4)) {
      Out = makeRegRegImm(RISCV::C_LW, Reg0, Base, Imm);
      return true;
    }
    return false;
  case RISCV::SW:
    if (isSP(Base) && isUImmScaled(Imm, 252, 4)) {
      Out = makeRegRegImm(RISCV::C_SWSP, Reg0, Base, Imm);
      return true;
    }
    if (isGPRC(Reg0) && isGPRC(Base) && isUImmScaled(Imm, 124, 4)) {
      Out = makeRegRegImm(RISCV::C_SW, Reg0, Base, Imm);
      return true;
    }
    return false;
  }
}

static bool compressJALR(const MCInst &Inst, MCInst &Out) {
  if (!hasRegRegImm(Inst))
    return false;

  const MCPhysReg Rd = Inst.getOperand(0).getReg();
  const MCPhysReg Rs1 = Inst.getOperand(1).getReg();
  const int64_t Imm = Inst.getOperand(2).getImm();
  if (Imm != 0 || !isNonZeroGPR(Rs1))
    return false;

  if (Rd == RISCV::X0) {
    Out = makeReg(RISCV::C_JR, Rs1);
    return true;
  }

  // Do not compress JALR x1 here. C.JALR writes pc+2 instead of pc+4, which is
  // ABI-safe for ordinary calls but not for all hand-written code patterns.
  return false;
}

static bool compressInst(const MCInst &Inst, MCInst &Out) {
  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::ADDI:
    return compressADDI(Inst, Out);
  case RISCV::LUI:
    return compressLUI(Inst, Out);
  case RISCV::SLLI:
  case RISCV::SRLI:
  case RISCV::SRAI:
  case RISCV::ANDI:
    return compressShiftOrAndI(Inst, Out);
  case RISCV::ADD:
  case RISCV::SUB:
  case RISCV::XOR:
  case RISCV::OR:
  case RISCV::AND:
    return compressALU(Inst, Out);
  case RISCV::LW:
  case RISCV::SW:
    return compressLoadStore(Inst, Out);
  case RISCV::JALR:
    return compressJALR(Inst, Out);
  case RISCV::EBREAK:
    Out = make0(RISCV::C_EBREAK);
    return true;
  }
}

static bool isCJOffset(int64_t Offset) {
  return Offset >= -2048 && Offset <= 2046 && !(Offset & 1);
}

static uint64_t compressDirectJumps(BinaryFunction &BF) {
  BinaryContext &BC = BF.getBinaryContext();
  uint64_t Changed = 0;

  // Shrinking a jump can only bring other local jumps closer to their target,
  // so repeat after each layout update until no further C.J forms fit.
  do {
    DenseMap<const MCSymbol *, uint64_t> LabelOffsets;
    DenseMap<const MCInst *, uint64_t> InstructionOffsets;
    uint64_t Offset = 0;
    for (BinaryBasicBlock &BB : BF) {
      LabelOffsets[BB.getLabel()] = Offset;
      for (MCInst &Inst : BB) {
        InstructionOffsets[&Inst] = Offset;
        Offset += BC.computeInstructionSize(Inst);
      }
    }

    uint64_t ChangedThisRound = 0;
    for (BinaryBasicBlock &BB : BF) {
      for (MCInst &Inst : BB) {
        if (Inst.getOpcode() != RISCV::JAL || Inst.getNumOperands() < 2 ||
            !Inst.getOperand(0).isReg() ||
            Inst.getOperand(0).getReg() != RISCV::X0)
          continue;

        const MCSymbol *Target = BC.MIB->getTargetSymbol(Inst);
        if (!Target || !BF.getBasicBlockForLabel(Target))
          continue;

        const auto TargetIt = LabelOffsets.find(Target);
        const auto InstIt = InstructionOffsets.find(&Inst);
        if (TargetIt == LabelOffsets.end() || InstIt == InstructionOffsets.end())
          continue;

        const int64_t Distance = static_cast<int64_t>(TargetIt->second) -
                                 static_cast<int64_t>(InstIt->second);
        if (!isCJOffset(Distance))
          continue;

        ++Changed;
        ++ChangedThisRound;
        if (!opts::RISCVCompressPeepholeDryRun) {
          MCInst Compressed;
          Compressed.setOpcode(RISCV::C_J);
          Compressed.addOperand(Inst.getOperand(1));
          Inst = Compressed;
        }
      }
    }

    if (opts::RISCVCompressPeepholeDryRun || !ChangedThisRound)
      break;
  } while (true);

  return Changed;
}

} // namespace

uint64_t RISCVCompressPeephole::runOnFunction(BinaryFunction &BF) {
  uint64_t Changed = 0;

  for (BinaryBasicBlock &BB : BF) {
    for (auto II = BB.begin(); II != BB.end(); ++II) {
      MCInst Compressed;
      if (!compressInst(*II, Compressed))
        continue;

      ++Changed;
      if (!opts::RISCVCompressPeepholeDryRun)
        *II = Compressed;
    }
  }

  Changed += compressDirectJumps(BF);
  return Changed;
}

void RISCVCompressPeephole::runOnFunctions(BinaryContext &BC) {
  if (!opts::RISCVCompressPeepholeOpt)
    return;

  uint64_t Changed = 0;
  for (auto &BFIt : BC.getBinaryFunctions())
    Changed += runOnFunction(BFIt.second);

  if (opts::Verbosity > 0 && Changed) {
    outs() << "BOLT-INFO: RISC-V compressed " << Changed
           << " instructions";
    if (opts::RISCVCompressPeepholeDryRun)
      outs() << " (dry-run)";
    outs() << '\n';
  }
}

} // namespace bolt
} // namespace llvm
