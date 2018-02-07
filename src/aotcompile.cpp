// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "llvm-version.h"
#include "platform.h"
#include "options.h"
#include <iostream>
#include <sstream>

// analysis passes
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/TypeBasedAliasAnalysis.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Verifier.h>
#if defined(USE_POLLY)
#include <polly/RegisterPasses.h>
#include <polly/LinkAllPasses.h>
#include <polly/CodeGen/CodegenCleanup.h>
#if defined(USE_POLLY_ACC)
#include <polly/Support/LinkGPURuntime.h>
#endif
#endif
// for outputting assembly
#include <llvm/CodeGen/Passes.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/Target/TargetLoweringObjectFile.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCStreamer.h>

#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Instrumentation.h>
#include <llvm/Transforms/Vectorize.h>
#include <llvm/Transforms/Scalar/GVN.h>
#if JL_LLVM_VERSION >= 40000
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#endif

namespace llvm {
    extern Pass *createLowerSimdLoopPass();
}

#if JL_LLVM_VERSION >= 40000
#  include <llvm/Bitcode/BitcodeWriter.h>
#else
#  include <llvm/Bitcode/ReaderWriter.h>
#endif
#include <llvm/Bitcode/BitcodeWriterPass.h>

#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/Transforms/Utils/Cloning.h>

// target support
#include <llvm/ADT/Triple.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/DynamicLibrary.h>


#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/ADT/SmallSet.h>

using namespace llvm;

#include "julia.h"
#include "julia_internal.h"
#include "codegen_shared.h"
#include "jitlayers.h"
#include "julia_assert.h"

// MSVC's link.exe requires each function declaration to have a Comdat section
// So rather than litter the code with conditionals,
// all global values that get emitted call this function
// and it decides whether the definition needs a Comdat section and adds the appropriate declaration
template<class T> // for GlobalObject's
static T *addComdat(T *G)
{
#if defined(_OS_WINDOWS_)
    if (imaging_mode && !G->isDeclaration()) {
        // Add comdat information to make MSVC link.exe happy
        // it's valid to emit this for ld.exe too,
        // but makes it very slow to link for no benefit
        if (G->getParent() == shadow_output) {
#if defined(_COMPILER_MICROSOFT_)
            Comdat *jl_Comdat = G->getParent()->getOrInsertComdat(G->getName());
            // ELF only supports Comdat::Any
            jl_Comdat->setSelectionKind(Comdat::NoDuplicates);
            G->setComdat(jl_Comdat);
#endif
#if defined(_CPU_X86_64_)
            // Add unwind exception personalities to functions to handle async exceptions
            assert(!juliapersonality_func || juliapersonality_func->getParent() == shadow_output);
            if (Function *F = dyn_cast<Function>(G))
                F->setPersonalityFn(juliapersonality_func);
#endif
        }
        // add __declspec(dllexport) to everything marked for export
        if (G->getLinkage() == GlobalValue::ExternalLinkage)
            G->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
        else
            G->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    }
#endif
    return G;
}


typedef struct {
    Value *gv;
    int32_t index; // uses 1-based indexing
} jl_value_llvm;
static std::vector<GlobalValue*> jl_sysimg_gvars;
static std::map<void*, jl_value_llvm> jl_value_to_llvm;

typedef struct {
    std::unique_ptr<Module> M;
    std::vector<GlobalValue*> jl_sysimg_fvars;
    std::vector<GlobalValue*> jl_sysimg_gvars;
    std::map<jl_method_instance_t *, std::tuple<uint8_t, uint32_t, uint32_t>> jl_fvar_map;
} jl_native_code_desc_t;

// global variables to pointers are pretty common,
// so this method is available as a convenience for emitting them.
// for other types, the formula for implementation is straightforward:
// (see stringConstPtr, for an alternative example to the code below)
//
// if in imaging_mode, emit a GlobalVariable with the same name and an initializer to the shadow_module
// making it valid for emission and reloading in the sysimage
//
// then add a global mapping to the current value (usually from calloc'd space)
// to the execution engine to make it valid for the current session (with the current value)
void* jl_emit_and_add_to_shadow(GlobalVariable *gv, void *gvarinit)
{
    PointerType *T = cast<PointerType>(gv->getType()->getElementType()); // pointer is the only supported type here

    GlobalVariable *shadowvar = NULL;
    if (imaging_mode)
        shadowvar = global_proto(gv, shadow_output);

    if (shadowvar) {
        shadowvar->setInitializer(ConstantPointerNull::get(T));
        shadowvar->setLinkage(GlobalVariable::InternalLinkage);
        addComdat(shadowvar);
        if (imaging_mode && gvarinit) {
            // make the pointer valid for future sessions
            jl_sysimg_gvars.push_back(shadowvar);
            jl_value_llvm gv_struct;
            gv_struct.gv = global_proto(gv);
            gv_struct.index = jl_sysimg_gvars.size();
            jl_value_to_llvm[gvarinit] = gv_struct;
        }
    }

    // make the pointer valid for this session
    void *slot = calloc(1, sizeof(void*));
    jl_ExecutionEngine->addGlobalMapping(gv, slot);
    return slot;
}

GlobalVariable *jl_get_global_for(const char *cname, void *addr, Module *M, Type* T)
{
    // emit a GlobalVariable for a jl_value_t named "cname"
    std::map<void*, jl_value_llvm>::iterator it;
    // first see if there already is a GlobalVariable for this address
    it = jl_value_to_llvm.find(addr);
    if (it != jl_value_to_llvm.end())
        return prepare_global_in(M, (llvm::GlobalVariable*)it->second.gv);

    std::stringstream gvname;
    gvname << cname << globalUnique++;
    // no existing GlobalVariable, create one and store it
    GlobalVariable *gv = new GlobalVariable(*M, T,
                           false, GlobalVariable::ExternalLinkage,
                           NULL, gvname.str());
    *(void**)jl_emit_and_add_to_shadow(gv, addr) = addr;
    return gv;
}

static void emit_offset_table(Module *mod, const std::vector<GlobalValue*> &vars, StringRef name, Type *T_psize)
{
    // Emit a global variable with all the variable addresses.
    // The cloning pass will convert them into offsets.
    assert(!vars.empty());
    size_t nvars = vars.size();
    std::vector<Constant*> addrs(nvars);
    for (size_t i = 0; i < nvars; i++)
        addrs[i] = ConstantExpr::getBitCast(vars[i], T_psize);
    ArrayType *vars_type = ArrayType::get(T_psize, nvars);
    new GlobalVariable(*mod, vars_type, true,
                       GlobalVariable::ExternalLinkage,
                       ConstantArray::get(vars_type, addrs),
                       name);
}

static void jl_gen_llvm_globaldata(jl_native_code_desc_t *data, const char *sysimg_data, size_t sysimg_len)
{
    Module *mod = data->M.get();
    Type *T_size;
    if (sizeof(size_t) == 8)
        T_size = Type::getInt64Ty(mod->getContext());
    else
        T_size = Type::getInt32Ty(mod->getContext());
    Type *T_psize = T_size->getPointerTo();
    emit_offset_table(mod, data->jl_sysimg_gvars, "jl_sysimg_gvars", T_psize);
    emit_offset_table(mod, data->jl_sysimg_fvars, "jl_sysimg_fvars", T_psize);
    addComdat(new GlobalVariable(*mod,
                                 T_size,
                                 true,
                                 GlobalVariable::ExternalLinkage,
                                 ConstantInt::get(T_size, globalUnique+1),
                                 "jl_globalUnique"));

    // reflect the address of the jl_RTLD_DEFAULT_handle variable
    // back to the caller, so that we can check for consistency issues
    GlobalValue *jlRTLD_DEFAULT_var = mod->getNamedValue("jl_RTLD_DEFAULT_handle");
    addComdat(new GlobalVariable(*mod,
                                 jlRTLD_DEFAULT_var->getType(),
                                 true,
                                 GlobalVariable::ExternalLinkage,
                                 jlRTLD_DEFAULT_var,
                                 "jl_RTLD_DEFAULT_handle_pointer"));

    if (sysimg_data) {
        Constant *data = ConstantDataArray::get(mod->getContext(),
            ArrayRef<uint8_t>((const unsigned char*)sysimg_data, sysimg_len));
        addComdat(new GlobalVariable(*mod, data->getType(), false,
                                     GlobalVariable::ExternalLinkage,
                                     data, "jl_system_image_data"))->setAlignment(64);
        Constant *len = ConstantInt::get(T_size, sysimg_len);
        addComdat(new GlobalVariable(*mod, len->getType(), true,
                                     GlobalVariable::ExternalLinkage,
                                     len, "jl_system_image_size"));
    }
}

extern "C"
int32_t jl_get_llvm_gv(jl_value_t *p)
{
    // map a jl_value_t memory location to a GlobalVariable
    auto it = jl_value_to_llvm.find(p);
    if (it == jl_value_to_llvm.end())
        return 0;
    return it->second.index;
}

extern "C"
void jl_get_function_id(void *native_code, jl_method_instance_t *linfo,
        uint8_t *api, uint32_t *func_idx, uint32_t *specfunc_idx)
{
    jl_native_code_desc_t *data = (jl_native_code_desc_t*)native_code;
    // get the function index in the fvar lookup table
    auto it = data->jl_fvar_map.find(linfo);
    if (it != data->jl_fvar_map.end()) {
        std::tie(*api, *func_idx, *specfunc_idx) = it->second;
    }
}

// takes the running content that has collected in the shadow module and dump it to disk
// this builds the object file portion of the sysimage files for fast startup
extern "C"
void *jl_create_native(jl_array_t *methods)
{
    jl_native_code_desc_t *data = new jl_native_code_desc_t;
    jl_codegen_call_targets_t workqueue;
    std::map<jl_method_instance_t *, jl_compile_result_t> emitted;
    jl_method_instance_t *mi = NULL;
    jl_code_info_t *src = NULL;
    JL_GC_PUSH1(&src);
    JL_LOCK(&codegen_lock);

    for (int worlds = 2; worlds > 0; worlds--) {
        size_t world = (worlds == 1 ? jl_world_counter : jl_typeinf_world);
        if (!world)
            continue;
        size_t i, l;
        for (i = 0, l = jl_array_len(methods); i < l; i++) {
            mi = (jl_method_instance_t*)jl_array_ptr_ref(methods, i);
            if ((worlds == 1 || mi->max_world < jl_world_counter) && mi->min_world <= world && world <= mi->max_world) {
                src = (jl_code_info_t*)mi->inferred;
                if (src && (jl_value_t*)src != jl_nothing)
                    src = jl_uncompress_ast(mi->def.method, (jl_array_t*)src);
                if (!src || !jl_is_code_info(src)) {
                    src = jl_type_infer(&mi, world, 0);
                }
                if (!emitted.count(mi)) {
                    jl_compile_result_t result = jl_compile_linfo1(mi, src, world, workqueue, false, &jl_default_cgparams);
                    if (std::get<0>(result))
                        emitted[mi] = std::move(result);
                }
            }
        }
        jl_compile_workqueue(world, false, emitted, workqueue);
    }
    JL_GC_POP();

    // clones the contents of the module `m` to the shadow_output collector
    ValueToValueMapTy VMap;
    std::unique_ptr<Module> clone(CloneModule(shadow_output, VMap));
    for (auto &def : emitted) {
        jl_merge_module(clone.get(), std::move(std::get<0>(def.second)));
        jl_method_instance_t *this_li = def.first;
        jl_llvm_functions_t decls = std::get<1>(def.second);
        jl_value_t *rettype = std::get<2>(def.second);
        uint8_t api = std::get<3>(def.second);
        Function *func = cast<Function>(clone->getNamedValue(decls.functionObject));
        Function *cfunc = NULL;
        if (!decls.functionObject.empty())
            cfunc = cast<Function>(clone->getNamedValue(decls.functionObject));
        uint32_t func_id = 0;
        uint32_t cfunc_id = 0;
        if (cfunc && this_li->rettype == rettype) {
            data->jl_sysimg_fvars.push_back(cfunc);
            cfunc_id = data->jl_sysimg_fvars.size();
        }
        data->jl_sysimg_fvars.push_back(func);
        func_id = data->jl_sysimg_fvars.size();
        data->jl_fvar_map[this_li] = std::make_tuple(api, cfunc_id, func_id);
    }

    for (Module::iterator I = clone->begin(), E = clone->end(); I != E; ++I) {
        Function *F = &*I;
        if (!F->isDeclaration()) {
            F->setLinkage(Function::InternalLinkage);
            addComdat(F);
        }
    }

    data->jl_sysimg_gvars = jl_sysimg_gvars;
    for (size_t i = 0; i < data->jl_sysimg_gvars.size(); i++)
        data->jl_sysimg_gvars[i] = cast<llvm::GlobalValue>(VMap[data->jl_sysimg_gvars[i]]);
    data->M = std::move(clone);

    JL_UNLOCK(&codegen_lock); // Might GC
    return (void*)data;
}


// takes the running content that has collected in the shadow module and dump it to disk
// this builds the object file portion of the sysimage files for fast startup
extern "C"
void jl_dump_native(void *native_code,
        const char *bc_fname, const char *unopt_bc_fname, const char *obj_fname,
        const char *sysimg_data, size_t sysimg_len)
{
    jl_native_code_desc_t *data = (jl_native_code_desc_t*)native_code;
    JL_TIMING(NATIVE_DUMP);
    // We don't want to use MCJIT's target machine because
    // it uses the large code model and we may potentially
    // want less optimizations there.
    Triple TheTriple = Triple(jl_TargetMachine->getTargetTriple());
    // make sure to emit the native object format, even if FORCE_ELF was set in codegen
#if defined(_OS_WINDOWS_)
    TheTriple.setObjectFormat(Triple::COFF);
#elif defined(_OS_DARWIN_)
    TheTriple.setObjectFormat(Triple::MachO);
    TheTriple.setOS(llvm::Triple::MacOSX);
#endif
    std::unique_ptr<TargetMachine>
    TM(jl_TargetMachine->getTarget().createTargetMachine(
        TheTriple.getTriple(),
        jl_TargetMachine->getTargetCPU(),
        jl_TargetMachine->getTargetFeatureString(),
        jl_TargetMachine->Options,
#if defined(_OS_LINUX_) || defined(_OS_FREEBSD_)
        Reloc::PIC_,
#else
        Optional<Reloc::Model>(),
#endif
        // Use small model so that we can use signed 32bits offset in the function and GV tables
        CodeModel::Small,
        CodeGenOpt::Aggressive // -O3 TODO: respect command -O0 flag?
        ));

    legacy::PassManager PM;
    addTargetPasses(&PM, TM.get());

    // set up optimization passes
    std::unique_ptr<raw_fd_ostream> unopt_bc_OS;
    std::unique_ptr<raw_fd_ostream> bc_OS;
    std::unique_ptr<raw_fd_ostream> obj_OS;

    if (unopt_bc_fname) {
        // call output handler directly to avoid special case handling of `-` filename
        int FD;
        std::error_code EC = sys::fs::openFileForWrite(unopt_bc_fname, FD, sys::fs::F_None);
        unopt_bc_OS.reset(new raw_fd_ostream(FD, true));
        std::string err;
        if (EC)
            err = "ERROR: failed to open --output-unopt-bc file '" + std::string(unopt_bc_fname) + "': " + EC.message();
        if (!err.empty())
            jl_safe_printf("%s\n", err.c_str());
        else {
            PM.add(createBitcodeWriterPass(*unopt_bc_OS.get()));
        }
    }

    if (bc_fname || obj_fname)
        addOptimizationPasses(&PM, jl_options.opt_level, true);

    if (bc_fname) {
        // call output handler directly to avoid special case handling of `-` filename
        int FD;
        std::error_code EC = sys::fs::openFileForWrite(bc_fname, FD, sys::fs::F_None);
        bc_OS.reset(new raw_fd_ostream(FD, true));
        std::string err;
        if (EC)
            err = "ERROR: failed to open --output-bc file '" + std::string(bc_fname) + "': " + EC.message();
        if (!err.empty())
            jl_safe_printf("%s\n", err.c_str());
        else {
            PM.add(createBitcodeWriterPass(*bc_OS.get()));
        }
    }

    if (obj_fname) {
        // call output handler directly to avoid special case handling of `-` filename
        int FD;
        std::error_code EC = sys::fs::openFileForWrite(obj_fname, FD, sys::fs::F_None);
        obj_OS.reset(new raw_fd_ostream(FD, true));
        std::string err;
        if (EC)
            err = "ERROR: failed to open --output-o file '" + std::string(obj_fname) + "': " + EC.message();
        if (!err.empty())
            jl_safe_printf("%s\n", err.c_str());
        else {
            if (TM->addPassesToEmitFile(PM, *obj_OS.get(), TargetMachine::CGFT_ObjectFile, false)) {
                jl_safe_printf("ERROR: target does not support generation of object files\n");
            }
        }
    }

    // Reset the target triple to make sure it matches the new target machine
    data->M->setTargetTriple(TM->getTargetTriple().str());
#if JL_LLVM_VERSION >= 40000
    DataLayout DL = TM->createDataLayout();
    DL.reset(DL.getStringRepresentation() + "-ni:10:11:12");
    data->M->setDataLayout(DL);
#else
    data->M->setDataLayout(TM->createDataLayout());
#endif

    // add metadata information
    if (imaging_mode)
        jl_gen_llvm_globaldata(data, sysimg_data, sysimg_len);

    // do the actual work
    PM.run(*data->M);
    imaging_mode = false;

    delete data;
}

// clones the contents of the module `m` to the shadow_output collector
// TODO: this is deprecated
void jl_add_to_shadow(Module *m)
{
    ValueToValueMapTy VMap;
    std::unique_ptr<Module> clone(CloneModule(m, VMap));
    for (Module::iterator I = clone->begin(), E = clone->end(); I != E; ++I) {
        Function *F = &*I;
        if (!F->isDeclaration()) {
            F->setLinkage(Function::InternalLinkage);
            addComdat(F);
        }
    }
    jl_merge_module(shadow_output, std::move(clone));
}
