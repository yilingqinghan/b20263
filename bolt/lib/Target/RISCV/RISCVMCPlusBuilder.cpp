//===- bolt/Target/RISCV/RISCVMCPlusBuilder.cpp -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides RISCV-specific MCPlus builder.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Core/MCPlusBuilder.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCFixupKindInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "mcplus"

using namespace llvm;
using namespace bolt;

namespace {

class RISCVMCPlusBuilder : public MCPlusBuilder {
public:
  using MCPlusBuilder::MCPlusBuilder;
  // bool isLoad(const MCInst &Inst) const override {
  //   llvm::errs() << "Opcode================================"<<Inst.getOpcode()<<"\n";
  //   return 1;
  // }//假设全是Load，先查后面的错 TODO
  bool isLoadWord(const MCInst &Inst) const {
  return (Inst.getOpcode() == RISCV::LW ||
          Inst.getOpcode() == RISCV::LBU ||
          Inst.getOpcode() == RISCV::LHU ||
          Inst.getOpcode() == RISCV::LB ||
          Inst.getOpcode() == RISCV::LH ||
          Inst.getOpcode() == RISCV::LWU); // only in RV64
  }

  bool isLoadDouble(const MCInst &Inst) const {
    return (Inst.getOpcode() == RISCV::LD); // only in RV64
  }

  bool isLoadFloat(const MCInst &Inst) const {
    return (Inst.getOpcode() == RISCV::FLW ||
            Inst.getOpcode() == RISCV::FLD); // FLD only in RV64
  }

  bool isCompressedLoad(const MCInst &Inst) const {
    return (Inst.getOpcode() == RISCV::C_LW ||
            Inst.getOpcode() == RISCV::C_LWSP ||
            Inst.getOpcode() == RISCV::C_LD ||
            Inst.getOpcode() == RISCV::C_LDSP ||
            Inst.getOpcode() == RISCV::C_FLW ||
            Inst.getOpcode() == RISCV::C_FLWSP ||
            Inst.getOpcode() == RISCV::C_FLD ||
            Inst.getOpcode() == RISCV::C_FLDSP);
  }

  bool isLoad(const MCInst &Inst) const override {
    return isLoadWord(Inst) || isLoadDouble(Inst) || isLoadFloat(Inst) || isCompressedLoad(Inst);
  }
  // bool isStore(const MCInst &Inst) const override {
  //   llvm::errs() << "Opcode================================"<<Inst.getOpcode()<<"\n";
  //   return 0;
  // }

  bool isStoreInt(const MCInst &Inst) const {
    return (Inst.getOpcode() == RISCV::SB ||
            Inst.getOpcode() == RISCV::SH ||
            Inst.getOpcode() == RISCV::SW ||
            Inst.getOpcode() == RISCV::SD || // only on RV64
            Inst.getOpcode() == RISCV::C_SW ||
            Inst.getOpcode() == RISCV::C_SWSP ||
            Inst.getOpcode() == RISCV::C_SD ||
            Inst.getOpcode() == RISCV::C_SDSP);
  }

  bool isStoreFloat(const MCInst &Inst) const {
    return (Inst.getOpcode() == RISCV::FSW ||
            Inst.getOpcode() == RISCV::FSD ||
            Inst.getOpcode() == RISCV::C_FSW ||
            Inst.getOpcode() == RISCV::C_FSWSP ||
            Inst.getOpcode() == RISCV::C_FSD ||
            Inst.getOpcode() == RISCV::C_FSDSP);
  }

  bool isStore(const MCInst &Inst) const override {
    return isStoreInt(Inst) || isStoreFloat(Inst);
  }
  // bool isCall(const MCInst &Inst) const override {
  //   llvm::errs() << "isCall Opcode================================"<<Inst.getOpcode()<<"\n";
  //   return 0;
  // }

  // bool isCall(const MCInst &Inst) const override {
  //   unsigned Opc = Inst.getOpcode();
  //   return (Opc == RISCV::JAL   ||
  //           Opc == RISCV::JALR  ||
  //           // Opc == RISCV::CALL  ||   // LLVM 伪指令
  //           // Opc == RISCV::TAIL  ||   // LLVM 伪指令
  //           Opc == RISCV::C_JAL ||   // RV32 only
  //           Opc == RISCV::PseudoCALL ||      // 可能别名
  //           Opc == RISCV::PseudoTAIL);
  // }

  bool shouldRecordCodeRelocation(uint64_t RelType) const override {
    switch (RelType) {
    case ELF::R_RISCV_JAL:
    case ELF::R_RISCV_CALL:
    case ELF::R_RISCV_CALL_PLT:
    case ELF::R_RISCV_BRANCH:
    case ELF::R_RISCV_RVC_BRANCH:
    case ELF::R_RISCV_RVC_JUMP:
    case ELF::R_RISCV_GOT_HI20:
    case ELF::R_RISCV_PCREL_HI20:
    case ELF::R_RISCV_PCREL_LO12_I:
    case ELF::R_RISCV_LO12_I:
    case ELF::R_RISCV_HI20:
    case ELF::R_RISCV_LO12_S:
    case ELF::R_RISCV_COPY:
      return true;
    case ELF::R_RISCV_32:
    case ELF::R_RISCV_64:
      return false;
    default:
      llvm_unreachable("Unexpected RISCV relocation type in code");
    }
  }

  bool isNop(const MCInst &Inst) const {
    return Inst.getOpcode() == RISCV::ADDI &&
           Inst.getOperand(0).getReg() == RISCV::X0 &&
           Inst.getOperand(1).getReg() == RISCV::X0 &&
           Inst.getOperand(2).getImm() == 0;
  }

  bool isCNop(const MCInst &Inst) const {
    return Inst.getOpcode() == RISCV::C_NOP;
  }

  bool isNoop(const MCInst &Inst) const override {
    return isNop(Inst) || isCNop(Inst);
  }

  bool hasPCRelOperand(const MCInst &Inst) const override {
    switch (Inst.getOpcode()) {
    default:
      return false;
    case RISCV::JAL:
    case RISCV::AUIPC:
      return true;
    }
  }

  unsigned getInvertedBranchOpcode(unsigned Opcode) const {
    switch (Opcode) {
    default:
      llvm_unreachable("Failed to invert branch opcode");
      return Opcode;
    case RISCV::BEQ:
      return RISCV::BNE;
    case RISCV::BNE:
      return RISCV::BEQ;
    case RISCV::BLT:
      return RISCV::BGE;
    case RISCV::BGE:
      return RISCV::BLT;
    case RISCV::BLTU:
      return RISCV::BGEU;
    case RISCV::BGEU:
      return RISCV::BLTU;
    case RISCV::C_BEQZ:
      return RISCV::C_BNEZ;
    case RISCV::C_BNEZ:
      return RISCV::C_BEQZ;
    }
  }
  bool requiresAlignedAddress(const MCInst &Inst) const override {
    // 基础 RISCV ISA 中不需要软件判断是否对齐
    // 除非你特别想处理向量或者非对齐访问 trap 情况
    return false;
  }

  bool isStackAccess(const MCInst &Inst, bool &IsLoad, bool &IsStore,
                   bool &IsStoreFromReg, MCPhysReg &Reg, int32_t &SrcImm,
                   uint16_t &StackPtrReg, int64_t &StackOffset, uint8_t &Size,
                   bool &IsSimple, bool &IsIndexed) const override {

    unsigned Opc = Inst.getOpcode();
    IsLoad = false;
    IsStore = false;
    IsStoreFromReg = false;
    IsSimple = false;
    IsIndexed = false;
    StackPtrReg = RISCV::X2; // sp 是 x2

    // Handle loads
    switch (Opc) {
    case RISCV::LW:
    case RISCV::LH:
    case RISCV::LB:
      IsLoad = true;
      IsSimple = true;
      Size = 4; // 例如 LW 是 4，LD 是 8
      Reg = Inst.getOperand(0).getReg(); // 目标寄存器
      break;
    case RISCV::LD:
      IsLoad = true;
      IsSimple = true;
      Size = 8;
      Reg = Inst.getOperand(0).getReg(); // 目标寄存器
      break;
    case RISCV::SW:
    case RISCV::SH:
    case RISCV::SB:
      IsStore = true;
      IsStoreFromReg = true;
      IsSimple = true;
      Size = 4;
      Reg = Inst.getOperand(1).getReg(); // 源寄存器
      break;
    case RISCV::SD:
      IsStore = true;
      IsStoreFromReg = true;
      IsSimple = true;
      Size = 8;
      Reg = Inst.getOperand(1).getReg(); // 源寄存器
      break;
    default:
      return false;
    }

    // 判断访问的是不是 sp 相对地址
    int BaseRegIdx = IsLoad ? 2 : 2; // 第二个操作数是 base 寄存器
    int OffsetIdx = IsLoad ? 1 : 2;  // load/sw 都是 offset(base)

    if (!Inst.getOperand(BaseRegIdx).isReg() || !Inst.getOperand(OffsetIdx).isImm())
      return false;

    uint16_t BaseReg = Inst.getOperand(BaseRegIdx).getReg();
    if (BaseReg != RISCV::X2) // 只处理 sp
      return false;

    StackOffset = Inst.getOperand(OffsetIdx).getImm();
    return true;
  }

  bool escapesVariable(const MCInst &Inst, bool HasFramePointer) const override {
//    return false;
    const MCInstrDesc &MCII = Info->get(Inst.getOpcode());
    const unsigned NumDefs = MCII.getNumDefs();
    const int MemOpNo = getMemoryOperandNo(Inst); // 可选：你定义一个通用逻辑

    static BitVector SPAliases(getAliases(RISCV::X2));  // sp
    static BitVector SPFPAliases(BitVector(getAliases(RISCV::X2)) |=
                                 getAliases(RISCV::X8)); // sp + fp

    bool AccessMem = MCII.mayLoad() || MCII.mayStore();
    bool DoesLeak = false;

    for (int I = 0, E = MCPlus::getNumPrimeOperands(Inst); I != E; ++I) {
      if (MemOpNo != -1 && AccessMem && I >= MemOpNo && I <= MemOpNo + 1)
        continue;
      if (I < static_cast<int>(NumDefs))
        continue;

      const MCOperand &Operand = Inst.getOperand(I);
      if (!Operand.isReg())
        continue;

      unsigned Reg = Operand.getReg();
      if (HasFramePointer && SPFPAliases[Reg]) {
        DoesLeak = true;
        break;
      }
      if (!HasFramePointer && SPAliases[Reg]) {
        DoesLeak = true;
        break;
      }
    }

    if (DoesLeak) {
      DoesLeak = !any_of(defOperands(Inst), [&](const MCOperand &Operand) {
        if (!Operand.isReg()) return false;
        unsigned Reg = Operand.getReg();
        return HasFramePointer ? (bool)SPFPAliases[Reg] : (bool)SPAliases[Reg];
      });
    }

    return DoesLeak;
  }

  int getMemoryOperandNo(const MCInst &Inst) const override {
    switch (Inst.getOpcode()) {
      // LOAD 类型：lw, lh, lb, lwu, lhu, lbu, ld
    case RISCV::LW:
    case RISCV::LH:
    case RISCV::LB:
    case RISCV::LBU:
    case RISCV::LHU:
    case RISCV::LWU:
    case RISCV::LD:
      // 格式：rd, offset(rs1) -> rs1 是 base，偏移在 Imm
      return 1; // rs1 是 base pointer

      // STORE 类型：sw, sh, sb, sd
    case RISCV::SW:
    case RISCV::SH:
    case RISCV::SB:
    case RISCV::SD:
      // 格式：rs2, offset(rs1) -> rs1 是 base，偏移在 Imm
      return 1;

      // 有压缩指令也可以支持（如 C.LW, C.SW）
    case RISCV::C_LW:
    case RISCV::C_LD:
    case RISCV::C_SW:
    case RISCV::C_SD:
      return 1;

    default:
      return -1; // 不包含 memory operand
    }
  }

  MCPhysReg getStackPointer() const override {
    return RISCV::X2;  // sp
  }

  MCPhysReg getFramePointer() const override {
    return RISCV::X8;  // fp (a.k.a. s0)
  }

  MCPhysReg getFlagsReg() const override {
    llvm_unreachable("RISC-V has no flags register");
  }

  bool isRegToRegMove(const MCInst &Inst, MCPhysReg &From, MCPhysReg &To) const override {
    switch (Inst.getOpcode()) {
    default:
      return false;

    case RISCV::ADDI:
      // 典型伪 move：addi rd, rs1, 0
        if (Inst.getOperand(2).isImm() && Inst.getOperand(2).getImm() == 0) {
          To = Inst.getOperand(0).getReg();    // rd
          From = Inst.getOperand(1).getReg();  // rs1
          return true;
        }
      return false;

    case RISCV::C_MV:
      // 压缩指令：c.mv rd, rs
        To = Inst.getOperand(0).getReg();    // rd
      From = Inst.getOperand(1).getReg();  // rs
      return true;
    }
  }

  int getPushSize(const MCInst &Inst) const override {
    switch (Inst.getOpcode()) {
    case RISCV::SW:
    case RISCV::C_SWSP:   // 16-bit compressed
      return 4;
    case RISCV::SD:
    case RISCV::C_SDSP:   // 16-bit compressed (RV64 only)
      return 8;
    case RISCV::FSW:
    case RISCV::C_FSWSP:
      return 4;
    case RISCV::FSD:
    case RISCV::C_FSDSP:
      return 8;
    default:
      return 0;
    }
  }

  int getPopSize(const MCInst &Inst) const override {
    switch (Inst.getOpcode()) {
    case RISCV::LW:
    case RISCV::C_LWSP:   // 16-bit compressed
      return 4;
    case RISCV::LD:
    case RISCV::C_LDSP:   // 16-bit compressed (RV64 only)
      return 8;
    case RISCV::FLW:
    case RISCV::C_FLWSP:
      return 4;
    case RISCV::FLD:
    case RISCV::C_FLDSP:
      return 8;
    default:
      return 0;
    }
  }

  bool evaluateStackOffsetExpr(const MCInst &Inst, int64_t &Output,
                             std::pair<MCPhysReg, int64_t> Input1,
                             std::pair<MCPhysReg, int64_t> Input2) const override {

    auto getOperandVal = [&](MCPhysReg Reg) -> ErrorOr<int64_t> {
      if (Reg == Input1.first)
        return Input1.second;
      if (Reg == Input2.first)
        return Input2.second;
      return make_error_code(std::errc::result_out_of_range);
    };

    switch (Inst.getOpcode()) {
    default:
      return false;

    case RISCV::ADDI: {
      // addi rd, rs1, imm
      if (!Inst.getOperand(2).isImm())
        return false;
      MCPhysReg Src = Inst.getOperand(1).getReg();
      int64_t Imm = Inst.getOperand(2).getImm();
      if (ErrorOr<int64_t> InputVal = getOperandVal(Src)) {
        Output = *InputVal + Imm;
        return true;
      }
      return false;
    }

    case RISCV::LUI: {
      // 只处理在高位构造偏移的场景：lui + addi 通常要联用，这里单独不适用
      return false;
    }

    case RISCV::ADD: {
      // add rd, rs1, rs2 —— 需要知道两个寄存器值
      MCPhysReg R1 = Inst.getOperand(1).getReg();
      MCPhysReg R2 = Inst.getOperand(2).getReg();
      if (ErrorOr<int64_t> Val1 = getOperandVal(R1)) {
        if (ErrorOr<int64_t> Val2 = getOperandVal(R2)) {
          Output = *Val1 + *Val2;
          return true;
        }
      }
      return false;
    }

    case RISCV::SUB: {
      // sub rd, rs1, rs2
      MCPhysReg R1 = Inst.getOperand(1).getReg();
      MCPhysReg R2 = Inst.getOperand(2).getReg();
      if (ErrorOr<int64_t> Val1 = getOperandVal(R1)) {
        if (ErrorOr<int64_t> Val2 = getOperandVal(R2)) {
          Output = *Val1 - *Val2;
          return true;
        }
      }
      return false;
    }
    }

    return false;
  }

  bool reverseBranchCondition(MCInst &Inst, const MCSymbol *TBB,
                              MCContext *Ctx) const override {
    auto Opcode = getInvertedBranchOpcode(Inst.getOpcode());
    Inst.setOpcode(Opcode);
    return replaceBranchTarget(Inst, TBB, Ctx);
  }

  bool replaceBranchTarget(MCInst &Inst, const MCSymbol *TBB,
                           MCContext *Ctx) const override {
    assert((isCall(Inst) || isBranch(Inst)) && !isIndirectBranch(Inst) &&
           "Invalid instruction");

    unsigned SymOpIndex;
    auto Result = getSymbolRefOperandNum(Inst, SymOpIndex);
    (void)Result;
    assert(Result && "unimplemented branch");

    Inst.getOperand(SymOpIndex) = MCOperand::createExpr(
        MCSymbolRefExpr::create(TBB, MCSymbolRefExpr::VK_None, *Ctx));
    return true;
  }

  IndirectBranchType analyzeIndirectBranch(
      MCInst &Instruction, InstructionIterator Begin, InstructionIterator End,
      const unsigned PtrSize, MCInst *&MemLocInstr, unsigned &BaseRegNum,
      unsigned &IndexRegNum, int64_t &DispValue, const MCExpr *&DispExpr,
      MCInst *&PCRelBaseOut) const override {
    MemLocInstr = nullptr;
    BaseRegNum = 0;
    IndexRegNum = 0;
    DispValue = 0;
    DispExpr = nullptr;
    PCRelBaseOut = nullptr;
    return IndirectBranchType::UNKNOWN;
  }

//std::optional<Relocation>
//  createRelocation(const MCFixup &Fixup,
//                   const MCAsmBackend &MAB) const override {
//    const MCFixupKindInfo &FKI = MAB.getFixupKindInfo(Fixup.getKind());
//    errs() << "Fixup kind: " << Fixup.getKind()
//             << ", TargetOffset = " << FKI.TargetOffset
//             << ", TargetSize = " << FKI.TargetSize
//             << ", PCRel = " << ((FKI.Flags & MCFixupKindInfo::FKF_IsPCRel) != 0)
//             << "\n";
////    assert(FKI.TargetOffset == 0 && "0-bit relocation offset expected"); RISC-V不需要！X86独有
//    const uint64_t RelOffset = Fixup.getOffset();
//
//    uint64_t RelType;
//    if (FKI.Flags & MCFixupKindInfo::FKF_IsPCRel) {
//      switch (FKI.TargetSize) {
//      default:
//        return std::nullopt;
//      case 32: case 20:
//        RelType = ELF::R_RISCV_PCREL_HI20; break;
//      case 12: case 11:
//        RelType = ELF::R_RISCV_PCREL_LO12_I; break;
//      }
//    } else {
//      switch (FKI.TargetSize) {
//      default:
//        return std::nullopt;
//      case 32: RelType = ELF::R_RISCV_32; break;
//      case 64: RelType = ELF::R_RISCV_64; break;
//      case 20: RelType = ELF::R_RISCV_HI20; break;
//      case 12: RelType = ELF::R_RISCV_LO12_I; break;
////      case 12: RelType = ELF::R_RISCV_LO12_S; break;
//      }
//    }
//
//    uint64_t Addend = 0;
//    MCSymbol *Symbol = nullptr;
//    const MCExpr *ValueExpr = Fixup.getValue();
//
//    if (ValueExpr->getKind() == MCExpr::Binary) {
//      const auto *BinaryExpr = cast<MCBinaryExpr>(ValueExpr);
//      assert(BinaryExpr->getOpcode() == MCBinaryExpr::Add &&
//             "unexpected binary expression");
//      const MCExpr *LHS = BinaryExpr->getLHS();
//      if (LHS->getKind() == MCExpr::Constant) {
//        Addend = cast<MCConstantExpr>(LHS)->getValue();
//      } else if (LHS->getKind() == MCExpr::Binary) {
//        const auto *LHSBinaryExpr = cast<MCBinaryExpr>(LHS);
//        assert(LHSBinaryExpr->getOpcode() == MCBinaryExpr::Add &&
//               "unexpected binary expression");
//        const MCExpr *LLHS = LHSBinaryExpr->getLHS();
//        assert(LLHS->getKind() == MCExpr::SymbolRef && "unexpected LLHS");
//        Symbol = const_cast<MCSymbol *>(this->getTargetSymbol(LLHS));
//        const MCExpr *RLHS = LHSBinaryExpr->getRHS();
//        assert(RLHS->getKind() == MCExpr::Constant && "unexpected RLHS");
//        Addend = cast<MCConstantExpr>(RLHS)->getValue();
//      } else {
//        assert(LHS->getKind() == MCExpr::SymbolRef && "unexpected LHS");
//        Symbol = const_cast<MCSymbol *>(this->getTargetSymbol(LHS));
//      }
//      const MCExpr *RHS = BinaryExpr->getRHS();
//      assert(RHS->getKind() == MCExpr::Constant && "unexpected RHS");
//      Addend += cast<MCConstantExpr>(RHS)->getValue();
//    } else {
//      assert(ValueExpr->getKind() == MCExpr::SymbolRef && "unexpected value");
//      Symbol = const_cast<MCSymbol *>(this->getTargetSymbol(ValueExpr));
//    }
//
//    return Relocation({RelOffset, Symbol, RelType, Addend, 0});
//  }

  bool convertJmpToTailCall(MCInst &Inst) override {
    if (isTailCall(Inst))
      return false;

    switch (Inst.getOpcode()) {
    default:
      llvm_unreachable("unsupported tail call opcode");
    case RISCV::JAL:
    case RISCV::JALR:
    case RISCV::C_J:
    case RISCV::C_JR:
    case RISCV::C_JALR:  // 20250407
      break;
    }

    setTailCall(Inst);
    return true;
  }

  bool createReturn(MCInst &Inst) const override {
    // TODO "c.jr ra" when RVC is enabled
    Inst.setOpcode(RISCV::JALR);
    Inst.clear();
    Inst.addOperand(MCOperand::createReg(RISCV::X0));
    Inst.addOperand(MCOperand::createReg(RISCV::X1));
    Inst.addOperand(MCOperand::createImm(0));
    return true;
  }

  bool createUncondBranch(MCInst &Inst, const MCSymbol *TBB,
                          MCContext *Ctx) const override {
    Inst.setOpcode(RISCV::JAL);
    Inst.clear();
    Inst.addOperand(MCOperand::createReg(RISCV::X0));
    Inst.addOperand(MCOperand::createExpr(
        MCSymbolRefExpr::create(TBB, MCSymbolRefExpr::VK_None, *Ctx)));
    return true;
  }

  bool analyzeBranch(InstructionIterator Begin, InstructionIterator End,
                     const MCSymbol *&TBB, const MCSymbol *&FBB,
                     MCInst *&CondBranch,
                     MCInst *&UncondBranch) const override {
    auto I = End;

    while (I != Begin) {
      --I;

      // Ignore nops and CFIs
      if (isPseudo(*I) || isNoop(*I))
        continue;

      // Stop when we find the first non-terminator
      if (!isTerminator(*I) || isTailCall(*I) || !isBranch(*I))
        break;

      // Handle unconditional branches.
      if (isUnconditionalBranch(*I)) {
        // If any code was seen after this unconditional branch, we've seen
        // unreachable code. Ignore them.
        CondBranch = nullptr;
        UncondBranch = &*I;
        const MCSymbol *Sym = getTargetSymbol(*I);
        assert(Sym != nullptr &&
               "Couldn't extract BB symbol from jump operand");
        TBB = Sym;
        continue;
      }

      // Handle conditional branches and ignore indirect branches
      if (isIndirectBranch(*I))
        return false;

      if (CondBranch == nullptr) {
        const MCSymbol *TargetBB = getTargetSymbol(*I);
        if (TargetBB == nullptr) {
          // Unrecognized branch target
          return false;
        }
        FBB = TBB;
        TBB = TargetBB;
        CondBranch = &*I;
        continue;
      }

      llvm_unreachable("multiple conditional branches in one BB");
    }

    return true;
  }

  bool getSymbolRefOperandNum(const MCInst &Inst, unsigned &OpNum) const {
    switch (Inst.getOpcode()) {
    default:
      return false;
    case RISCV::C_J:
    case RISCV::C_JAL:
      OpNum = 0;
      return true;
    case RISCV::JAL:
    case RISCV::C_BEQZ:
    case RISCV::C_BNEZ:
      OpNum = 1;
      return true;
    case RISCV::BEQ:
    case RISCV::BGE:
    case RISCV::BGEU:
    case RISCV::BNE:
    case RISCV::BLT:
    case RISCV::BLTU:
      OpNum = 2;
      return true;
    }
  }

  const MCSymbol *getTargetSymbol(const MCExpr *Expr) const override {
    auto *RISCVExpr = dyn_cast<RISCVMCExpr>(Expr);
    if (RISCVExpr && RISCVExpr->getSubExpr())
      return getTargetSymbol(RISCVExpr->getSubExpr());

    auto *BinExpr = dyn_cast<MCBinaryExpr>(Expr);
    if (BinExpr)
      return getTargetSymbol(BinExpr->getLHS());

    auto *SymExpr = dyn_cast<MCSymbolRefExpr>(Expr);
    if (SymExpr && SymExpr->getKind() == MCSymbolRefExpr::VK_None)
      return &SymExpr->getSymbol();

    return nullptr;
  }

  const MCSymbol *getTargetSymbol(const MCInst &Inst,
                                  unsigned OpNum = 0) const override {
    if (!OpNum && !getSymbolRefOperandNum(Inst, OpNum))
      return nullptr;

    const MCOperand &Op = Inst.getOperand(OpNum);
    if (!Op.isExpr())
      return nullptr;

    return MCPlusBuilder::getTargetSymbol(Op.getExpr());
  }

  bool lowerTailCall(MCInst &Inst) override {
    removeAnnotation(Inst, MCPlus::MCAnnotation::kTailCall);
    if (getConditionalTailCall(Inst))
      unsetConditionalTailCall(Inst);
    return true;
  }

  uint64_t analyzePLTEntry(MCInst &Instruction, InstructionIterator Begin,
                           InstructionIterator End,
                           uint64_t BeginPC) const override {
    auto I = Begin;

    assert(I != End);
    auto &AUIPC = *I++;
    assert(AUIPC.getOpcode() == RISCV::AUIPC);
    assert(AUIPC.getOperand(0).getReg() == RISCV::X28);

    assert(I != End);
    auto &LD = *I++;
    assert(LD.getOpcode() == RISCV::LD);
    assert(LD.getOperand(0).getReg() == RISCV::X28);
    assert(LD.getOperand(1).getReg() == RISCV::X28);

    assert(I != End);
    auto &JALR = *I++;
    (void)JALR;
    assert(JALR.getOpcode() == RISCV::JALR);
    assert(JALR.getOperand(0).getReg() == RISCV::X6);
    assert(JALR.getOperand(1).getReg() == RISCV::X28);

    assert(I != End);
    auto &NOP = *I++;
    (void)NOP;
    assert(isNoop(NOP));

    assert(I == End);

    auto AUIPCOffset = AUIPC.getOperand(1).getImm() << 12;
    auto LDOffset = LD.getOperand(2).getImm();
    return BeginPC + AUIPCOffset + LDOffset;
  }

  bool replaceImmWithSymbolRef(MCInst &Inst, const MCSymbol *Symbol,
                               int64_t Addend, MCContext *Ctx, int64_t &Value,
                               uint64_t RelType) const override {
    unsigned ImmOpNo = -1U;

    for (unsigned Index = 0; Index < MCPlus::getNumPrimeOperands(Inst);
         ++Index) {
      if (Inst.getOperand(Index).isImm()) {
        ImmOpNo = Index;
        break;
      }
    }

    if (ImmOpNo == -1U)
      return false;

    Value = Inst.getOperand(ImmOpNo).getImm();
    setOperandToSymbolRef(Inst, ImmOpNo, Symbol, Addend, Ctx, RelType);
    return true;
  }

  const MCExpr *getTargetExprFor(MCInst &Inst, const MCExpr *Expr,
                                 MCContext &Ctx,
                                 uint64_t RelType) const override {
    switch (RelType) {
    default:
      return Expr;
    case ELF::R_RISCV_GOT_HI20:
    case ELF::R_RISCV_TLS_GOT_HI20:
    case ELF::R_RISCV_TLS_GD_HI20:
    // The GOT is reused so no need to create GOT relocations
    case ELF::R_RISCV_PCREL_HI20:
      return RISCVMCExpr::create(Expr, RISCVMCExpr::VK_RISCV_PCREL_HI, Ctx);
    case ELF::R_RISCV_PCREL_LO12_I:
      return RISCVMCExpr::create(Expr, RISCVMCExpr::VK_RISCV_PCREL_LO, Ctx);
    case ELF::R_RISCV_CALL:
      return RISCVMCExpr::create(Expr, RISCVMCExpr::VK_RISCV_CALL, Ctx);
    case ELF::R_RISCV_CALL_PLT:
      return RISCVMCExpr::create(Expr, RISCVMCExpr::VK_RISCV_CALL_PLT, Ctx);
    case ELF::R_RISCV_HI20:
      return RISCVMCExpr::create(Expr, RISCVMCExpr::VK_RISCV_HI, Ctx);
    case ELF::R_RISCV_LO12_I:
      return RISCVMCExpr::create(Expr, RISCVMCExpr::VK_RISCV_LO, Ctx);
    case ELF::R_RISCV_LO12_S:
      return RISCVMCExpr::create(Expr, RISCVMCExpr::VK_RISCV_LO, Ctx);
    }
  }

  bool evaluateMemOperandTarget(const MCInst &Inst, uint64_t &Target,
                                uint64_t Address,
                                uint64_t Size) const override {
    return false;
  }

  bool isCallAuipc(const MCInst &Inst) const {
    if (Inst.getOpcode() != RISCV::AUIPC)
      return false;

    const auto &ImmOp = Inst.getOperand(1);
    if (!ImmOp.isExpr())
      return false;

    const auto *ImmExpr = ImmOp.getExpr();
    if (!isa<RISCVMCExpr>(ImmExpr))
      return false;

    switch (cast<RISCVMCExpr>(ImmExpr)->getKind()) {
    default:
      return false;
    case RISCVMCExpr::VK_RISCV_CALL:
    case RISCVMCExpr::VK_RISCV_CALL_PLT:
      return true;
    }
  }

  bool isRISCVCall(const MCInst &First, const MCInst &Second) const override {
    if (!isCallAuipc(First))
      return false;

    assert(Second.getOpcode() == RISCV::JALR);
    return true;
  }
};

} // end anonymous namespace

namespace llvm {
namespace bolt {

MCPlusBuilder *createRISCVMCPlusBuilder(const MCInstrAnalysis *Analysis,
                                        const MCInstrInfo *Info,
                                        const MCRegisterInfo *RegInfo) {
  return new RISCVMCPlusBuilder(Analysis, Info, RegInfo);
}

} // namespace bolt
} // namespace llvm
