//===- ParallelDSP.cpp - Parallel DSP Pass --------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Armv6 introduced instructions to perform 32-bit SIMD operations. The
/// purpose of this pass is do some IR pattern matching to create ACLE
/// DSP intrinsics, which map on these 32-bit SIMD operations.
/// This pass runs only when unaligned accesses is supported/enabled.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "ARM.h"
#include "ARMSubtarget.h"

using namespace llvm;
using namespace PatternMatch;

#define DEBUG_TYPE "arm-parallel-dsp"

STATISTIC(NumSMLAD , "Number of smlad instructions generated");

static cl::opt<bool>
DisableParallelDSP("disable-arm-parallel-dsp", cl::Hidden, cl::init(false),
                   cl::desc("Disable the ARM Parallel DSP pass"));

namespace {
  struct OpChain;
  struct BinOpChain;
  struct Reduction;

  using OpChainList     = SmallVector<std::unique_ptr<OpChain>, 8>;
  using ReductionList   = SmallVector<Reduction, 8>;
  using ValueList       = SmallVector<Value*, 8>;
  using MemInstList     = SmallVector<Instruction*, 8>;
  using LoadInstList    = SmallVector<LoadInst*, 8>;
  using PMACPair        = std::pair<BinOpChain*,BinOpChain*>;
  using PMACPairList    = SmallVector<PMACPair, 8>;
  using Instructions    = SmallVector<Instruction*,16>;
  using MemLocList      = SmallVector<MemoryLocation, 4>;

  struct OpChain {
    Instruction   *Root;
    ValueList     AllValues;
    MemInstList   VecLd;    // List of all sequential load instructions.
    LoadInstList  Loads;    // List of all load instructions.
    MemLocList    MemLocs;  // All memory locations read by this tree.
    bool          ReadOnly = true;

    OpChain(Instruction *I, ValueList &vl) : Root(I), AllValues(vl) { }
    virtual ~OpChain() = default;

    void SetMemoryLocations() {
      const auto Size = MemoryLocation::UnknownSize;
      for (auto *V : AllValues) {
        if (auto *I = dyn_cast<Instruction>(V)) {
          if (I->mayWriteToMemory())
            ReadOnly = false;
          if (auto *Ld = dyn_cast<LoadInst>(V)) {
            MemLocs.push_back(MemoryLocation(Ld->getPointerOperand(), Size));
            Loads.push_back(Ld);
          }
        }
      }
    }

    unsigned size() const { return AllValues.size(); }
  };

  // 'BinOpChain' and 'Reduction' are just some bookkeeping data structures.
  // 'Reduction' contains the phi-node and accumulator statement from where we
  // start pattern matching, and 'BinOpChain' the multiplication
  // instructions that are candidates for parallel execution.
  struct BinOpChain : public OpChain {
    ValueList     LHS;      // List of all (narrow) left hand operands.
    ValueList     RHS;      // List of all (narrow) right hand operands.
    bool Exchange = false;

    BinOpChain(Instruction *I, ValueList &lhs, ValueList &rhs) :
      OpChain(I, lhs), LHS(lhs), RHS(rhs) {
        for (auto *V : RHS)
          AllValues.push_back(V);
      }
  };

  struct Reduction {
    PHINode         *Phi;             // The Phi-node from where we start
                                      // pattern matching.
    Instruction     *AccIntAdd;       // The accumulating integer add statement,
                                      // i.e, the reduction statement.

    OpChainList     MACCandidates;    // The MAC candidates associated with
                                      // this reduction statement.
    Reduction (PHINode *P, Instruction *Acc) : Phi(P), AccIntAdd(Acc) { };
  };

  class ARMParallelDSP : public LoopPass {
    ScalarEvolution   *SE;
    AliasAnalysis     *AA;
    TargetLibraryInfo *TLI;
    DominatorTree     *DT;
    LoopInfo          *LI;
    Loop              *L;
    const DataLayout  *DL;
    Module            *M;

    bool InsertParallelMACs(Reduction &Reduction, PMACPairList &PMACPairs);
    bool AreSequentialLoads(LoadInst *Ld0, LoadInst *Ld1, MemInstList &VecMem);
    PMACPairList CreateParallelMACPairs(OpChainList &Candidates);
    Instruction *CreateSMLADCall(LoadInst *VecLd0, LoadInst *VecLd1,
                                 Instruction *Acc, bool Exchange,
                                 Instruction *InsertAfter);

    /// Try to match and generate: SMLAD, SMLADX - Signed Multiply Accumulate
    /// Dual performs two signed 16x16-bit multiplications. It adds the
    /// products to a 32-bit accumulate operand. Optionally, the instruction can
    /// exchange the halfwords of the second operand before performing the
    /// arithmetic.
    bool MatchSMLAD(Function &F);
    bool MatchTopBottomMuls(BasicBlock *LoopBody);

  public:
    static char ID;

    ARMParallelDSP() : LoopPass(ID) { }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      LoopPass::getAnalysisUsage(AU);
      AU.addRequired<AssumptionCacheTracker>();
      AU.addRequired<ScalarEvolutionWrapperPass>();
      AU.addRequired<AAResultsWrapperPass>();
      AU.addRequired<TargetLibraryInfoWrapperPass>();
      AU.addRequired<LoopInfoWrapperPass>();
      AU.addRequired<DominatorTreeWrapperPass>();
      AU.addRequired<TargetPassConfig>();
      AU.addPreserved<LoopInfoWrapperPass>();
      AU.setPreservesCFG();
    }

    bool runOnLoop(Loop *TheLoop, LPPassManager &) override {
      if (DisableParallelDSP)
        return false;
      L = TheLoop;
      SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
      AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
      TLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
      DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
      LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
      auto &TPC = getAnalysis<TargetPassConfig>();

      BasicBlock *Header = TheLoop->getHeader();
      if (!Header)
        return false;

      // TODO: We assume the loop header and latch to be the same block.
      // This is not a fundamental restriction, but lifting this would just
      // require more work to do the transformation and then patch up the CFG.
      if (Header != TheLoop->getLoopLatch()) {
        LLVM_DEBUG(dbgs() << "The loop header is not the loop latch: not "
                             "running pass ARMParallelDSP\n");
        return false;
      }

      Function &F = *Header->getParent();
      M = F.getParent();
      DL = &M->getDataLayout();

      auto &TM = TPC.getTM<TargetMachine>();
      auto *ST = &TM.getSubtarget<ARMSubtarget>(F);

      if (!ST->allowsUnalignedMem()) {
        LLVM_DEBUG(dbgs() << "Unaligned memory access not supported: not "
                             "running pass ARMParallelDSP\n");
        return false;
      }

      if (!ST->hasDSP()) {
        LLVM_DEBUG(dbgs() << "DSP extension not enabled: not running pass "
                             "ARMParallelDSP\n");
        return false;
      }

      LoopAccessInfo LAI(L, SE, TLI, AA, DT, LI);
      bool Changes = false;

      LLVM_DEBUG(dbgs() << "\n== Parallel DSP pass ==\n");
      LLVM_DEBUG(dbgs() << " - " << F.getName() << "\n\n");
      Changes = MatchSMLAD(F);
      if (!Changes)
        Changes = MatchTopBottomMuls(Header);
      return Changes;
    }
  };
}

// MaxBitwidth: the maximum supported bitwidth of the elements in the DSP
// instructions, which is set to 16. So here we should collect all i8 and i16
// narrow operations.
// TODO: we currently only collect i16, and will support i8 later, so that's
// why we check that types are equal to MaxBitWidth, and not <= MaxBitWidth.
template<unsigned MaxBitWidth>
static bool IsNarrowSequence(Value *V, ValueList &VL) {
  LLVM_DEBUG(dbgs() << "Is narrow sequence? "; V->dump());
  ConstantInt *CInt;

  if (match(V, m_ConstantInt(CInt))) {
    // TODO: if a constant is used, it needs to fit within the bit width.
    return false;
  }

  auto *I = dyn_cast<Instruction>(V);
  if (!I)
   return false;

  Value *Val, *LHS, *RHS;
  if (match(V, m_Trunc(m_Value(Val)))) {
    if (cast<TruncInst>(I)->getDestTy()->getIntegerBitWidth() == MaxBitWidth)
      return IsNarrowSequence<MaxBitWidth>(Val, VL);
  } else if (match(V, m_Add(m_Value(LHS), m_Value(RHS)))) {
    // TODO: we need to implement sadd16/sadd8 for this, which enables to
    // also do the rewrite for smlad8.ll, but it is unsupported for now.
    LLVM_DEBUG(dbgs() << "No, unsupported Op:\t"; I->dump());
    return false;
  } else if (match(V, m_ZExtOrSExt(m_Value(Val)))) {
    if (cast<CastInst>(I)->getSrcTy()->getIntegerBitWidth() != MaxBitWidth) {
      LLVM_DEBUG(dbgs() << "No, wrong SrcTy size: " <<
        cast<CastInst>(I)->getSrcTy()->getIntegerBitWidth() << "\n");
      return false;
    }

    if (match(Val, m_Load(m_Value()))) {
      LLVM_DEBUG(dbgs() << "Yes, found narrow Load:\t"; Val->dump());
      VL.push_back(Val);
      VL.push_back(I);
      return true;
    }
  }
  LLVM_DEBUG(dbgs() << "No, unsupported Op:\t"; I->dump());
  return false;
}

// Element-by-element comparison of Value lists returning true if they are
// instructions with the same opcode or constants with the same value.
static bool AreSymmetrical(const ValueList &VL0,
                           const ValueList &VL1) {
  if (VL0.size() != VL1.size()) {
    LLVM_DEBUG(dbgs() << "Muls are mismatching operand list lengths: "
                      << VL0.size() << " != " << VL1.size() << "\n");
    return false;
  }

  const unsigned Pairs = VL0.size();
  LLVM_DEBUG(dbgs() << "Number of operand pairs: " << Pairs << "\n");

  for (unsigned i = 0; i < Pairs; ++i) {
    const Value *V0 = VL0[i];
    const Value *V1 = VL1[i];
    const auto *Inst0 = dyn_cast<Instruction>(V0);
    const auto *Inst1 = dyn_cast<Instruction>(V1);

    LLVM_DEBUG(dbgs() << "Pair " << i << ":\n";
               dbgs() << "mul1: "; V0->dump();
               dbgs() << "mul2: "; V1->dump());

    if (!Inst0 || !Inst1)
      return false;

    if (Inst0->isSameOperationAs(Inst1)) {
      LLVM_DEBUG(dbgs() << "OK: same operation found!\n");
      continue;
    }

    const APInt *C0, *C1;
    if (!(match(V0, m_APInt(C0)) && match(V1, m_APInt(C1)) && C0 == C1))
      return false;
  }

  LLVM_DEBUG(dbgs() << "OK: found symmetrical operand lists.\n");
  return true;
}

template<typename MemInst>
static bool AreSequentialAccesses(MemInst *MemOp0, MemInst *MemOp1,
                                  MemInstList &VecMem, const DataLayout &DL,
                                  ScalarEvolution &SE) {
  if (!MemOp0->isSimple() || !MemOp1->isSimple()) {
    LLVM_DEBUG(dbgs() << "No, not touching volatile access\n");
    return false;
  }
  if (isConsecutiveAccess(MemOp0, MemOp1, DL, SE)) {
    VecMem.clear();
    VecMem.push_back(MemOp0);
    VecMem.push_back(MemOp1);
    LLVM_DEBUG(dbgs() << "OK: accesses are consecutive.\n");
    return true;
  }
  LLVM_DEBUG(dbgs() << "No, accesses aren't consecutive.\n");
  return false;
}

bool ARMParallelDSP::AreSequentialLoads(LoadInst *Ld0, LoadInst *Ld1,
                                        MemInstList &VecMem) {
  if (!Ld0 || !Ld1)
    return false;

  LLVM_DEBUG(dbgs() << "Are consecutive loads:\n";
    dbgs() << "Ld0:"; Ld0->dump();
    dbgs() << "Ld1:"; Ld1->dump();
  );

  if (!Ld0->hasOneUse() || !Ld1->hasOneUse()) {
    LLVM_DEBUG(dbgs() << "No, load has more than one use.\n");
    return false;
  }

  return AreSequentialAccesses<LoadInst>(Ld0, Ld1, VecMem, *DL, *SE);
}

PMACPairList
ARMParallelDSP::CreateParallelMACPairs(OpChainList &Candidates) {
  const unsigned Elems = Candidates.size();
  PMACPairList PMACPairs;

  if (Elems < 2)
    return PMACPairs;

  SmallPtrSet<const Instruction*, 4> Paired;
  for (unsigned i = 0; i < Elems; ++i) {
    BinOpChain *PMul0 = static_cast<BinOpChain*>(Candidates[i].get());
    if (Paired.count(PMul0->Root))
      continue;

    for (unsigned j = 0; j < Elems; ++j) {
      if (i == j)
        continue;

      BinOpChain *PMul1 = static_cast<BinOpChain*>(Candidates[j].get());
      if (Paired.count(PMul1->Root))
        continue;

      const Instruction *Mul0 = PMul0->Root;
      const Instruction *Mul1 = PMul1->Root;
      if (Mul0 == Mul1)
        continue;

      assert(PMul0 != PMul1 && "expected different chains");

      LLVM_DEBUG(dbgs() << "\nCheck parallel muls:\n";
                 dbgs() << "- "; Mul0->dump();
                 dbgs() << "- "; Mul1->dump());

      const ValueList &Mul0_LHS = PMul0->LHS;
      const ValueList &Mul0_RHS = PMul0->RHS;
      const ValueList &Mul1_LHS = PMul1->LHS;
      const ValueList &Mul1_RHS = PMul1->RHS;

      if (!AreSymmetrical(Mul0_LHS, Mul1_LHS) ||
          !AreSymmetrical(Mul0_RHS, Mul1_RHS))
        continue;

      LLVM_DEBUG(dbgs() << "OK: mul operands list match:\n");
      // The first elements of each vector should be loads with sexts. If we
      // find that its two pairs of consecutive loads, then these can be
      // transformed into two wider loads and the users can be replaced with
      // DSP intrinsics.
      bool Found = false;
      for (unsigned x = 0; x < Mul0_LHS.size(); x += 2) {
        auto *Ld0 = dyn_cast<LoadInst>(Mul0_LHS[x]);
        auto *Ld1 = dyn_cast<LoadInst>(Mul1_LHS[x]);
        auto *Ld2 = dyn_cast<LoadInst>(Mul0_RHS[x]);
        auto *Ld3 = dyn_cast<LoadInst>(Mul1_RHS[x]);

        if (!Ld0 || !Ld1 || !Ld2 || !Ld3)
          continue;

        LLVM_DEBUG(dbgs() << "Looking at operands " << x << ":\n"
                   << "\t Ld0: " << *Ld0 << "\n"
                   << "\t Ld1: " << *Ld1 << "\n"
                   << "and operands " << x + 2 << ":\n"
                   << "\t Ld2: " << *Ld2 << "\n"
                   << "\t Ld3: " << *Ld3 << "\n");

        if (AreSequentialLoads(Ld0, Ld1, PMul0->VecLd)) {
          if (AreSequentialLoads(Ld2, Ld3, PMul1->VecLd)) {
            LLVM_DEBUG(dbgs() << "OK: found two pairs of parallel loads!\n");
            PMACPairs.push_back(std::make_pair(PMul0, PMul1));
            Found = true;
          } else if (AreSequentialLoads(Ld3, Ld2, PMul1->VecLd)) {
            LLVM_DEBUG(dbgs() << "OK: found two pairs of parallel loads!\n");
            LLVM_DEBUG(dbgs() << "    exchanging Ld2 and Ld3\n");
            PMul1->Exchange = true;
            PMACPairs.push_back(std::make_pair(PMul0, PMul1));
            Found = true;
          }
        } else if (AreSequentialLoads(Ld1, Ld0, PMul0->VecLd)) {
          if (AreSequentialLoads(Ld2, Ld3, PMul1->VecLd)) {
            LLVM_DEBUG(dbgs() << "OK: found two pairs of parallel loads!\n");
            LLVM_DEBUG(dbgs() << "    exchanging Ld0 and Ld1\n");
            LLVM_DEBUG(dbgs() << "    and swapping muls\n");
            PMul0->Exchange = true;
            // Only the second operand can be exchanged, so swap the muls.
            PMACPairs.push_back(std::make_pair(PMul1, PMul0));
            Found = true;
          }
        }
      }
      if (Found) {
        Paired.insert(Mul0);
        Paired.insert(Mul1);
        break;
      }
    }
  }
  return PMACPairs;
}

bool ARMParallelDSP::InsertParallelMACs(Reduction &Reduction,
                                        PMACPairList &PMACPairs) {
  Instruction *Acc = Reduction.Phi;
  Instruction *InsertAfter = Reduction.AccIntAdd;

  for (auto &Pair : PMACPairs) {
    BinOpChain *PMul0 = Pair.first;
    BinOpChain *PMul1 = Pair.second;
    LLVM_DEBUG(dbgs() << "Found parallel MACs!!\n";
               dbgs() << "- "; PMul0->Root->dump();
               dbgs() << "- "; PMul1->Root->dump());

    auto *VecLd0 = cast<LoadInst>(PMul0->VecLd[0]);
    auto *VecLd1 = cast<LoadInst>(PMul1->VecLd[0]);
    Acc = CreateSMLADCall(VecLd0, VecLd1, Acc, PMul1->Exchange, InsertAfter);
    InsertAfter = Acc;
  }

  if (Acc != Reduction.Phi) {
    LLVM_DEBUG(dbgs() << "Replace Accumulate: "; Acc->dump());
    Reduction.AccIntAdd->replaceAllUsesWith(Acc);
    return true;
  }
  return false;
}

static void MatchReductions(Function &F, Loop *TheLoop, BasicBlock *Header,
                            ReductionList &Reductions) {
  RecurrenceDescriptor RecDesc;
  const bool HasFnNoNaNAttr =
    F.getFnAttribute("no-nans-fp-math").getValueAsString() == "true";
  const BasicBlock *Latch = TheLoop->getLoopLatch();

  // We need a preheader as getIncomingValueForBlock assumes there is one.
  if (!TheLoop->getLoopPreheader()) {
    LLVM_DEBUG(dbgs() << "No preheader found, bailing out\n");
    return;
  }

  for (PHINode &Phi : Header->phis()) {
    const auto *Ty = Phi.getType();
    if (!Ty->isIntegerTy(32) && !Ty->isIntegerTy(64))
      continue;

    const bool IsReduction =
      RecurrenceDescriptor::AddReductionVar(&Phi,
                                            RecurrenceDescriptor::RK_IntegerAdd,
                                            TheLoop, HasFnNoNaNAttr, RecDesc);
    if (!IsReduction)
      continue;

    Instruction *Acc = dyn_cast<Instruction>(Phi.getIncomingValueForBlock(Latch));
    if (!Acc)
      continue;

    Reductions.push_back(Reduction(&Phi, Acc));
  }

  LLVM_DEBUG(
    dbgs() << "\nAccumulating integer additions (reductions) found:\n";
    for (auto &R : Reductions) {
      dbgs() << "-  "; R.Phi->dump();
      dbgs() << "-> "; R.AccIntAdd->dump();
    }
  );
}

static void AddMulCandidate(OpChainList &Candidates,
                            Instruction *Mul,
                            Value *MulOp0, Value *MulOp1) {
  LLVM_DEBUG(dbgs() << "OK, found mul:\t"; Mul->dump());
  assert(Mul->getOpcode() == Instruction::Mul &&
         "expected mul instruction");
  ValueList LHS;
  ValueList RHS;
  if (IsNarrowSequence<16>(MulOp0, LHS) &&
      IsNarrowSequence<16>(MulOp1, RHS)) {
    LLVM_DEBUG(dbgs() << "OK, found narrow mul: "; Mul->dump());
    Candidates.push_back(make_unique<BinOpChain>(Mul, LHS, RHS));
  }
}

static void MatchParallelMACSequences(Reduction &R,
                                      OpChainList &Candidates) {
  Instruction *Acc = R.AccIntAdd;
  LLVM_DEBUG(dbgs() << "\n- Analysing:\t" << *Acc);

  // Returns false to signal the search should be stopped.
  std::function<bool(Value*)> Match =
    [&Candidates, &Match](Value *V) -> bool {

    auto *I = dyn_cast<Instruction>(V);
    if (!I)
      return false;

    Value *MulOp0, *MulOp1;

    switch (I->getOpcode()) {
    case Instruction::Add:
      if (Match(I->getOperand(0)) || (Match(I->getOperand(1))))
        return true;
      break;
    case Instruction::Mul:
      if (match (I, (m_Mul(m_Value(MulOp0), m_Value(MulOp1))))) {
        AddMulCandidate(Candidates, I, MulOp0, MulOp1);
        return false;
      }
      break;
    case Instruction::SExt:
      if (match (I, (m_SExt(m_Mul(m_Value(MulOp0), m_Value(MulOp1)))))) {
        Instruction *Mul = cast<Instruction>(I->getOperand(0));
        AddMulCandidate(Candidates, Mul, MulOp0, MulOp1);
        return false;
      }
      break;
    }
    return false;
  };

  while (Match (Acc));
  LLVM_DEBUG(dbgs() << "Finished matching MAC sequences, found "
             << Candidates.size() << " candidates.\n");
}

// Collects all instructions that are not part of the MAC chains, which is the
// set of instructions that can potentially alias with the MAC operands.
static void AliasCandidates(BasicBlock *Header, Instructions &Reads,
                            Instructions &Writes) {
  for (auto &I : *Header) {
    if (I.mayReadFromMemory())
      Reads.push_back(&I);
    if (I.mayWriteToMemory())
      Writes.push_back(&I);
  }
}

// Check whether statements in the basic block that write to memory alias with
// the memory locations accessed by the MAC-chains.
// TODO: we need the read statements when we accept more complicated chains.
static bool AreAliased(AliasAnalysis *AA, Instructions &Reads,
                       Instructions &Writes, OpChainList &Candidates) {
  LLVM_DEBUG(dbgs() << "Alias checks:\n");
  for (auto &Candidate : Candidates) {
    LLVM_DEBUG(dbgs() << "mul: "; Candidate->Root->dump());
    Candidate->SetMemoryLocations();

    // At the moment, we allow only simple chains that only consist of reads,
    // accumulate their result with an integer add, and thus that don't write
    // memory, and simply bail if they do.
    if (!Candidate->ReadOnly)
      return true;

    // Now for all writes in the basic block, check that they don't alias with
    // the memory locations accessed by our MAC-chain:
    for (auto *I : Writes) {
      LLVM_DEBUG(dbgs() << "- "; I->dump());
      assert(Candidate->MemLocs.size() >= 2 && "expecting at least 2 memlocs");
      for (auto &MemLoc : Candidate->MemLocs) {
        if (isModOrRefSet(intersectModRef(AA->getModRefInfo(I, MemLoc),
                                          ModRefInfo::ModRef))) {
          LLVM_DEBUG(dbgs() << "Yes, aliases found\n");
          return true;
        }
      }
    }
  }

  LLVM_DEBUG(dbgs() << "OK: no aliases found!\n");
  return false;
}

static bool CheckMulMemory(OpChainList &Candidates) {
  for (auto &C : Candidates) {
    // A mul has 2 operands, and a narrow op consist of sext and a load; thus
    // we expect at least 4 items in this operand value list.
    if (C->size() < 4) {
      LLVM_DEBUG(dbgs() << "Operand list too short.\n");
      return false;
    }
    ValueList &LHS = static_cast<BinOpChain*>(C.get())->LHS;
    ValueList &RHS = static_cast<BinOpChain*>(C.get())->RHS;

    // Use +=2 to skip over the expected extend instructions.
    for (unsigned i = 0, e = LHS.size(); i < e; i += 2) {
      if (!isa<LoadInst>(LHS[i]) || !isa<LoadInst>(RHS[i]))
        return false;
    }
  }
  return true;
}

static LoadInst *CreateLoadIns(IRBuilder<NoFolder> &IRB, LoadInst *BaseLoad,
                               const Type *LoadTy) {
  const unsigned AddrSpace = BaseLoad->getPointerAddressSpace();
 
  Value *VecPtr = IRB.CreateBitCast(BaseLoad->getPointerOperand(),
                                     LoadTy->getPointerTo(AddrSpace));
  return IRB.CreateAlignedLoad(VecPtr, BaseLoad->getAlignment());
}

/// Attempt to widen loads and use smulbb, smulbt, smultb and smultt muls.
// TODO: This, like smlad generation, expects the leave operands to be loads
// that are sign extended. We should be able to handle scalar values as well
// performing these muls on word x half types to generate smulwb and smulwt.
bool ARMParallelDSP::MatchTopBottomMuls(BasicBlock *LoopBody) {
  LLVM_DEBUG(dbgs() << "Attempting to find BT|TB muls.\n");

  OpChainList Candidates;
  for (auto &I : *LoopBody) {
    if (I.getOpcode() == Instruction::Mul) {
      Type *Ty = I.getType();
      if (Ty->isIntegerTy() &&
          (Ty->getScalarSizeInBits() == 32 ||
           Ty->getScalarSizeInBits() == 64))
      AddMulCandidate(Candidates, &I, I.getOperand(0), I.getOperand(1));
    }
  }

  if (Candidates.empty())
    return false;

  Instructions Reads;
  Instructions Writes;
  AliasCandidates(LoopBody, Reads, Writes);

  if (AreAliased(AA, Reads, Writes, Candidates))
    return false;

  DenseMap<LoadInst*, Instruction*> LoadUsers;
  DenseMap<LoadInst*, LoadInst*> SeqLoads;
  SmallPtrSet<LoadInst*, 8> OffsetLoads;

  for (unsigned i = 0; i < Candidates.size(); ++i) {
    for (unsigned j = 0; j < Candidates.size(); ++j) {
      if (i == j)
        continue;

      OpChain *MulChain0 = Candidates[i].get();
      OpChain *MulChain1 = Candidates[j].get();

      for (auto *Ld0 : MulChain0->Loads) {
        if (SeqLoads.count(Ld0) || OffsetLoads.count(Ld0))
          continue;

        for (auto *Ld1 : MulChain1->Loads) {
          if (SeqLoads.count(Ld1) || OffsetLoads.count(Ld1))
            continue;

          MemInstList VecMem;
          if (AreSequentialLoads(Ld0, Ld1, VecMem)) {
            SeqLoads[Ld0] = Ld1;
            OffsetLoads.insert(Ld1);
            LoadUsers[Ld0] = MulChain0->Root;
            LoadUsers[Ld1] = MulChain1->Root;
          }
        }
      }
    }
  }

  if (SeqLoads.empty())
    return false;

  IRBuilder<NoFolder> IRB(LoopBody);
  const Type *Ty = IntegerType::get(M->getContext(), 32);

  // We know that at least one of the operands is a SExt of Ld.
  auto GetSExt = [](Instruction *I, LoadInst *Ld, unsigned OpIdx) -> Instruction* {
    if (!isa<Instruction>(I->getOperand(OpIdx)))
      return nullptr;

    Value *SExt = nullptr;
    if (cast<Instruction>(I->getOperand(OpIdx))->getOperand(0) == Ld)
      SExt = I->getOperand(0);
    else
      SExt = I->getOperand(1);

    return cast<Instruction>(SExt);
  };

  LLVM_DEBUG(dbgs() << "Found some sequential loads, now widening:\n");
  for (auto &Pair : SeqLoads) {
    LoadInst *BaseLd = Pair.first;
    LoadInst *OffsetLd = Pair.second;
    IRB.SetInsertPoint(BaseLd);
    LoadInst *WideLd = CreateLoadIns(IRB, BaseLd, Ty);
    LLVM_DEBUG(dbgs() << " - with base load: " << *BaseLd << "\n");
    LLVM_DEBUG(dbgs() << " - created wide load: " << *WideLd << "\n");
    Instruction *BaseUser = LoadUsers[BaseLd];
    Instruction *OffsetUser = LoadUsers[OffsetLd];

    Instruction *BaseSExt = GetSExt(BaseUser, BaseLd, 0);
    if (!BaseSExt)
      BaseSExt = GetSExt(BaseUser, BaseLd, 1);
    Instruction *OffsetSExt = GetSExt(OffsetUser, OffsetLd, 0);
    if (!OffsetSExt)
      OffsetSExt = GetSExt(OffsetUser, OffsetLd, 1);

    assert((BaseSExt && OffsetSExt) && "failed to find SExts");

    // BaseUser needs to: (asr (shl WideLoad, 16), 16)
    // OffsetUser needs to: (asr WideLoad, 16)
    auto *Shl = cast<Instruction>(IRB.CreateShl(WideLd, 16));
    auto *Bottom = cast<Instruction>(IRB.CreateAShr(Shl, 16));
    auto *Top = cast<Instruction>(IRB.CreateAShr(WideLd, 16));
    BaseUser->replaceUsesOfWith(BaseSExt, Bottom);
    OffsetUser->replaceUsesOfWith(OffsetSExt, Top);

    BaseSExt->eraseFromParent();
    OffsetSExt->eraseFromParent();
    BaseLd->eraseFromParent();
    OffsetLd->eraseFromParent();
  }
  LLVM_DEBUG(dbgs() << "Block after top bottom mul replacements:\n"
             << *LoopBody << "\n");
  return true;
}

// Loop Pass that needs to identify integer add/sub reductions of 16-bit vector
// multiplications.
// To use SMLAD:
// 1) we first need to find integer add reduction PHIs,
// 2) then from the PHI, look for this pattern:
//
// acc0 = phi i32 [0, %entry], [%acc1, %loop.body]
// ld0 = load i16
// sext0 = sext i16 %ld0 to i32
// ld1 = load i16
// sext1 = sext i16 %ld1 to i32
// mul0 = mul %sext0, %sext1
// ld2 = load i16
// sext2 = sext i16 %ld2 to i32
// ld3 = load i16
// sext3 = sext i16 %ld3 to i32
// mul1 = mul i32 %sext2, %sext3
// add0 = add i32 %mul0, %acc0
// acc1 = add i32 %add0, %mul1
//
// Which can be selected to:
//
// ldr.h r0
// ldr.h r1
// smlad r2, r0, r1, r2
//
// If constants are used instead of loads, these will need to be hoisted
// out and into a register.
//
// If loop invariants are used instead of loads, these need to be packed
// before the loop begins.
//
bool ARMParallelDSP::MatchSMLAD(Function &F) {
  BasicBlock *Header = L->getHeader();
  LLVM_DEBUG(dbgs() << "= Matching SMLAD =\n";
             dbgs() << "Header block:\n"; Header->dump();
             dbgs() << "Loop info:\n\n"; L->dump());

  ReductionList Reductions;
  MatchReductions(F, L, Header, Reductions);
  if (Reductions.empty())
    return false;

  for (auto &R : Reductions) {
    OpChainList MACCandidates;
    MatchParallelMACSequences(R, MACCandidates);
    if (!CheckMulMemory(MACCandidates))
      continue;

    R.MACCandidates = std::move(MACCandidates);

    LLVM_DEBUG(dbgs() << "MAC candidates:\n";
      for (auto &M : R.MACCandidates)
        M->Root->dump();
      dbgs() << "\n";);
  }

  // Collect all instructions that may read or write memory. Our alias
  // analysis checks bail out if any of these instructions aliases with an
  // instruction from the MAC-chain.
  Instructions Reads, Writes;
  AliasCandidates(Header, Reads, Writes);

  bool Changed = false;
  for (auto &R : Reductions) {
    if (AreAliased(AA, Reads, Writes, R.MACCandidates))
      return false;
    PMACPairList PMACPairs = CreateParallelMACPairs(R.MACCandidates);
    Changed |= InsertParallelMACs(R, PMACPairs);
  }

  LLVM_DEBUG(if (Changed) dbgs() << "Header block:\n"; Header->dump(););
  return Changed;
}

Instruction *ARMParallelDSP::CreateSMLADCall(LoadInst *VecLd0, LoadInst *VecLd1,
                                             Instruction *Acc, bool Exchange,
                                             Instruction *InsertAfter) {
  LLVM_DEBUG(dbgs() << "Create SMLAD intrinsic using:\n"
             << "- " << *VecLd0 << "\n"
             << "- " << *VecLd1 << "\n"
             << "- " << *Acc << "\n"
             << "Exchange: " << Exchange << "\n");

  IRBuilder<NoFolder> Builder(InsertAfter->getParent(),
                              ++BasicBlock::iterator(InsertAfter));

  // Replace the reduction chain with an intrinsic call
  const Type *Ty = IntegerType::get(M->getContext(), 32);
  LoadInst *NewLd0 = CreateLoadIns(Builder, &VecLd0[0], Ty);
  LoadInst *NewLd1 = CreateLoadIns(Builder, &VecLd1[0], Ty);
  Value* Args[] = { NewLd0, NewLd1, Acc };
  Function *SMLAD = nullptr;
  if (Exchange)
    SMLAD = Acc->getType()->isIntegerTy(32) ?
      Intrinsic::getDeclaration(M, Intrinsic::arm_smladx) :
      Intrinsic::getDeclaration(M, Intrinsic::arm_smlaldx);
  else
    SMLAD = Acc->getType()->isIntegerTy(32) ?
      Intrinsic::getDeclaration(M, Intrinsic::arm_smlad) :
      Intrinsic::getDeclaration(M, Intrinsic::arm_smlald);
  CallInst *Call = Builder.CreateCall(SMLAD, Args);
  NumSMLAD++;
  return Call;
}

Pass *llvm::createARMParallelDSPPass() {
  return new ARMParallelDSP();
}

char ARMParallelDSP::ID = 0;

INITIALIZE_PASS_BEGIN(ARMParallelDSP, "arm-parallel-dsp",
                "Transform loops to use DSP intrinsics", false, false)
INITIALIZE_PASS_END(ARMParallelDSP, "arm-parallel-dsp",
                "Transform loops to use DSP intrinsics", false, false)
