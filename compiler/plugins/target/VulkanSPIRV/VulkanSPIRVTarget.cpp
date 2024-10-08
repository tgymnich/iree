// Copyright 2019 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/Dialect/Codegen/IR/IREECodegenDialect.h"
#include "iree/compiler/Codegen/Dialect/GPU/TargetUtils/KnownTargets.h"
#include "iree/compiler/Codegen/SPIRV/Passes.h"
#include "iree/compiler/Dialect/HAL/Target/TargetRegistry.h"
#include "iree/compiler/PluginAPI/Client.h"
#include "iree/compiler/Utils/FlatbufferUtils.h"
#include "iree/compiler/Utils/ModuleUtils.h"
#include "iree/schemas/spirv_executable_def_builder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/AsmParser/AsmParser.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVAttributes.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVOps.h"
#include "mlir/Dialect/SPIRV/IR/TargetAndABI.h"
#include "mlir/Dialect/SPIRV/Linking/ModuleCombiner.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectResourceBlobManager.h"
#include "mlir/Target/SPIRV/Serialization.h"

namespace mlir::iree_compiler::IREE::HAL {

namespace {
struct VulkanSPIRVTargetOptions {
  // Use vp_android_baseline_2022 profile as the default target--it's a good
  // lowest common denominator to guarantee the generated SPIR-V is widely
  // accepted for now. Eventually we want to use a list for multi-targeting.
  std::string target = "vp_android_baseline_2022";
  bool indirectBindings = false;

  void bindOptions(OptionsBinder &binder) {
    static llvm::cl::OptionCategory category("VulkanSPIRV HAL Target");
    binder.opt<std::string>(
        "iree-vulkan-target", target,
        llvm::cl::desc(
            "Vulkan target controlling the SPIR-V environment. Given the wide "
            "support of Vulkan, this option supports a few schemes: 1) LLVM "
            "CodeGen backend style: e.g., 'gfx*' for AMD GPUs and 'sm_*' for "
            "NVIDIA GPUs; 2) architecture code name style: e.g., "
            "'rdna3'/'valhall4'/'ampere'/'adreno' for AMD/ARM/NVIDIA/Qualcomm "
            "GPUs; 3) product name style: 'rx7900xtx'/'rtx4090' for AMD/NVIDIA "
            "GPUs. See "
            "https://iree.dev/guides/deployment-configurations/gpu-vulkan/ for "
            "more details."));
    binder.opt<bool>(
        "iree-vulkan-experimental-indirect-bindings", indirectBindings,
        llvm::cl::desc(
            "Force indirect bindings for all generated dispatches."));
  }
};
} // namespace

// TODO: VulkanOptions for choosing the Vulkan version and extensions/features.
class VulkanTargetDevice : public TargetDevice {
public:
  VulkanTargetDevice(const VulkanSPIRVTargetOptions &options)
      : options_(options) {}

  IREE::HAL::DeviceTargetAttr
  getDefaultDeviceTarget(MLIRContext *context,
                         const TargetRegistry &targetRegistry) const override {
    Builder b(context);
    SmallVector<NamedAttribute> configItems;

    auto configAttr = b.getDictionaryAttr(configItems);

    SmallVector<IREE::HAL::ExecutableTargetAttr> executableTargetAttrs;
    targetRegistry.getTargetBackend("vulkan-spirv")
        ->getDefaultExecutableTargets(context, "vulkan", configAttr,
                                      executableTargetAttrs);

    return IREE::HAL::DeviceTargetAttr::get(context, b.getStringAttr("vulkan"),
                                            configAttr, executableTargetAttrs);
  }

private:
  const VulkanSPIRVTargetOptions &options_;
};

class VulkanSPIRVTargetBackend : public TargetBackend {
public:
  VulkanSPIRVTargetBackend(const VulkanSPIRVTargetOptions &options)
      : options_(options) {}

  std::string getLegacyDefaultDeviceID() const override { return "vulkan"; }

  void getDefaultExecutableTargets(
      MLIRContext *context, StringRef deviceID, DictionaryAttr deviceConfigAttr,
      SmallVectorImpl<IREE::HAL::ExecutableTargetAttr> &executableTargetAttrs)
      const override {
    executableTargetAttrs.push_back(
        getExecutableTarget(context, options_.indirectBindings));
  }

  IREE::HAL::ExecutableTargetAttr
  getExecutableTarget(MLIRContext *context, bool indirectBindings) const {
    Builder b(context);
    SmallVector<NamedAttribute> configItems;
    auto addConfig = [&](StringRef name, Attribute value) {
      configItems.emplace_back(b.getStringAttr(name), value);
    };

    if (auto target = GPU::getVulkanTargetDetails(options_.target, context)) {
      addConfig("iree.gpu.target", target);
    } else {
      emitError(b.getUnknownLoc(), "Unknown Vulkan target '")
          << options_.target << "'";
      return nullptr;
    }

    return IREE::HAL::ExecutableTargetAttr::get(
        context, b.getStringAttr("vulkan-spirv"),
        indirectBindings ? b.getStringAttr("vulkan-spirv-fb-ptr")
                         : b.getStringAttr("vulkan-spirv-fb"),
        b.getDictionaryAttr(configItems));
  }

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<IREE::Codegen::IREECodegenDialect, spirv::SPIRVDialect,
                    gpu::GPUDialect, IREE::GPU::IREEGPUDialect>();
  }

  void
  buildConfigurationPassPipeline(IREE::HAL::ExecutableTargetAttr targetAttr,
                                 OpPassManager &passManager) override {
    buildSPIRVCodegenConfigurationPassPipeline(passManager);
  }

  void buildTranslationPassPipeline(IREE::HAL::ExecutableTargetAttr targetAttr,
                                    OpPassManager &passManager) override {
    buildSPIRVCodegenPassPipeline(passManager);
  }

  void buildLinkingPassPipeline(OpPassManager &passManager) override {
    buildSPIRVLinkingPassPipeline(passManager);
  }

  LogicalResult serializeExecutable(const SerializationOptions &options,
                                    IREE::HAL::ExecutableVariantOp variantOp,
                                    OpBuilder &executableBuilder) override {
    // Today we special-case external variants but in the future we could allow
    // for a linking approach allowing both code generation and external .spv
    // files to be combined together.
    if (variantOp.isExternal()) {
      return serializeExternalExecutable(options, variantOp, executableBuilder);
    }

    ModuleOp innerModuleOp = variantOp.getInnerModule();
    auto spirvModuleOps = innerModuleOp.getOps<spirv::ModuleOp>();
    if (spirvModuleOps.empty()) {
      return variantOp.emitError() << "should contain some spirv.module ops";
    }

    DenseMap<StringRef, uint64_t> entryPointOrdinals;

    SmallVector<IREE::HAL::ExecutableExportOp> exportOps =
        llvm::to_vector(variantOp.getOps<IREE::HAL::ExecutableExportOp>());
    for (auto exportOp : exportOps) {
      uint64_t ordinal = 0;
      if (std::optional<APInt> optionalOrdinal = exportOp.getOrdinal()) {
        ordinal = optionalOrdinal->getZExtValue();
      } else {
        // For executables with only one entry point, linking doesn't kick in at
        // all. So the ordinal can be missing for this case.
        if (!llvm::hasSingleElement(exportOps)) {
          return exportOp.emitError() << "should have ordinal attribute";
        }
      }
      entryPointOrdinals[exportOp.getSymName()] = ordinal;
    }
    uint64_t ordinalCount = entryPointOrdinals.size();

    FlatbufferBuilder builder;
    iree_hal_spirv_ExecutableDef_start_as_root(builder);

    // Attach embedded source file contents.
    SmallVector<iree_hal_spirv_SourceFileDef_ref_t> sourceFileRefs;
    if (auto sourcesAttr = variantOp.getSourcesAttr()) {
      for (auto sourceAttr : llvm::reverse(sourcesAttr.getValue())) {
        if (auto resourceAttr = dyn_cast_if_present<DenseResourceElementsAttr>(
                sourceAttr.getValue())) {
          auto filenameRef = builder.createString(sourceAttr.getName());
          auto contentRef = builder.streamUint8Vec([&](llvm::raw_ostream &os) {
            auto blobData = resourceAttr.getRawHandle().getBlob()->getData();
            os.write(blobData.data(), blobData.size());
            return true;
          });
          sourceFileRefs.push_back(iree_hal_spirv_SourceFileDef_create(
              builder, filenameRef, contentRef));
        }
      }
      std::reverse(sourceFileRefs.begin(), sourceFileRefs.end());
    }

    // The list of shader modules.
    SmallVector<iree_hal_spirv_ShaderModuleDef_ref_t> shaderModuleRefs;

    // Per entry-point data.
    // Note that the following vectors should all be of the same size and
    // element at index #i is for entry point with ordinal #i!
    SmallVector<StringRef> entryPointNames;
    SmallVector<uint32_t> subgroupSizes;
    SmallVector<uint32_t> shaderModuleIndices;
    SmallVector<iree_hal_spirv_FileLineLocDef_ref_t> sourceLocationRefs;
    entryPointNames.resize(ordinalCount);
    subgroupSizes.resize(ordinalCount);
    shaderModuleIndices.resize(ordinalCount);

    // Iterate over all spirv.module ops and encode them into the FlatBuffer
    // data structure.
    bool hasAnySubgroupSizes = false;
    for (spirv::ModuleOp spvModuleOp : spirvModuleOps) {
      // Currently the spirv.module op should only have one entry point. Get it.
      auto spirvEntryPoints = spvModuleOp.getOps<spirv::EntryPointOp>();
      if (!llvm::hasSingleElement(spirvEntryPoints)) {
        return spvModuleOp.emitError()
               << "expected to contain exactly one entry point";
      }
      spirv::EntryPointOp spvEntryPoint = *spirvEntryPoints.begin();
      uint64_t ordinal = entryPointOrdinals.at(spvEntryPoint.getFn());

      if (!options.dumpIntermediatesPath.empty()) {
        std::string assembly;
        llvm::raw_string_ostream os(assembly);
        spvModuleOp.print(os, OpPrintingFlags().useLocalScope());
        dumpDataToPath(options.dumpIntermediatesPath, options.dumpBaseName,
                       spvEntryPoint.getFn(), ".spirv.mlir", assembly);
      }

      // Serialize the spirv::ModuleOp into the binary blob.
      SmallVector<uint32_t, 0> spvBinary;
      if (failed(spirv::serialize(spvModuleOp, spvBinary)) ||
          spvBinary.empty()) {
        return spvModuleOp.emitError() << "failed to serialize";
      }
      if (!options.dumpBinariesPath.empty()) {
        dumpDataToPath<uint32_t>(options.dumpBinariesPath, options.dumpBaseName,
                                 spvEntryPoint.getFn(), ".spv", spvBinary);
      }
      auto spvCodeRef = flatbuffers_uint32_vec_create(builder, spvBinary.data(),
                                                      spvBinary.size());
      shaderModuleIndices[ordinal] = shaderModuleRefs.size();
      shaderModuleRefs.push_back(
          iree_hal_spirv_ShaderModuleDef_create(builder, spvCodeRef));

      // The IREE runtime uses ordinals instead of names. We need to attach the
      // entry point name for VkShaderModuleCreateInfo.
      entryPointNames[ordinal] = spvEntryPoint.getFn();

      // If there are subgroup size requests, we need to pick up too.
      auto fn = spvModuleOp.lookupSymbol<spirv::FuncOp>(spvEntryPoint.getFn());
      auto abi = fn->getAttrOfType<spirv::EntryPointABIAttr>(
          spirv::getEntryPointABIAttrName());
      if (abi && abi.getSubgroupSize()) {
        subgroupSizes[ordinal] = *abi.getSubgroupSize();
        hasAnySubgroupSizes = true;
      } else {
        subgroupSizes[ordinal] = 0;
      }

      // Optional source location information for debugging/profiling.
      if (options.debugLevel >= 1) {
        if (auto loc = findFirstFileLoc(spvEntryPoint.getLoc())) {
          // We only ever resize to the maximum -- so all previous data will be
          // kept as-is.
          sourceLocationRefs.resize(ordinalCount);
          auto filenameRef = builder.createString(loc->getFilename());
          sourceLocationRefs[ordinal] = iree_hal_spirv_FileLineLocDef_create(
              builder, filenameRef, loc->getLine());
        }
      }
    }

    // Optional compilation stage source files.
    SmallVector<iree_hal_spirv_StageLocationsDef_ref_t> stageLocationsRefs;
    if (options.debugLevel >= 3) {
      for (auto exportOp : exportOps) {
        SmallVector<iree_hal_spirv_StageLocationDef_ref_t> stageLocationRefs;
        if (auto locsAttr = exportOp.getSourceLocsAttr()) {
          for (auto locAttr : locsAttr.getValue()) {
            if (auto loc =
                    findFirstFileLoc(cast<LocationAttr>(locAttr.getValue()))) {
              auto stageNameRef = builder.createString(locAttr.getName());
              auto filenameRef = builder.createString(loc->getFilename());
              stageLocationRefs.push_back(
                  iree_hal_spirv_StageLocationDef_create(
                      builder, stageNameRef,
                      iree_hal_spirv_FileLineLocDef_create(builder, filenameRef,
                                                           loc->getLine())));
            }
          }
        }
        if (!stageLocationRefs.empty()) {
          // We only ever resize to the maximum -- so all previous data will
          // be kept as-is.
          stageLocationsRefs.resize(ordinalCount);
          int64_t ordinal = exportOp.getOrdinalAttr().getInt();
          stageLocationsRefs[ordinal] = iree_hal_spirv_StageLocationsDef_create(
              builder, builder.createOffsetVecDestructive(stageLocationRefs));
        }
      }
    }

    // Add top-level executable fields following their order of definition.
    auto entryPointsRef = builder.createStringVec(entryPointNames);
    flatbuffers_int32_vec_ref_t subgroupSizesRef =
        hasAnySubgroupSizes ? builder.createInt32Vec(subgroupSizes) : 0;
    flatbuffers_int32_vec_ref_t shaderModuleIndicesRef =
        builder.createInt32Vec(shaderModuleIndices);
    iree_hal_spirv_ExecutableDef_entry_points_add(builder, entryPointsRef);
    if (subgroupSizesRef) {
      iree_hal_spirv_ExecutableDef_subgroup_sizes_add(builder,
                                                      subgroupSizesRef);
    }
    iree_hal_spirv_ExecutableDef_shader_module_indices_add(
        builder, shaderModuleIndicesRef);
    auto shaderModulesRef =
        builder.createOffsetVecDestructive(shaderModuleRefs);
    iree_hal_spirv_ExecutableDef_shader_modules_add(builder, shaderModulesRef);
    if (!sourceLocationRefs.empty()) {
      auto sourceLocationsRef =
          builder.createOffsetVecDestructive(sourceLocationRefs);
      iree_hal_spirv_ExecutableDef_source_locations_add(builder,
                                                        sourceLocationsRef);
    }
    if (!stageLocationsRefs.empty()) {
      auto stageLocationsRef =
          builder.createOffsetVecDestructive(stageLocationsRefs);
      iree_hal_spirv_ExecutableDef_stage_locations_add(builder,
                                                       stageLocationsRef);
    }
    if (!sourceFileRefs.empty()) {
      auto sourceFilesRef = builder.createOffsetVecDestructive(sourceFileRefs);
      iree_hal_spirv_ExecutableDef_source_files_add(builder, sourceFilesRef);
    }

    iree_hal_spirv_ExecutableDef_end_as_root(builder);

    // Add the binary data to the target executable.
    auto binaryOp = executableBuilder.create<IREE::HAL::ExecutableBinaryOp>(
        variantOp.getLoc(), variantOp.getSymName(),
        variantOp.getTarget().getFormat(),
        builder.getBufferAttr(executableBuilder.getContext()));
    binaryOp.setMimeTypeAttr(
        executableBuilder.getStringAttr("application/x-flatbuffers"));

    return success();
  }

  LogicalResult
  serializeExternalExecutable(const SerializationOptions &options,
                              IREE::HAL::ExecutableVariantOp variantOp,
                              OpBuilder &executableBuilder) {
    if (!variantOp.getObjects().has_value()) {
      return variantOp.emitOpError()
             << "no objects defined for external variant";
    } else if (variantOp.getObjects()->getValue().size() != 1) {
      // For now we assume there will be exactly one object file.
      // TODO(#7824): support multiple .spv files in a single flatbuffer archive
      // so that we can combine executables.
      return variantOp.emitOpError() << "only one object reference is "
                                        "supported for external variants";
    }

    // Take exported names verbatim for passing into VkShaderModuleCreateInfo.
    SmallVector<StringRef, 8> entryPointNames;
    for (auto exportOp : variantOp.getExportOps()) {
      entryPointNames.emplace_back(exportOp.getSymName());
    }
    // We only have one object file for now. So all entry points have shader
    // module index 0.
    SmallVector<uint32_t, 8> shaderModuleIndices(entryPointNames.size(), 0);

    // Load .spv object file.
    auto objectAttr = llvm::cast<IREE::HAL::ExecutableObjectAttr>(
        variantOp.getObjects()->getValue().front());
    std::string spvBinary;
    if (auto data = objectAttr.loadData()) {
      spvBinary = data.value();
    } else {
      return variantOp.emitOpError()
             << "object file could not be loaded: " << objectAttr;
    }
    if (spvBinary.size() % 4 != 0) {
      return variantOp.emitOpError()
             << "object file is not 4-byte aligned as expected for SPIR-V";
    }

    FlatbufferBuilder builder;
    iree_hal_spirv_ExecutableDef_start_as_root(builder);

    auto spvCodeRef = flatbuffers_uint32_vec_create(
        builder, reinterpret_cast<const uint32_t *>(spvBinary.data()),
        spvBinary.size() / sizeof(uint32_t));
    SmallVector<iree_hal_spirv_ShaderModuleDef_ref_t> shaderModuleRefs;
    shaderModuleRefs.push_back(
        iree_hal_spirv_ShaderModuleDef_create(builder, spvCodeRef));

    // Add top-level executable fields following their order of definition.
    auto entryPointsRef = builder.createStringVec(entryPointNames);
    auto shaderModuleIndicesRef = builder.createInt32Vec(shaderModuleIndices);
    iree_hal_spirv_ExecutableDef_entry_points_add(builder, entryPointsRef);
    iree_hal_spirv_ExecutableDef_shader_module_indices_add(
        builder, shaderModuleIndicesRef);
    auto shaderModulesRef =
        builder.createOffsetVecDestructive(shaderModuleRefs);
    iree_hal_spirv_ExecutableDef_shader_modules_add(builder, shaderModulesRef);

    iree_hal_spirv_ExecutableDef_end_as_root(builder);

    // Add the binary data to the target executable.
    auto binaryOp = executableBuilder.create<IREE::HAL::ExecutableBinaryOp>(
        variantOp.getLoc(), variantOp.getSymName(),
        variantOp.getTarget().getFormat(),
        builder.getBufferAttr(executableBuilder.getContext()));
    binaryOp.setMimeTypeAttr(
        executableBuilder.getStringAttr("application/x-flatbuffers"));

    return success();
  }

private:
  const VulkanSPIRVTargetOptions &options_;
};

namespace {
struct VulkanSPIRVSession
    : public PluginSession<VulkanSPIRVSession, VulkanSPIRVTargetOptions,
                           PluginActivationPolicy::DefaultActivated> {
  void populateHALTargetDevices(IREE::HAL::TargetDeviceList &targets) {
    // #hal.device.target<"vulkan", ...
    targets.add("vulkan", [&]() {
      return std::make_shared<VulkanTargetDevice>(options);
    });
  }
  void populateHALTargetBackends(IREE::HAL::TargetBackendList &targets) {
    // #hal.executable.target<"vulkan-spirv", ...
    targets.add("vulkan-spirv", [&]() {
      return std::make_shared<VulkanSPIRVTargetBackend>(options);
    });
  }
};

} // namespace

} // namespace mlir::iree_compiler::IREE::HAL

extern "C" bool iree_register_compiler_plugin_hal_target_vulkan_spirv(
    mlir::iree_compiler::PluginRegistrar *registrar) {
  registrar->registerPlugin<mlir::iree_compiler::IREE::HAL::VulkanSPIRVSession>(
      "hal_target_vulkan_spirv");
  return true;
}

IREE_DEFINE_COMPILER_OPTION_FLAGS(
    mlir::iree_compiler::IREE::HAL::VulkanSPIRVTargetOptions);
