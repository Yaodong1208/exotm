# Set up flags for the llvm plugin
CXXPASSSOFILE = $(TM_ROOT)/plugin/plugin/build/libtmplugin.so
OPTFLAGS      = -load-pass-plugin $(CXXPASSSOFILE) -passes=tm_plugin