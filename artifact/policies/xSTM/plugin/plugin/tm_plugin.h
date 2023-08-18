#pragma once

#include <queue>
#include <unordered_map>
#include <vector>

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include "signatures.h"
#include "types.h"

/// tm_plugin is a pass over modules that provides support for transactional
/// memory. It supports three APIs:
///   -- A lambda-based API, in which transactional instrumentation is at the
///      granularity of function bodies.
///   -- A legacy C API, also at the granularity of functions, that passes
///      function pointers and opaque argument pointers to a library.
///   -- An RAII-based API, in which the lifetime of a special "TX" object
///      dictates the lexical scope that requires instrumentation.
///
/// NB: This particular implementation uses a "lite" version of the RAII API. In
///     this version, we do not care about using HTM/CGL, nor do we care about
///     efficient irrevocability.  Thus we can simply instrument the RAII
///     regions, without cloning them.
class tm_plugin : public llvm::PassInfoMixin<tm_plugin> {
  /// All of the signatures for types and functions used by the plugin
  signatures sigs;

  /// Data required for the simplified RAII mechanism
  std::unordered_map<llvm::Function *, std::set<llvm::BasicBlock *> *>
      raii_lite_state;

  /// a worklist for managing the functions that require instrumentation.  These
  /// could be reachable from the RAII API, or could be discovered through the
  /// lambda or legacy API
  std::queue<llvm::Function *> func_worklist;

  /// The set of functions that need instrumentation. This set includes all
  /// of the functions that were explicitly annotated by the programmer, and all
  /// functions reachable from region launch points and annotated functions
  ///
  /// TODO: this is not a useful name
  std::unordered_map<llvm::Function *, function_features> functions;

  /// a list of all our suspected lambdas, since they require extra attention
  ///
  /// TODO: Are these the functions that we think are lambda bodes?
  ///
  /// TODO: The use of a vector introduces O(N) lookup overhead later in the
  ///       code
  std::vector<llvm::Function *> lambdas;

  /// a list of functions that have the tm_pure attribute
  std::vector<llvm::Function *> purelist;

  /// a list of functions that use the tm_rename attribute
  std::unordered_map<llvm::Function *, llvm::Function *> renamelist;

  /// A lookup function for finding things in the functions map
  llvm::Function *get_clone(llvm::Function *input_function) {
    auto function = functions.find(input_function);
    if (function != functions.end())
      return function->second.clone;
    else
      return nullptr;
  }

  /// Discovery: find all annotations in the Module and attach them to the
  /// corresponding functions
  void attach_annotations_to_functions(llvm::Module &M);

  /// Discovery: populate the pure list with things we know are pure
  void populate_purelist(llvm::Module &M);

  /// Discovery: find the functions in the module that are annotated, and put
  /// them into the worklist
  void discover_annotated_funcs(llvm::Module &M);

  /// Discovery: find the functions that are reached via TM_EXECUTE_C, and put
  /// them into the worklist
  void discover_capi_funcs(llvm::Module &M);

  /// Discovery: Find the lambda-based executions, and put them in the worklist
  /// and lambdas list
  void discover_lambda_funcs(llvm::Module &M);

  /// Discovery: Find all basic blocks that need instrumentation according to
  /// the RAII API, and put them in raii_regions
  void discover_raii_lite(llvm::Module &M);

  /// Discovery: Find the annotated constructors, and put them in the worklist
  void discover_constructor(llvm::Module &M);

  /// Discovery: Process the worklist until it is empty, in order to find all
  /// reachable functions.
  void discover_reachable_funcs();

  /// Discovery: Process the raii_regions list to find reachable functions.
  void discover_reachable_raii_lite(llvm::Module &M);

  /// Cloning: clone all functions in the worklist
  void create_clones();

  /// Function Instrumentation: Instrument the instrutions within an RAII region
  void instrument_regions_raii_lite(llvm::Module &M);

  /// Function Instrumentation: helper to transform a callsite to use a clone
  llvm::Instruction *transform_callsite(llvm::CallBase *callsite,
                                        llvm::BasicBlock::iterator inst);

  /// Function Instrumentation: helper to replace a call instruction with a new
  /// instruction
  llvm::Instruction *create_callinst(llvm::CallBase *callsite,
                                     llvm::BasicBlock::iterator inst,
                                     llvm::Value *val, llvm::Value *orig_val);

  /// Function Instrumentation: helper to replace an invoke instruction with a
  /// new instruction
  llvm::Instruction *create_invokeinst(llvm::CallBase *callsite,
                                       llvm::BasicBlock::iterator inst,
                                       llvm::Value *val, llvm::Value *orig_val);

  /// Function Instrumentation: helper to insert a call to TM_UNSAFE before the
  /// provided instruction
  void prefix_with_unsafe(llvm::BasicBlock::iterator inst);

  /// Function Instrumentation: helper to try to convert a store instruction
  /// into a tm_store
  llvm::CallInst *convert_store(llvm::StoreInst *store);

  /// Function Instrumentation: helper to try to convert a load instruction into
  /// a tm_load
  llvm::Instruction *convert_load(llvm::LoadInst *load);

  /// Function Instrumentation: helper to try to convert a intrinsic instruction
  /// into a safe call
  void convert_intrinsics(llvm::Function *callee,
                          llvm::BasicBlock::iterator inst);

  /// Function Instrumentation: Main routine for function body transformation
  void instrument_function_bodies();

  /// A helper for instrument_function_bodies
  void instrument_bb(llvm::BasicBlock *bb,
                     llvm::SmallVector<llvm::Instruction *, 8> &skips);

  /// Boundary Instrumentation: transform region start commands that use the C
  /// API
  void convert_region_begin_c_api(llvm::Module &M);

  /// Boundary Instrumentation: transform the code inside of lambdas
  void convert_lambdas_cxx_api(llvm::Module &M);

  /// Run-time Mappings: add the static initializer that tells the runtime
  /// library about function-to-clone mappings
  void create_runtime_mappings(llvm::Module &M);

  /// Optimization: remove extra unsafe function calls from a basic block
  void optimize_unsafe(llvm::Module &M);

public:
  /// Constructor: call the super's constructor and set up the plugin
  tm_plugin() {}

  /// Instrument a module
  ///
  /// @param M The module to instrument
  /// @param _ The module analysis manager, unused in this case
  ///
  /// @return Information about which analyses were preserved.  Typically none.
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};