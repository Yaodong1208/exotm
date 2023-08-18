#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include "../../common/tm_defines.h"

#include "local_config.h"
#include "tm_plugin.h"

using namespace llvm;

/// Iterate through all of the annotations in the Module, and whenever we find
/// an annotation that ought to be associated with a Function, we get the
/// corresponding Function object from the Module and attach the annotation
/// directly to it.
void tm_plugin::attach_annotations_to_functions(Module &M) {
  // Get the global annotations.  If there aren't any, we're done.
  auto annotations = M.getGlobalVariable("llvm.global.annotations");
  if (!annotations)
    return;

  // Iterate through the ConstArray of annotations, for each that is associated
  // with a function, attach it to the function *regardless of what the
  // annotation is*
  auto *CA = dyn_cast<ConstantArray>(annotations->getOperand(0));
  for (auto &OI : CA->operands()) {
    auto *e = dyn_cast<ConstantStruct>(OI.get());
    if (auto *f = dyn_cast<Function>(e->getOperand(0))) {
      auto anno = cast<ConstantDataArray>(e->getOperand(1)->getOperand(0));
      f->addFnAttr(anno->getAsCString());
    }
  }
}

/// Populate the pure list with things we know are pure
void tm_plugin::populate_purelist(Module &M) {
  // We don't want to instrument TM API calls, or things we know need to be left
  // alone.
  //
  // TODO: __clang_call_terminate and setjmp are not really appropriate, but we
  //       have to have them for now.
  for (auto name :
       {TM_TRANSLATE_CALL_STR, TM_EXECUTE_STR, TM_EXECUTE_C_STR,
        TM_EXECUTE_C_INTERNAL_STR, TM_SETJUMP_NAME, TM_RAII_CTOR, TM_RAII_DTOR,
        TM_CLANG_STDTERMINATE, TM_RAII_BEGIN_STR, TM_RAII_END_STR}) {
    if (Function *f = M.getFunction(name))
      purelist.push_back(f);
  }

  // populate the pure list with any additional custom overrides
  for (const char *name : discovery_pure_overrides) {
    if (Function *f = M.getFunction(name))
      purelist.push_back(dyn_cast<Function>(f));
  }
}

/// Find the functions in the module that have TM_ANNOTATE, TM_PURE, or
/// TM_RENAME annotations, and put them in the work list
void tm_plugin::discover_annotated_funcs(Module &M) {
  // Go through the functions, searching for annotated definitions.  Note that
  // annotations can only be attached to definitions, not declarations.
  for (auto fn = M.getFunctionList().begin(), e = M.getFunctionList().end();
       fn != e; ++fn) {
    if (!(fn->isDeclaration())) {
      if (fn->hasFnAttribute(TM_ANNOTATE_STR)) {
        func_worklist.push(dyn_cast<Function>(fn));
      } else if (fn->hasFnAttribute(TM_PURE_STR)) {
        purelist.push_back(dyn_cast<Function>(fn));
      }

      // Find functions with the TM_RENAME attribute, change their names, and
      // put them in the work list.  We assume that these functions have
      // TM_RENAME as attribute #1
      //
      // TODO: revisit this part later.  Why can't we use hasFnAttribute?  Why
      //       isn't this an ELSE?  Is this really working correctly?
      auto FnAttr = fn->getAttributes().getFnAttrs();
      auto AttrSTR = FnAttr.getAsString(1);
      auto begin_pos = AttrSTR.find(TM_RENAME_STR);
      if (begin_pos != StringRef::npos) {
        auto end_pos = AttrSTR.find("\"", begin_pos);
        if (end_pos != StringRef::npos && end_pos != begin_pos) {
          fn->setName(Twine(
              TM_PREFIX_STR,
              AttrSTR.substr(begin_pos + strlen(TM_RENAME_STR),
                             end_pos - (begin_pos + strlen(TM_RENAME_STR)))));
          func_worklist.push(dyn_cast<Function>(fn));
          // Find the original version of functions that are being renamed, and
          // put them into the rename list
          Function *func =
              M.getFunction(fn->getName().substr(strlen(TM_PREFIX_STR)));
          renamelist.insert({dyn_cast<Function>(fn), func});
        }
      }
    }
  }
}

/// Find the functions in the module that are actually lambda bodies that
/// conform to our API, and add them to the work list
void tm_plugin::discover_lambda_funcs(Module &M) {
  // Strategy: Find all of the call instructions in the program
  // - If the call is to TM_EXECUTE_STR, then the second argument is the lambda
  //   object
  // - Follow def-use chains from the definition of the second argument to GEPs
  //   to stores of functions.  These are the (pure) manager functions and the
  //   (inst-needed) lambda bodies.
  for (auto &F : M) {
    if (F.isDeclaration())
      continue; // Skip declarations
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (nullptr == CB)
          continue; // Skip if not a call
        auto FF = CB->getCalledFunction();
        if (nullptr == FF)
          continue; // Skip indirect function calls
        if (!FF->getName().equals(TM_EXECUTE_STR))
          continue; // Skip if it's not a call to the lambda executor

        // Find all GEPs that use the Lambda
        // TODO: when we get rid of TM_OPAQUE, 1 will become 0
        auto LO = CB->getOperand(1); // The Lambda Object
        for (auto U : LO->users()) {
          auto GEP = dyn_cast<GetElementPtrInst>(U);
          if (GEP == nullptr)
            continue; // not a GEP

          // Find all uses of the GEP that store a function into the object
          for (auto UUU : GEP->users()) {
            auto US = dyn_cast<StoreInst>(UUU);
            if (US == nullptr)
              continue; // Not a store
            auto USF = dyn_cast<Function>(US->getValueOperand());
            if (nullptr == USF)
              continue; // Operand not a function
            auto name = USF->getName();
            // TODO: don't hard-code strings!
            if (name.endswith("18_Manager_operation")) {
              purelist.push_back(USF);
            } else {
              // We're going to need a clone of this lambda body
              func_worklist.push(USF);
              // We're also going to need to stick a branch in the original
              // body, so that we can dispatch to instrumented code if the TM
              // uses instrumentation.
              lambdas.push_back(USF);
            }
          }
        }
      }
    }
  }
}

/// Find the functions in the module that are called via TM_EXECUTE_C, and put
/// them in the work list
void tm_plugin::discover_capi_funcs(Module &M) {
  // Find any call to TM_EXECUTE_C in any function's body
  for (auto fn = M.getFunctionList().begin(), e = M.getFunctionList().end();
       fn != e; ++fn) {
    // TODO: On one hand, we should use this style (inst_begin) in
    //       discover_lambda_funcs.  On the other, use the callbase cast early
    //       to avoid so much nesting?
    for (inst_iterator I = inst_begin(*fn), E = inst_end(*fn); I != E; ++I) {
      if (isa<CallInst>(*I) || isa<InvokeInst>(*I)) {
        CallBase *CS = cast<CallBase>(&*I);
        if (Function *Callee = CS->getCalledFunction()) {
          // If this is a call to TM_EXECUTE_C, then operand 1 is a function
          // that needs to be processed, unless (a) it's a function pointer
          // that we don't know how to turn into a function, or (b) the
          // function we're calling does not have a definition in this TU
          if (Callee->getName() == TM_EXECUTE_C_STR) {
            // TODO: when we get rid of flags, 1 becomes 0
            if (Function *f = dyn_cast<Function>(CS->getArgOperand(1))) {
              if (!f->isDeclaration()) {
                func_worklist.push(f);
              }
            }
          }
        }
      }
    }
  }
}

/// Find annotated constructors, and put them in the work list
void tm_plugin::discover_constructor(Module &M) {
  /// a worklist for helping us to remove calls to TM_CTOR during discovery
  SmallVector<Instruction *, 128> ctor_list;

  // Find any call to TM_CTOR in any function's body
  //
  // TODO: again, clean up this list to avoid so much nesting.
  for (auto fn = M.getFunctionList().begin(), e = M.getFunctionList().end();
       fn != e; ++fn) {
    for (inst_iterator I = inst_begin(*fn), E = inst_end(*fn); I != E; ++I) {
      if (isa<CallInst>(*I) || isa<InvokeInst>(*I)) {
        CallBase *CS = cast<CallBase>(&*I); // CallSite is CallInst | InvokeInst
        if (Function *Callee = CS->getCalledFunction()) {
          if (Callee->getName() == TM_CTOR_STR) {
            if (Function *parent = CS->getCaller()) {
              if (!parent->isDeclaration()) {
                func_worklist.push(parent);
                ctor_list.push_back(&*I);
              }
            }
          }
        }
      }
    }
  }
  // Remove TM_CTOR calls
  while (!ctor_list.empty()) {
    auto *I = ctor_list.pop_back_val();
    I->eraseFromParent();
  }
}

/// Process the worklist until we are sure that there are no TM-reachable
/// functions that we have not found.  We want to find functions that are in
/// deep call chains, but we can only handle call chains that are entirely
/// within a Module.  Inter-module calls are not handled efficiently by our TM
/// plugin, because annotations are not part of the type system.  Thus
/// inter-module calls will lead to either (a) dynamic lookup, or (b)
/// serialization.
void tm_plugin::discover_reachable_funcs() {
  // Iterate over the annotated function queue to find all functions that
  // might need to be visited
  while (!func_worklist.empty()) {
    auto fn = func_worklist.front();
    func_worklist.pop();
    // TODO: Why don't we filter out pure functions eagerly?

    // If the current function is one we haven't seen before, save it and
    // process it
    if (functions.find(fn) == functions.end()) {
      // Track if the function is a lambda
      function_features f;
      f.orig = fn;
      if (std::find(lambdas.begin(), lambdas.end(), fn) != lambdas.end())
        f.orig_lambda = true;
      // If pure, clone == self; if TM_RENAME, clone \in renamelist; else delay
      // on making clone
      f.clone = nullptr;
      if (std::find(purelist.begin(), purelist.end(), fn) != purelist.end())
        f.clone = fn;
      auto origFn = renamelist.find(fn);
      if (origFn != renamelist.end()) {
        f.orig = (*origFn).second;
        f.clone = (*origFn).first;
        functions.insert({(*origFn).second, f});
      } else {
        functions.insert({fn, f});
      }
      // process this function by finding all of its call instructions and
      // adding them to the worklist
      for (inst_iterator I = inst_begin(*fn), E = inst_end(*fn); I != E; ++I) {
        // CallSite is CallInst | InvokeInst | Indirect call
        // TODO: use casting to handle this with less nesting
        if (isa<CallInst>(*I) || isa<InvokeInst>(*I)) {
          CallBase *CS = cast<CallBase>(&*I);
          // Get the function, figure out if we have a body for it.  If indirect
          // call, we'll get a nullptr.
          if (Function *func = CS->getCalledFunction())
            if (!(func->isDeclaration()))
              func_worklist.push(func);
        }
      }
    }
  }
}

/// Iterate through the list of discovered functions, and for each one, generate
/// a clone and add it to the module
///
/// NB: The interaction between this code and C++ name mangling is not what one
///     would expect.  If a mangled name is _Z18test_clone_noparamv, the clone
///     name will be tm__Z18test_clone_noparamv, not _Z21tm_test_clone_noparamv.
///     This appears to be a necessary cost of memory instrumentation without
///     front-end support
void tm_plugin::create_clones() {
  for (auto &fn : functions) {
    // If a function is a special function or it has a pure attribute,
    // there is no need to generate a clone version
    if (renamelist.find(fn.second.clone) == renamelist.end() &&
        std::find(purelist.begin(), purelist.end(), fn.first) ==
            purelist.end()) {

      // Sometimes, when cloning one function into another through the standard
      // LLVM utilities, the new function will have more arguments, or arguments
      // in a different order, than the original function.  Consequently,
      // CloneFunction takes a ValueToValueMap that explains how the arguments
      // to the original map to arguments of the clone. In our case, we are not
      // modifying the argument order or count, so an empty map is sufficient.
      ValueToValueMapTy v2vmap;

      // Create the clone.  We are using the simplest cloning technique that
      // LLVM offers.  The clone will go into the current module
      Function *newfunc = CloneFunction(fn.first, v2vmap, nullptr);

      // Give the new function a name by concatenating the TM_PREFIX_STR with
      // the original name of the function, and then save the new function so we
      // can instrument it in a later phase
      newfunc->setName(Twine(TM_PREFIX_STR, fn.first->getName()));
      fn.second.clone = newfunc;
    }
  }
}