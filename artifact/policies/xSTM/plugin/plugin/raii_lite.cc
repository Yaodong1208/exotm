#include <stack>

#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "../../common/tm_defines.h"

#include "tm_plugin.h"

using namespace llvm;
using namespace std;

namespace {

/// In the RAII API, transactions are delineated by a special constructor (ctor)
/// and destructor (dtor).  We will ultimately need to instrument the sub-graph
/// of the CFG that starts with the instruction immediately succeeding the ctor,
/// and ends with the instruction immediately preceding the dtor.
///
/// The complicating factors are that (1) the ctor and dtor could be at *any*
/// point within the CFG, and (2) transactions can be nested.
///
/// To keep things simple, we split BBs that have ctors and dtors.  The goal is
/// that each ctor is in its own BB, and each dtor is the first instruction in
/// its BB.  Organizing the BBs like this gives us a nice invariant: *every*
/// instruction in the graph of blocks between the ctor and dtor is supposed to
/// be instrumented.
///
/// @returns A vector of all the ctors in the module.  Note that while we return
///          a vector of raii_region_t structs, those structs are incomplete:
///          they only have the begin instructions.
set<Function *> normalize_raii_boundaries(Module &M) {

  // TODO: clean it up a ton
  set<Function *> interesting_functions;

  // Process one function at a time.
  for (auto &F : M) {
    set<Value *> jbset;
    // We start by looking for calls to TM_RAII_BEGIN_STR.  For each, we extract
    // its argument.  Those are the setjmp buffers.  Since RAII_BEGIN and
    // RAII_END are `noexcept`, the lifetimes of the setjmp buffers will bind
    // tightly to the boundaries of transactions.
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (nullptr == CB)
          continue; // We're looking for calls
        auto FN = CB->getCalledFunction();
        if (nullptr == FN)
          continue; // Skip indirect calls
        if (FN->getName().equals(TM_RAII_BEGIN_STR)) {
          jbset.insert(CB->getOperand(0));
        }
      }
    }

    if (jbset.empty())
      continue;

    interesting_functions.insert(&F);

    // Now iterate the instructions again.  This time, we record our splitbefore
    // set as lifetime begin and TM_RAII_END_STR, and our splitafter set as
    // TM_RAII_BEGIN_STR and lifetime and
    set<Instruction *> splitbefore, splitafter;
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (nullptr == CB)
          continue; // We're looking for calls
        auto FN = CB->getCalledFunction();
        if (nullptr == FN)
          continue; // Skip indirect calls
        if (FN->getName().equals(TM_RAII_BEGIN_STR))
          splitafter.insert(CB);
        else if (FN->getName().equals(TM_RAII_END_STR))
          splitbefore.insert(CB);
        else if ((FN->getName().equals("llvm.lifetime.start.p0")) &&
                 (jbset.find(CB->getOperand(1)) != jbset.end()))
          splitbefore.insert(CB);
        else if ((FN->getName().equals("llvm.lifetime.end.p0")) &&
                 (jbset.find(CB->getOperand(1)) != jbset.end()))
          splitafter.insert(CB);
      }
    }

    // Now we can split everything easily
    for (auto I : splitbefore) {
      auto parent = I->getParent();
      bool first = false;
      for (auto &IN : *parent) {
        if (&IN == I)
          first = true;
        break;
      }
      if (!first)
        parent->splitBasicBlock(I);
    }
    for (auto I : splitafter) {
      auto parent = I->getParent();
      bool found = false;
      Instruction *succ = nullptr;
      for (auto &IN : *parent) {
        if (found) {
          succ = &IN;
          break;
        }
        if (&IN == I)
          found = true;
      }
      if (succ != nullptr)
        parent->splitBasicBlock(succ);
    }
  }
  return interesting_functions;

  // putting noexcept into the begin/end certainly does simplify the cfg.
  // Perhaps too much?
  //
  // Maybe the key here is that our transactions are lexically scoped, and
  // throwing across transaction boundaries is not allowed.  That means we don't
  // really need to be so worried: if the begin/end count matches, we're not in
  // a tx.  We can augment each BB with two ints: pre_count and post_count.
  //
  // Lexical scoping also means that a self edge in the CFG can't change the
  // count.  It also means that a back edge in the CFG can't change the count.
  //
  // Splitting any BB that has a begin or end
}

/// Recursive step of a lightweight DFS to find all BBs that are within a
/// lexically-scoped transaction
///
/// NB: Due to invokes and landing pads, we can't quite get away with using the
/// LLVM Dominators.
///
/// @param current  The BB we're analyzing
/// @param depth    The transaction depth
/// @param xSet     Set of BBs that are in transactions
/// @param visited  Set of BBs that have been visited
/// @param starters Set of BBs that start a transaction
/// @param enders   Set of BBs that end a transaction
void trace(BasicBlock *current, int depth, set<BasicBlock *> *xSet,
           set<BasicBlock *> &visited, set<BasicBlock *> &starters,
           set<BasicBlock *> &enders) {
  if (starters.find(current) != starters.end())
    ++depth; // This block starts a transaction
  else if (enders.find(current) != enders.end())
    --depth; // This block ends a transaction
  else if (depth > 0)
    xSet->insert(current); // This block is in a transaction
  visited.insert(current); // Don't process it again

  // Recurse to analyze successors
  for (auto *SUCC : successors(current))
    if (visited.find(SUCC) == visited.end())
      trace(SUCC, depth, xSet, visited, starters, enders);
}

set<BasicBlock *> *find_tx_blocks(Function &F) {
  // Find all the blocks that begin or end a transaction
  set<BasicBlock *> starters, enders;
  BasicBlock *firstBB = nullptr;
  for (auto &BB : F) {
    if (firstBB == nullptr)
      firstBB = &BB; // Cache the first BB, we need it later
    for (auto &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (nullptr == CB)
        continue; // We're looking for calls
      auto FN = CB->getCalledFunction();
      if (nullptr == FN)
        continue; // Skip indirect calls
      if (FN->getName().equals(TM_RAII_BEGIN_STR)) {
        starters.insert(&BB);
        break;
      }
      if (FN->getName().equals(TM_RAII_END_STR)) {
        enders.insert(&BB);
        break;
      }
    }
  }
  // Populate xSets with all BBs in F that have a nesting depth > 0
  set<BasicBlock *> *xSets = new set<BasicBlock *>();
  set<BasicBlock *> visits; // TODO: I don't like needing visits :(
  trace(firstBB, 0, xSets, visits, starters, enders);

  return xSets;
}

} // namespace

/// Search through the module and find every RAII region.  An RAII region is
/// defined by a matched pair of ctor and dtor calls.  Note that there is
/// transaction nesting, so we can't just start at a ctor and stop at the first
/// dtor we find.
///
/// In addition to *finding* these pairs, we do some slight manipulation, so
/// that each ctor or dtor call is in its own basic block.  The end state of the
/// call is that the plugin's raii_regions collection is properly populated.
void tm_plugin::discover_raii_lite(Module &M) {
  // Find the RAII start points, and make sure each ctor/dtor is in its own BB
  //
  // NB: entries in raii_regions will only have the ctor field set, not dtor or
  //     instruction_blocks
  auto interesting_functions = normalize_raii_boundaries(M);
  for (auto F : interesting_functions) {
    auto blocks = find_tx_blocks(*F);
    raii_lite_state.insert({F, blocks});
  }
}

/// For each sub-CFG of the program that is bounded by an RAII ctor and dtor,
/// search through all of its basic blocks and find any function calls/invokes.
/// For each, add the function to func_worklist, but only if we have the
/// definition for the function.
void tm_plugin::discover_reachable_raii_lite(Module &M) {
  for (auto r : raii_lite_state) {
    for (auto bb : *r.second) {
      for (auto &inst : *bb) {
        // TODO: cast to callbase instead of if?
        if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
          CallBase *CS = cast<CallBase>(&inst);
          if (Function *Callee = CS->getCalledFunction())
            if (!(Callee->isDeclaration()))
              func_worklist.push(Callee);
        }
      }
    }
  }
}

// Go through each RAII region, and for each instruction within the region,
// instrument it.  Our current strategy for the RAII API is to replace each
// instruction with a "diamond": a branch that looks at whether the TM system
// has dynamically decided that instrumentation is needed or not, a path that
// does the instrumentation, and a path that doesn't.  We expect LLVM to be
// smart enough to combine these branches, since they are based on a value that
// is immutable for the duration of the transaction.
void tm_plugin::instrument_regions_raii_lite(llvm::Module &M) {
  SmallVector<Instruction *, 8> skips; // unused, but API is ugly
  for (auto r : raii_lite_state)
    for (auto bb : *r.second)
      instrument_bb(bb, skips);
  return;
}