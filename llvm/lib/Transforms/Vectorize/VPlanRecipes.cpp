//===- VPlanRecipes.cpp - Implementations for VPlan recipes ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains implementations for different VPlan recipes.
///
//===----------------------------------------------------------------------===//

#include "VPlan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include <cassert>

using namespace llvm;

extern cl::opt<bool> EnableVPlanNativePath;

#define LV_NAME "loop-vectorize"
#define DEBUG_TYPE LV_NAME

bool VPRecipeBase::mayWriteToMemory() const {
  switch (getVPDefID()) {
  case VPWidenMemoryInstructionSC: {
    return cast<VPWidenMemoryInstructionRecipe>(this)->isStore();
  }
  case VPReplicateSC:
  case VPWidenCallSC:
    return cast<Instruction>(getVPSingleValue()->getUnderlyingValue())
        ->mayWriteToMemory();
  case VPBranchOnMaskSC:
    return false;
  case VPWidenIntOrFpInductionSC:
  case VPWidenCanonicalIVSC:
  case VPWidenPHISC:
  case VPBlendSC:
  case VPWidenSC:
  case VPWidenGEPSC:
  case VPReductionSC:
  case VPWidenSelectSC: {
    const Instruction *I =
        dyn_cast_or_null<Instruction>(getVPSingleValue()->getUnderlyingValue());
    (void)I;
    assert((!I || !I->mayWriteToMemory()) &&
           "underlying instruction may write to memory");
    return false;
  }
  default:
    return true;
  }
}

bool VPRecipeBase::mayReadFromMemory() const {
  switch (getVPDefID()) {
  case VPWidenMemoryInstructionSC: {
    return !cast<VPWidenMemoryInstructionRecipe>(this)->isStore();
  }
  case VPReplicateSC:
  case VPWidenCallSC:
    return cast<Instruction>(getVPSingleValue()->getUnderlyingValue())
        ->mayReadFromMemory();
  case VPBranchOnMaskSC:
    return false;
  case VPWidenIntOrFpInductionSC:
  case VPWidenCanonicalIVSC:
  case VPWidenPHISC:
  case VPBlendSC:
  case VPWidenSC:
  case VPWidenGEPSC:
  case VPReductionSC:
  case VPWidenSelectSC: {
    const Instruction *I =
        dyn_cast_or_null<Instruction>(getVPSingleValue()->getUnderlyingValue());
    (void)I;
    assert((!I || !I->mayReadFromMemory()) &&
           "underlying instruction may read from memory");
    return false;
  }
  default:
    return true;
  }
}

bool VPRecipeBase::mayHaveSideEffects() const {
  switch (getVPDefID()) {
  case VPWidenIntOrFpInductionSC:
  case VPWidenPointerInductionSC:
  case VPWidenCanonicalIVSC:
  case VPWidenPHISC:
  case VPBlendSC:
  case VPWidenSC:
  case VPWidenGEPSC:
  case VPReductionSC:
  case VPWidenSelectSC:
  case VPScalarIVStepsSC: {
    const Instruction *I =
        dyn_cast_or_null<Instruction>(getVPSingleValue()->getUnderlyingValue());
    (void)I;
    assert((!I || !I->mayHaveSideEffects()) &&
           "underlying instruction has side-effects");
    return false;
  }
  case VPReplicateSC: {
    auto *R = cast<VPReplicateRecipe>(this);
    return R->getUnderlyingInstr()->mayHaveSideEffects();
  }
  default:
    return true;
  }
}

void VPLiveOut::fixPhi(VPlan &Plan, VPTransformState &State) {
  auto Lane = VPLane::getLastLaneForVF(State.VF);
  VPValue *ExitValue = getOperand(0);
  if (Plan.isUniformAfterVectorization(ExitValue))
    Lane = VPLane::getFirstLane();
  Phi->addIncoming(State.get(ExitValue, VPIteration(State.UF - 1, Lane)),
                   State.Builder.GetInsertBlock());
}

void VPRecipeBase::insertBefore(VPRecipeBase *InsertPos) {
  assert(!Parent && "Recipe already in some VPBasicBlock");
  assert(InsertPos->getParent() &&
         "Insertion position not in any VPBasicBlock");
  Parent = InsertPos->getParent();
  Parent->getRecipeList().insert(InsertPos->getIterator(), this);
}

void VPRecipeBase::insertBefore(VPBasicBlock &BB,
                                iplist<VPRecipeBase>::iterator I) {
  assert(!Parent && "Recipe already in some VPBasicBlock");
  assert(I == BB.end() || I->getParent() == &BB);
  Parent = &BB;
  BB.getRecipeList().insert(I, this);
}

void VPRecipeBase::insertAfter(VPRecipeBase *InsertPos) {
  assert(!Parent && "Recipe already in some VPBasicBlock");
  assert(InsertPos->getParent() &&
         "Insertion position not in any VPBasicBlock");
  Parent = InsertPos->getParent();
  Parent->getRecipeList().insertAfter(InsertPos->getIterator(), this);
}

void VPRecipeBase::removeFromParent() {
  assert(getParent() && "Recipe not in any VPBasicBlock");
  getParent()->getRecipeList().remove(getIterator());
  Parent = nullptr;
}

iplist<VPRecipeBase>::iterator VPRecipeBase::eraseFromParent() {
  assert(getParent() && "Recipe not in any VPBasicBlock");
  return getParent()->getRecipeList().erase(getIterator());
}

void VPRecipeBase::moveAfter(VPRecipeBase *InsertPos) {
  removeFromParent();
  insertAfter(InsertPos);
}

void VPRecipeBase::moveBefore(VPBasicBlock &BB,
                              iplist<VPRecipeBase>::iterator I) {
  removeFromParent();
  insertBefore(BB, I);
}

void VPInstruction::generateInstruction(VPTransformState &State,
                                        unsigned Part) {
  IRBuilderBase &Builder = State.Builder;
  Builder.SetCurrentDebugLocation(DL);

  if (Instruction::isBinaryOp(getOpcode())) {
    Value *A = State.get(getOperand(0), Part);
    Value *B = State.get(getOperand(1), Part);
    Value *V = Builder.CreateBinOp((Instruction::BinaryOps)getOpcode(), A, B);
    State.set(this, V, Part);
    return;
  }

  switch (getOpcode()) {
  case VPInstruction::Not: {
    Value *A = State.get(getOperand(0), Part);
    Value *V = Builder.CreateNot(A);
    State.set(this, V, Part);
    break;
  }
  case VPInstruction::ICmpULE: {
    Value *IV = State.get(getOperand(0), Part);
    Value *TC = State.get(getOperand(1), Part);
    Value *V = Builder.CreateICmpULE(IV, TC);
    State.set(this, V, Part);
    break;
  }
  case Instruction::Select: {
    Value *Cond = State.get(getOperand(0), Part);
    Value *Op1 = State.get(getOperand(1), Part);
    Value *Op2 = State.get(getOperand(2), Part);
    Value *V = Builder.CreateSelect(Cond, Op1, Op2);
    State.set(this, V, Part);
    break;
  }
  case VPInstruction::ActiveLaneMask: {
    // Get first lane of vector induction variable.
    Value *VIVElem0 = State.get(getOperand(0), VPIteration(Part, 0));
    // Get the original loop tripcount.
    Value *ScalarTC = State.get(getOperand(1), Part);

    auto *Int1Ty = Type::getInt1Ty(Builder.getContext());
    auto *PredTy = VectorType::get(Int1Ty, State.VF);
    Instruction *Call = Builder.CreateIntrinsic(
        Intrinsic::get_active_lane_mask, {PredTy, ScalarTC->getType()},
        {VIVElem0, ScalarTC}, nullptr, "active.lane.mask");
    State.set(this, Call, Part);
    break;
  }
  case VPInstruction::FirstOrderRecurrenceSplice: {
    // Generate code to combine the previous and current values in vector v3.
    //
    //   vector.ph:
    //     v_init = vector(..., ..., ..., a[-1])
    //     br vector.body
    //
    //   vector.body
    //     i = phi [0, vector.ph], [i+4, vector.body]
    //     v1 = phi [v_init, vector.ph], [v2, vector.body]
    //     v2 = a[i, i+1, i+2, i+3];
    //     v3 = vector(v1(3), v2(0, 1, 2))

    // For the first part, use the recurrence phi (v1), otherwise v2.
    auto *V1 = State.get(getOperand(0), 0);
    Value *PartMinus1 = Part == 0 ? V1 : State.get(getOperand(1), Part - 1);
    if (!PartMinus1->getType()->isVectorTy()) {
      State.set(this, PartMinus1, Part);
    } else {
      Value *V2 = State.get(getOperand(1), Part);
      State.set(this, Builder.CreateVectorSplice(PartMinus1, V2, -1), Part);
    }
    break;
  }
  case VPInstruction::CanonicalIVIncrement:
  case VPInstruction::CanonicalIVIncrementNUW: {
    Value *Next = nullptr;
    if (Part == 0) {
      bool IsNUW = getOpcode() == VPInstruction::CanonicalIVIncrementNUW;
      auto *Phi = State.get(getOperand(0), 0);
      // The loop step is equal to the vectorization factor (num of SIMD
      // elements) times the unroll factor (num of SIMD instructions).
      Value *Step =
          createStepForVF(Builder, Phi->getType(), State.VF, State.UF);
      Next = Builder.CreateAdd(Phi, Step, "index.next", IsNUW, false);
    } else {
      Next = State.get(this, 0);
    }

    State.set(this, Next, Part);
    break;
  }
  case VPInstruction::BranchOnCond: {
    if (Part != 0)
      break;

    Value *Cond = State.get(getOperand(0), VPIteration(Part, 0));
    VPRegionBlock *ParentRegion = getParent()->getParent();
    VPBasicBlock *Header = ParentRegion->getEntryBasicBlock();

    // Replace the temporary unreachable terminator with a new conditional
    // branch, hooking it up to backward destination for exiting blocks now and
    // to forward destination(s) later when they are created.
    BranchInst *CondBr =
        Builder.CreateCondBr(Cond, Builder.GetInsertBlock(), nullptr);

    if (getParent()->isExiting())
      CondBr->setSuccessor(1, State.CFG.VPBB2IRBB[Header]);

    CondBr->setSuccessor(0, nullptr);
    Builder.GetInsertBlock()->getTerminator()->eraseFromParent();
    break;
  }
  case VPInstruction::BranchOnCount: {
    if (Part != 0)
      break;
    // First create the compare.
    Value *IV = State.get(getOperand(0), Part);
    Value *TC = State.get(getOperand(1), Part);
    Value *Cond = Builder.CreateICmpEQ(IV, TC);

    // Now create the branch.
    auto *Plan = getParent()->getPlan();
    VPRegionBlock *TopRegion = Plan->getVectorLoopRegion();
    VPBasicBlock *Header = TopRegion->getEntry()->getEntryBasicBlock();

    // Replace the temporary unreachable terminator with a new conditional
    // branch, hooking it up to backward destination (the header) now and to the
    // forward destination (the exit/middle block) later when it is created.
    // Note that CreateCondBr expects a valid BB as first argument, so we need
    // to set it to nullptr later.
    BranchInst *CondBr = Builder.CreateCondBr(Cond, Builder.GetInsertBlock(),
                                              State.CFG.VPBB2IRBB[Header]);
    CondBr->setSuccessor(0, nullptr);
    Builder.GetInsertBlock()->getTerminator()->eraseFromParent();
    break;
  }
  default:
    llvm_unreachable("Unsupported opcode for instruction");
  }
}

void VPInstruction::execute(VPTransformState &State) {
  assert(!State.Instance && "VPInstruction executing an Instance");
  IRBuilderBase::FastMathFlagGuard FMFGuard(State.Builder);
  State.Builder.setFastMathFlags(FMF);
  for (unsigned Part = 0; Part < State.UF; ++Part)
    generateInstruction(State, Part);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPInstruction::dump() const {
  VPSlotTracker SlotTracker(getParent()->getPlan());
  print(dbgs(), "", SlotTracker);
}

void VPInstruction::print(raw_ostream &O, const Twine &Indent,
                          VPSlotTracker &SlotTracker) const {
  O << Indent << "EMIT ";

  if (hasResult()) {
    printAsOperand(O, SlotTracker);
    O << " = ";
  }

  switch (getOpcode()) {
  case VPInstruction::Not:
    O << "not";
    break;
  case VPInstruction::ICmpULE:
    O << "icmp ule";
    break;
  case VPInstruction::SLPLoad:
    O << "combined load";
    break;
  case VPInstruction::SLPStore:
    O << "combined store";
    break;
  case VPInstruction::ActiveLaneMask:
    O << "active lane mask";
    break;
  case VPInstruction::FirstOrderRecurrenceSplice:
    O << "first-order splice";
    break;
  case VPInstruction::CanonicalIVIncrement:
    O << "VF * UF + ";
    break;
  case VPInstruction::CanonicalIVIncrementNUW:
    O << "VF * UF +(nuw) ";
    break;
  case VPInstruction::BranchOnCond:
    O << "branch-on-cond";
    break;
  case VPInstruction::BranchOnCount:
    O << "branch-on-count ";
    break;
  default:
    O << Instruction::getOpcodeName(getOpcode());
  }

  O << FMF;

  for (const VPValue *Operand : operands()) {
    O << " ";
    Operand->printAsOperand(O, SlotTracker);
  }

  if (DL) {
    O << ", !dbg ";
    DL.print(O);
  }
}
#endif

void VPInstruction::setFastMathFlags(FastMathFlags FMFNew) {
  // Make sure the VPInstruction is a floating-point operation.
  assert((Opcode == Instruction::FAdd || Opcode == Instruction::FMul ||
          Opcode == Instruction::FNeg || Opcode == Instruction::FSub ||
          Opcode == Instruction::FDiv || Opcode == Instruction::FRem ||
          Opcode == Instruction::FCmp) &&
         "this op can't take fast-math flags");
  FMF = FMFNew;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPWidenCallRecipe::print(raw_ostream &O, const Twine &Indent,
                              VPSlotTracker &SlotTracker) const {
  O << Indent << "WIDEN-CALL ";

  auto *CI = cast<CallInst>(getUnderlyingInstr());
  if (CI->getType()->isVoidTy())
    O << "void ";
  else {
    printAsOperand(O, SlotTracker);
    O << " = ";
  }

  O << "call @" << CI->getCalledFunction()->getName() << "(";
  printOperands(O, SlotTracker);
  O << ")";
}

void VPWidenSelectRecipe::print(raw_ostream &O, const Twine &Indent,
                                VPSlotTracker &SlotTracker) const {
  O << Indent << "WIDEN-SELECT ";
  printAsOperand(O, SlotTracker);
  O << " = select ";
  getOperand(0)->printAsOperand(O, SlotTracker);
  O << ", ";
  getOperand(1)->printAsOperand(O, SlotTracker);
  O << ", ";
  getOperand(2)->printAsOperand(O, SlotTracker);
  O << (InvariantCond ? " (condition is loop invariant)" : "");
}
#endif

void VPWidenSelectRecipe::execute(VPTransformState &State) {
  auto &I = *cast<SelectInst>(getUnderlyingInstr());
  State.setDebugLocFromInst(&I);

  // The condition can be loop invariant but still defined inside the
  // loop. This means that we can't just use the original 'cond' value.
  // We have to take the 'vectorized' value and pick the first lane.
  // Instcombine will make this a no-op.
  auto *InvarCond =
      InvariantCond ? State.get(getOperand(0), VPIteration(0, 0)) : nullptr;

  for (unsigned Part = 0; Part < State.UF; ++Part) {
    Value *Cond = InvarCond ? InvarCond : State.get(getOperand(0), Part);
    Value *Op0 = State.get(getOperand(1), Part);
    Value *Op1 = State.get(getOperand(2), Part);
    Value *Sel = State.Builder.CreateSelect(Cond, Op0, Op1);
    State.set(this, Sel, Part);
    State.addMetadata(Sel, &I);
  }
}

void VPWidenRecipe::execute(VPTransformState &State) {
  auto &I = *cast<Instruction>(getUnderlyingValue());
  auto &Builder = State.Builder;
  switch (I.getOpcode()) {
  case Instruction::Call:
  case Instruction::Br:
  case Instruction::PHI:
  case Instruction::GetElementPtr:
  case Instruction::Select:
    llvm_unreachable("This instruction is handled by a different recipe.");
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::SRem:
  case Instruction::URem:
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::FNeg:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::FDiv:
  case Instruction::FRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor: {
    // Just widen unops and binops.
    State.setDebugLocFromInst(&I);

    for (unsigned Part = 0; Part < State.UF; ++Part) {
      SmallVector<Value *, 2> Ops;
      for (VPValue *VPOp : operands())
        Ops.push_back(State.get(VPOp, Part));

      Value *V = Builder.CreateNAryOp(I.getOpcode(), Ops);

      if (auto *VecOp = dyn_cast<Instruction>(V)) {
        VecOp->copyIRFlags(&I);

        // If the instruction is vectorized and was in a basic block that needed
        // predication, we can't propagate poison-generating flags (nuw/nsw,
        // exact, etc.). The control flow has been linearized and the
        // instruction is no longer guarded by the predicate, which could make
        // the flag properties to no longer hold.
        if (State.MayGeneratePoisonRecipes.contains(this))
          VecOp->dropPoisonGeneratingFlags();
      }

      // Use this vector value for all users of the original instruction.
      State.set(this, V, Part);
      State.addMetadata(V, &I);
    }

    break;
  }
  case Instruction::Freeze: {
    State.setDebugLocFromInst(&I);

    for (unsigned Part = 0; Part < State.UF; ++Part) {
      Value *Op = State.get(getOperand(0), Part);

      Value *Freeze = Builder.CreateFreeze(Op);
      State.set(this, Freeze, Part);
    }
    break;
  }
  case Instruction::ICmp:
  case Instruction::FCmp: {
    // Widen compares. Generate vector compares.
    bool FCmp = (I.getOpcode() == Instruction::FCmp);
    auto *Cmp = cast<CmpInst>(&I);
    State.setDebugLocFromInst(Cmp);
    for (unsigned Part = 0; Part < State.UF; ++Part) {
      Value *A = State.get(getOperand(0), Part);
      Value *B = State.get(getOperand(1), Part);
      Value *C = nullptr;
      if (FCmp) {
        // Propagate fast math flags.
        IRBuilder<>::FastMathFlagGuard FMFG(Builder);
        Builder.setFastMathFlags(Cmp->getFastMathFlags());
        C = Builder.CreateFCmp(Cmp->getPredicate(), A, B);
      } else {
        C = Builder.CreateICmp(Cmp->getPredicate(), A, B);
      }
      State.set(this, C, Part);
      State.addMetadata(C, &I);
    }

    break;
  }

  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::FPExt:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::SIToFP:
  case Instruction::UIToFP:
  case Instruction::Trunc:
  case Instruction::FPTrunc:
  case Instruction::BitCast: {
    auto *CI = cast<CastInst>(&I);
    State.setDebugLocFromInst(CI);

    /// Vectorize casts.
    Type *DestTy = (State.VF.isScalar())
                       ? CI->getType()
                       : VectorType::get(CI->getType(), State.VF);

    for (unsigned Part = 0; Part < State.UF; ++Part) {
      Value *A = State.get(getOperand(0), Part);
      Value *Cast = Builder.CreateCast(CI->getOpcode(), A, DestTy);
      State.set(this, Cast, Part);
      State.addMetadata(Cast, &I);
    }
    break;
  }
  default:
    // This instruction is not vectorized by simple widening.
    LLVM_DEBUG(dbgs() << "LV: Found an unhandled instruction: " << I);
    llvm_unreachable("Unhandled instruction!");
  } // end of switch.
}
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPWidenRecipe::print(raw_ostream &O, const Twine &Indent,
                          VPSlotTracker &SlotTracker) const {
  O << Indent << "WIDEN ";
  printAsOperand(O, SlotTracker);
  O << " = " << getUnderlyingInstr()->getOpcodeName() << " ";
  printOperands(O, SlotTracker);
}

void VPWidenIntOrFpInductionRecipe::print(raw_ostream &O, const Twine &Indent,
                                          VPSlotTracker &SlotTracker) const {
  O << Indent << "WIDEN-INDUCTION";
  if (getTruncInst()) {
    O << "\\l\"";
    O << " +\n" << Indent << "\"  " << VPlanIngredient(IV) << "\\l\"";
    O << " +\n" << Indent << "\"  ";
    getVPValue(0)->printAsOperand(O, SlotTracker);
  } else
    O << " " << VPlanIngredient(IV);

  O << ", ";
  getStepValue()->printAsOperand(O, SlotTracker);
}
#endif

bool VPWidenIntOrFpInductionRecipe::isCanonical() const {
  auto *StartC = dyn_cast<ConstantInt>(getStartValue()->getLiveInIRValue());
  auto *StepC = dyn_cast<SCEVConstant>(getInductionDescriptor().getStep());
  return StartC && StartC->isZero() && StepC && StepC->isOne();
}

VPCanonicalIVPHIRecipe *VPScalarIVStepsRecipe::getCanonicalIV() const {
  return cast<VPCanonicalIVPHIRecipe>(getOperand(0));
}

bool VPScalarIVStepsRecipe::isCanonical() const {
  auto *CanIV = getCanonicalIV();
  // The start value of the steps-recipe must match the start value of the
  // canonical induction and it must step by 1.
  if (CanIV->getStartValue() != getStartValue())
    return false;
  auto *StepVPV = getStepValue();
  if (StepVPV->getDef())
    return false;
  auto *StepC = dyn_cast_or_null<ConstantInt>(StepVPV->getLiveInIRValue());
  return StepC && StepC->isOne();
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPScalarIVStepsRecipe::print(raw_ostream &O, const Twine &Indent,
                                  VPSlotTracker &SlotTracker) const {
  O << Indent;
  printAsOperand(O, SlotTracker);
  O << Indent << "= SCALAR-STEPS ";
  printOperands(O, SlotTracker);
}

void VPWidenGEPRecipe::print(raw_ostream &O, const Twine &Indent,
                             VPSlotTracker &SlotTracker) const {
  O << Indent << "WIDEN-GEP ";
  O << (IsPtrLoopInvariant ? "Inv" : "Var");
  size_t IndicesNumber = IsIndexLoopInvariant.size();
  for (size_t I = 0; I < IndicesNumber; ++I)
    O << "[" << (IsIndexLoopInvariant[I] ? "Inv" : "Var") << "]";

  O << " ";
  printAsOperand(O, SlotTracker);
  O << " = getelementptr ";
  printOperands(O, SlotTracker);
}

void VPBlendRecipe::print(raw_ostream &O, const Twine &Indent,
                          VPSlotTracker &SlotTracker) const {
  O << Indent << "BLEND ";
  Phi->printAsOperand(O, false);
  O << " =";
  if (getNumIncomingValues() == 1) {
    // Not a User of any mask: not really blending, this is a
    // single-predecessor phi.
    O << " ";
    getIncomingValue(0)->printAsOperand(O, SlotTracker);
  } else {
    for (unsigned I = 0, E = getNumIncomingValues(); I < E; ++I) {
      O << " ";
      getIncomingValue(I)->printAsOperand(O, SlotTracker);
      O << "/";
      getMask(I)->printAsOperand(O, SlotTracker);
    }
  }
}

void VPReductionRecipe::print(raw_ostream &O, const Twine &Indent,
                              VPSlotTracker &SlotTracker) const {
  O << Indent << "REDUCE ";
  printAsOperand(O, SlotTracker);
  O << " = ";
  getChainOp()->printAsOperand(O, SlotTracker);
  O << " +";
  if (isa<FPMathOperator>(getUnderlyingInstr()))
    O << getUnderlyingInstr()->getFastMathFlags();
  O << " reduce." << Instruction::getOpcodeName(RdxDesc->getOpcode()) << " (";
  getVecOp()->printAsOperand(O, SlotTracker);
  if (getCondOp()) {
    O << ", ";
    getCondOp()->printAsOperand(O, SlotTracker);
  }
  O << ")";
  if (RdxDesc->IntermediateStore)
    O << " (with final reduction value stored in invariant address sank "
         "outside of loop)";
}

void VPReplicateRecipe::print(raw_ostream &O, const Twine &Indent,
                              VPSlotTracker &SlotTracker) const {
  O << Indent << (IsUniform ? "CLONE " : "REPLICATE ");

  if (!getUnderlyingInstr()->getType()->isVoidTy()) {
    printAsOperand(O, SlotTracker);
    O << " = ";
  }
  if (auto *CB = dyn_cast<CallBase>(getUnderlyingInstr())) {
    O << "call @" << CB->getCalledFunction()->getName() << "(";
    interleaveComma(make_range(op_begin(), op_begin() + (getNumOperands() - 1)),
                    O, [&O, &SlotTracker](VPValue *Op) {
                      Op->printAsOperand(O, SlotTracker);
                    });
    O << ")";
  } else {
    O << Instruction::getOpcodeName(getUnderlyingInstr()->getOpcode()) << " ";
    printOperands(O, SlotTracker);
  }

  if (AlsoPack)
    O << " (S->V)";
}

void VPPredInstPHIRecipe::print(raw_ostream &O, const Twine &Indent,
                                VPSlotTracker &SlotTracker) const {
  O << Indent << "PHI-PREDICATED-INSTRUCTION ";
  printAsOperand(O, SlotTracker);
  O << " = ";
  printOperands(O, SlotTracker);
}

void VPWidenMemoryInstructionRecipe::print(raw_ostream &O, const Twine &Indent,
                                           VPSlotTracker &SlotTracker) const {
  O << Indent << "WIDEN ";

  if (!isStore()) {
    getVPSingleValue()->printAsOperand(O, SlotTracker);
    O << " = ";
  }
  O << Instruction::getOpcodeName(Ingredient.getOpcode()) << " ";

  printOperands(O, SlotTracker);
}
#endif

void VPCanonicalIVPHIRecipe::execute(VPTransformState &State) {
  Value *Start = getStartValue()->getLiveInIRValue();
  PHINode *EntryPart = PHINode::Create(
      Start->getType(), 2, "index", &*State.CFG.PrevBB->getFirstInsertionPt());

  BasicBlock *VectorPH = State.CFG.getPreheaderBBFor(this);
  EntryPart->addIncoming(Start, VectorPH);
  EntryPart->setDebugLoc(DL);
  for (unsigned Part = 0, UF = State.UF; Part < UF; ++Part)
    State.set(this, EntryPart, Part);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPCanonicalIVPHIRecipe::print(raw_ostream &O, const Twine &Indent,
                                   VPSlotTracker &SlotTracker) const {
  O << Indent << "EMIT ";
  printAsOperand(O, SlotTracker);
  O << " = CANONICAL-INDUCTION";
}
#endif

bool VPWidenPointerInductionRecipe::onlyScalarsGenerated(ElementCount VF) {
  bool IsUniform = vputils::onlyFirstLaneUsed(this);
  return all_of(users(),
                [&](const VPUser *U) { return U->usesScalars(this); }) &&
         (IsUniform || !VF.isScalable());
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPWidenPointerInductionRecipe::print(raw_ostream &O, const Twine &Indent,
                                          VPSlotTracker &SlotTracker) const {
  O << Indent << "EMIT ";
  printAsOperand(O, SlotTracker);
  O << " = WIDEN-POINTER-INDUCTION ";
  getStartValue()->printAsOperand(O, SlotTracker);
  O << ", " << *IndDesc.getStep();
}
#endif

void VPExpandSCEVRecipe::execute(VPTransformState &State) {
  assert(!State.Instance && "cannot be used in per-lane");
  const DataLayout &DL = State.CFG.PrevBB->getModule()->getDataLayout();
  SCEVExpander Exp(SE, DL, "induction");

  Value *Res = Exp.expandCodeFor(Expr, Expr->getType(),
                                 &*State.Builder.GetInsertPoint());

  for (unsigned Part = 0, UF = State.UF; Part < UF; ++Part)
    State.set(this, Res, Part);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPExpandSCEVRecipe::print(raw_ostream &O, const Twine &Indent,
                               VPSlotTracker &SlotTracker) const {
  O << Indent << "EMIT ";
  getVPSingleValue()->printAsOperand(O, SlotTracker);
  O << " = EXPAND SCEV " << *Expr;
}
#endif

void VPWidenCanonicalIVRecipe::execute(VPTransformState &State) {
  Value *CanonicalIV = State.get(getOperand(0), 0);
  Type *STy = CanonicalIV->getType();
  IRBuilder<> Builder(State.CFG.PrevBB->getTerminator());
  ElementCount VF = State.VF;
  Value *VStart = VF.isScalar()
                      ? CanonicalIV
                      : Builder.CreateVectorSplat(VF, CanonicalIV, "broadcast");
  for (unsigned Part = 0, UF = State.UF; Part < UF; ++Part) {
    Value *VStep = createStepForVF(Builder, STy, VF, Part);
    if (VF.isVector()) {
      VStep = Builder.CreateVectorSplat(VF, VStep);
      VStep =
          Builder.CreateAdd(VStep, Builder.CreateStepVector(VStep->getType()));
    }
    Value *CanonicalVectorIV = Builder.CreateAdd(VStart, VStep, "vec.iv");
    State.set(this, CanonicalVectorIV, Part);
  }
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPWidenCanonicalIVRecipe::print(raw_ostream &O, const Twine &Indent,
                                     VPSlotTracker &SlotTracker) const {
  O << Indent << "EMIT ";
  printAsOperand(O, SlotTracker);
  O << " = WIDEN-CANONICAL-INDUCTION ";
  printOperands(O, SlotTracker);
}
#endif

void VPFirstOrderRecurrencePHIRecipe::execute(VPTransformState &State) {
  auto &Builder = State.Builder;
  // Create a vector from the initial value.
  auto *VectorInit = getStartValue()->getLiveInIRValue();

  Type *VecTy = State.VF.isScalar()
                    ? VectorInit->getType()
                    : VectorType::get(VectorInit->getType(), State.VF);

  BasicBlock *VectorPH = State.CFG.getPreheaderBBFor(this);
  if (State.VF.isVector()) {
    auto *IdxTy = Builder.getInt32Ty();
    auto *One = ConstantInt::get(IdxTy, 1);
    IRBuilder<>::InsertPointGuard Guard(Builder);
    Builder.SetInsertPoint(VectorPH->getTerminator());
    auto *RuntimeVF = getRuntimeVF(Builder, IdxTy, State.VF);
    auto *LastIdx = Builder.CreateSub(RuntimeVF, One);
    VectorInit = Builder.CreateInsertElement(
        PoisonValue::get(VecTy), VectorInit, LastIdx, "vector.recur.init");
  }

  // Create a phi node for the new recurrence.
  PHINode *EntryPart = PHINode::Create(
      VecTy, 2, "vector.recur", &*State.CFG.PrevBB->getFirstInsertionPt());
  EntryPart->addIncoming(VectorInit, VectorPH);
  State.set(this, EntryPart, 0);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPFirstOrderRecurrencePHIRecipe::print(raw_ostream &O, const Twine &Indent,
                                            VPSlotTracker &SlotTracker) const {
  O << Indent << "FIRST-ORDER-RECURRENCE-PHI ";
  printAsOperand(O, SlotTracker);
  O << " = phi ";
  printOperands(O, SlotTracker);
}
#endif

void VPReductionPHIRecipe::execute(VPTransformState &State) {
  PHINode *PN = cast<PHINode>(getUnderlyingValue());
  auto &Builder = State.Builder;

  // In order to support recurrences we need to be able to vectorize Phi nodes.
  // Phi nodes have cycles, so we need to vectorize them in two stages. This is
  // stage #1: We create a new vector PHI node with no incoming edges. We'll use
  // this value when we vectorize all of the instructions that use the PHI.
  bool ScalarPHI = State.VF.isScalar() || IsInLoop;
  Type *VecTy =
      ScalarPHI ? PN->getType() : VectorType::get(PN->getType(), State.VF);

  BasicBlock *HeaderBB = State.CFG.PrevBB;
  assert(State.CurrentVectorLoop->getHeader() == HeaderBB &&
         "recipe must be in the vector loop header");
  unsigned LastPartForNewPhi = isOrdered() ? 1 : State.UF;
  for (unsigned Part = 0; Part < LastPartForNewPhi; ++Part) {
    Value *EntryPart =
        PHINode::Create(VecTy, 2, "vec.phi", &*HeaderBB->getFirstInsertionPt());
    State.set(this, EntryPart, Part);
  }

  BasicBlock *VectorPH = State.CFG.getPreheaderBBFor(this);

  // Reductions do not have to start at zero. They can start with
  // any loop invariant values.
  VPValue *StartVPV = getStartValue();
  Value *StartV = StartVPV->getLiveInIRValue();

  Value *Iden = nullptr;
  RecurKind RK = RdxDesc.getRecurrenceKind();
  if (RecurrenceDescriptor::isMinMaxRecurrenceKind(RK) ||
      RecurrenceDescriptor::isSelectCmpRecurrenceKind(RK)) {
    // MinMax reduction have the start value as their identify.
    if (ScalarPHI) {
      Iden = StartV;
    } else {
      IRBuilderBase::InsertPointGuard IPBuilder(Builder);
      Builder.SetInsertPoint(VectorPH->getTerminator());
      StartV = Iden =
          Builder.CreateVectorSplat(State.VF, StartV, "minmax.ident");
    }
  } else {
    Iden = RdxDesc.getRecurrenceIdentity(RK, VecTy->getScalarType(),
                                         RdxDesc.getFastMathFlags());

    if (!ScalarPHI) {
      Iden = Builder.CreateVectorSplat(State.VF, Iden);
      IRBuilderBase::InsertPointGuard IPBuilder(Builder);
      Builder.SetInsertPoint(VectorPH->getTerminator());
      Constant *Zero = Builder.getInt32(0);
      StartV = Builder.CreateInsertElement(Iden, StartV, Zero);
    }
  }

  for (unsigned Part = 0; Part < LastPartForNewPhi; ++Part) {
    Value *EntryPart = State.get(this, Part);
    // Make sure to add the reduction start value only to the
    // first unroll part.
    Value *StartVal = (Part == 0) ? StartV : Iden;
    cast<PHINode>(EntryPart)->addIncoming(StartVal, VectorPH);
  }
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPReductionPHIRecipe::print(raw_ostream &O, const Twine &Indent,
                                 VPSlotTracker &SlotTracker) const {
  O << Indent << "WIDEN-REDUCTION-PHI ";

  printAsOperand(O, SlotTracker);
  O << " = phi ";
  printOperands(O, SlotTracker);
}
#endif

void VPWidenPHIRecipe::execute(VPTransformState &State) {
  assert(EnableVPlanNativePath &&
         "Non-native vplans are not expected to have VPWidenPHIRecipes.");

  // Currently we enter here in the VPlan-native path for non-induction
  // PHIs where all control flow is uniform. We simply widen these PHIs.
  // Create a vector phi with no operands - the vector phi operands will be
  // set at the end of vector code generation.
  VPBasicBlock *Parent = getParent();
  VPRegionBlock *LoopRegion = Parent->getEnclosingLoopRegion();
  unsigned StartIdx = 0;
  // For phis in header blocks of loop regions, use the index of the value
  // coming from the preheader.
  if (LoopRegion->getEntryBasicBlock() == Parent) {
    for (unsigned I = 0; I < getNumOperands(); ++I) {
      if (getIncomingBlock(I) ==
          LoopRegion->getSinglePredecessor()->getExitingBasicBlock())
        StartIdx = I;
    }
  }
  Value *Op0 = State.get(getOperand(StartIdx), 0);
  Type *VecTy = Op0->getType();
  Value *VecPhi = State.Builder.CreatePHI(VecTy, 2, "vec.phi");
  State.set(this, VecPhi, 0);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void VPWidenPHIRecipe::print(raw_ostream &O, const Twine &Indent,
                             VPSlotTracker &SlotTracker) const {
  O << Indent << "WIDEN-PHI ";

  auto *OriginalPhi = cast<PHINode>(getUnderlyingValue());
  // Unless all incoming values are modeled in VPlan  print the original PHI
  // directly.
  // TODO: Remove once all VPWidenPHIRecipe instances keep all relevant incoming
  // values as VPValues.
  if (getNumOperands() != OriginalPhi->getNumOperands()) {
    O << VPlanIngredient(OriginalPhi);
    return;
  }

  printAsOperand(O, SlotTracker);
  O << " = phi ";
  printOperands(O, SlotTracker);
}
#endif
