#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

#include "../../common/tm_defines.h"

#include "tm_plugin.h"

using namespace llvm;

PreservedAnalyses tm_plugin::run(Module &M, ModuleAnalysisManager &) {
  // Initialize the signatures object and attach annotations to functions.
  sigs.init(M);
  attach_annotations_to_functions(M);

  populate_purelist(M);

  // Discover any RAII regions and any functions reachable from an RAII region
  discover_raii_lite(M);
  discover_reachable_raii_lite(M); // Must do this before discover_reachable!

  // Discover annotated functions, functions that start transactions in the
  // lambda and C apis, and annotated constructors.
  discover_annotated_funcs(M);
  discover_capi_funcs(M);
  discover_lambda_funcs(M);
  discover_constructor(M);

  // Find all functions reachable from any of the above roots
  discover_reachable_funcs();

  // Cloning functions and instrument clone bodies
  create_clones();
  instrument_function_bodies();

  // Boundary instrumentation for lambda and C apis
  convert_region_begin_c_api(M);
  convert_lambdas_cxx_api(M);

  // Boundary and scoped body instrumentation for RAII API
  instrument_regions_raii_lite(M);

  // Optimizations
  optimize_unsafe(M);

  // Add a static initializer to the module, so that the run-time system
  // can see the function-to-clone mapping
  create_runtime_mappings(M);

  return PreservedAnalyses::none();
}

PassPluginLibraryInfo getPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "tm_plugin", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "tm_plugin") {
                    MPM.addPass(tm_plugin());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getPluginInfo();
}
