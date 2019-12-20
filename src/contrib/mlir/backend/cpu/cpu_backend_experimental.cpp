//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************
//
// This file contains code that is temporarily needed to overcome existing limitations in MLIR.
//
// NOTE: This file follows nGraph format style.
// Follows nGraph naming convention for public APIs only, else MLIR naming convention.

#include "cpu_backend_experimental.hpp"

#include <llvm/Support/CommandLine.h>
#include <mlir/Conversion/StandardToLLVM/ConvertStandardToLLVMPass.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/IR/Function.h>
#include <mlir/IR/StandardTypes.h>

// *** Experimental flags ***

llvm::cl::opt<bool>
    clEnableCustomMemRefLowering("ngraph-custom-memref-lowering",
                                 llvm::cl::init(false),
                                 llvm::cl::desc("Enable a custom memref lowering to LLVM"));

using namespace ngraph::runtime::ngmlir;
using namespace mlir;

namespace
{
    struct CustomFuncOpConversion : public LLVMOpLowering
    {
        CustomFuncOpConversion(LLVMTypeConverter& converter, PatternBenefit benefit)
            : LLVMOpLowering(FuncOp::getOperationName(),
                             converter.getDialect()->getContext(),
                             converter,
                             benefit)
        {
        }

        PatternMatchResult matchAndRewrite(Operation* op,
                                           ArrayRef<Value*> operands,
                                           ConversionPatternRewriter& rewriter) const override
        {
            auto funcOp = cast<FuncOp>(op);

            // Convert the original function arguments. Struct arguments are promoted to
            // pointer to struct arguments to allow calling external functions with
            // various ABIs (e.g. compiled from C/C++ on platform X).
            auto varargsAttr = funcOp.getAttrOfType<BoolAttr>("std.varargs");
            TypeConverter::SignatureConversion result(funcOp.getNumArguments());
            auto llvmType = lowering.convertFunctionSignature(
                funcOp.getType(), varargsAttr && varargsAttr.getValue(), result);

            // Only retain those attributes that are not constructed by build.
            SmallVector<NamedAttribute, 4> attributes;
            for (const auto& attr : funcOp.getAttrs())
            {
                if (attr.first.is(SymbolTable::getSymbolAttrName()) ||
                    attr.first.is(impl::getTypeAttrName()) || attr.first.is("std.varargs"))
                    continue;
                attributes.push_back(attr);
            }

            // Create an LLVM function, use external linkage by default until MLIR
            // functions have linkage.
            auto newFuncOp = rewriter.create<LLVM::LLVMFuncOp>(
                op->getLoc(), funcOp.getName(), llvmType, LLVM::Linkage::External, attributes);
            rewriter.inlineRegionBefore(funcOp.getBody(), newFuncOp.getBody(), newFuncOp.end());

            // Tell the rewriter to convert the region signature.
            rewriter.applySignatureConversion(&newFuncOp.getBody(), result);

            rewriter.eraseOp(op);
            return matchSuccess();
        }
    };

    class CustomMemRefDescriptor : public MemRefDescriptor
    {
    public:
        /// Construct a helper for the given descriptor value.
        explicit CustomMemRefDescriptor(Value* descriptor)
            : value(descriptor){};

        Value* getValue() override { return value; }

        /// Builds IR extracting the allocated pointer from the descriptor.
        Value* allocatedPtr(OpBuilder& builder, Location loc) override { return value; };
        /// Builds IR inserting the allocated pointer into the descriptor.
        void setAllocatedPtr(OpBuilder& builder, Location loc, Value* ptr) override
        {
            value = ptr;
        };

        /// Builds IR extracting the aligned pointer from the descriptor.
        Value* alignedPtr(OpBuilder& builder, Location loc) override { return value; };

        /// Builds IR inserting the aligned pointer into the descriptor.
        void setAlignedPtr(OpBuilder& builder, Location loc, Value* ptr) override{};

        /// Builds IR extracting the offset from the descriptor.
        Value* offset(OpBuilder& builder, Location loc) override { return nullptr; };

        /// Builds IR inserting the offset into the descriptor.
        void setOffset(OpBuilder& builder, Location loc, Value* offset) override{};

        void setConstantOffset(OpBuilder& builder, Location loc, uint64_t offset) override{};

        /// Builds IR extracting the pos-th size from the descriptor.
        Value* size(OpBuilder& builder, Location loc, unsigned pos) override { return nullptr; };

        /// Builds IR inserting the pos-th size into the descriptor
        void setSize(OpBuilder& builder, Location loc, unsigned pos, Value* size) override{};
        void setConstantSize(OpBuilder& builder,
                             Location loc,
                             unsigned pos,
                             uint64_t size) override{};

        /// Builds IR extracting the pos-th size from the descriptor.
        Value* stride(OpBuilder& builder, Location loc, unsigned pos) override { return nullptr; };

        /// Builds IR inserting the pos-th stride into the descriptor
        void setStride(OpBuilder& builder, Location loc, unsigned pos, Value* stride) override{};
        void setConstantStride(OpBuilder& builder,
                               Location loc,
                               unsigned pos,
                               uint64_t stride) override{};

        /// Returns the (LLVM) type this descriptor points to.
        LLVM::LLVMType getElementType() override { return value->getType().cast<LLVM::LLVMType>(); }

    private:
        Value* value;
    };

} // namespace

/// Custom Std-to-LLVM type converter that overrides `convertType` and `convertFunctionSignature`
/// taking into account that MemRef type will be lowered to a plain pointer. It falls back to the
/// standar LLVMTypeConverter for the remaining types.
Type CustomLLVMTypeConverter::convertType(Type type)
{
    if (auto memrefTy = type.dyn_cast<MemRefType>())
    {
        return convertMemRefType(memrefTy);
    }

    // Fall back to the base class.
    return LLVMTypeConverter::convertType(type);
}

/// Converts function signature following LLVMTypeConverter approach but lowering
/// MemRef arguments to plain LLVM pointers to element type.
LLVM::LLVMType CustomLLVMTypeConverter::convertFunctionSignature(
    FunctionType type, bool isVariadic, LLVMTypeConverter::SignatureConversion& result)
{
    // Convert argument types one by one and check for errors.
    for (auto& en : llvm::enumerate(type.getInputs()))
    {
        Type type = en.value();
        auto converted = convertType(type).dyn_cast_or_null<LLVM::LLVMType>();
        if (!converted)
            return {};
        result.addInputs(en.index(), converted);
    }

    SmallVector<LLVM::LLVMType, 8> argTypes;
    argTypes.reserve(llvm::size(result.getConvertedTypes()));
    for (Type type : result.getConvertedTypes())
        argTypes.push_back(unwrap(type));

    // If function does not return anything, create the void result type,
    // if it returns on element, convert it, otherwise pack the result types into
    // a struct.
    LLVM::LLVMType resultType = type.getNumResults() == 0
                                    ? LLVM::LLVMType::getVoidTy(llvmDialect)
                                    : unwrap(packFunctionResults(type.getResults()));
    if (!resultType)
        return {};
    return LLVM::LLVMType::getFunctionTy(resultType, argTypes, isVariadic);
}

/// Converts MemRefType to plain LLVM pointer to element type.
Type CustomLLVMTypeConverter::convertMemRefType(MemRefType type)
{
    int64_t offset;
    SmallVector<int64_t, 4> strides;
    bool strideSuccess = succeeded(getStridesAndOffset(type, strides, offset));
    assert(strideSuccess && "Non-strided layout maps must have been normalized away");
    (void)strideSuccess;

    LLVM::LLVMType elementType = unwrap(convertType(type.getElementType()));
    if (!elementType)
        return {};
    auto ptrTy = elementType.getPointerTo(type.getMemorySpace());
    return ptrTy;
}

/// Create a CustomMemRefDescriptor object for 'value'.
std::unique_ptr<MemRefDescriptor> CustomLLVMTypeConverter::createMemRefDescriptor(Value* value)
{
    return std::make_unique<CustomMemRefDescriptor>(value);
}

/// Create a CustomMemRefDescriptor object for an uninitialized descriptor (nullptr value). No new
/// IR is needed for such initialization.
std::unique_ptr<MemRefDescriptor> CustomLLVMTypeConverter::buildMemRefDescriptor(
    OpBuilder& builder, Location loc, Type descriptorType)
{
    return createMemRefDescriptor(nullptr);
}

/// Builds IR creating a MemRef descriptor that represents `type` and populates it with static shape
/// and stride information extracted from the type.
std::unique_ptr<MemRefDescriptor> CustomLLVMTypeConverter::buildStaticMemRefDescriptor(
    OpBuilder& builder, Location loc, MemRefType type, Value* memory)
{
    assert(type.hasStaticShape() && "unexpected dynamic shape");
    assert(type.getAffineMaps().empty() && "unexpected layout map");

    auto convertedType = convertType(type);
    assert(convertedType && "unexpected failure in memref type conversion");

    auto descr = buildMemRefDescriptor(builder, loc, convertedType);
    descr->setAllocatedPtr(builder, loc, memory);
    return descr;
}

/// Populates 'patterns' with default LLVM conversion patterns using CustomLLVMTypeConverter and a
/// custom conversion pattern for FuncOp which takes into account MemRef custom lowering to plain
/// LLVM pointer.
void ngraph::runtime::ngmlir::customPopulateStdToLLVMConversionPatterns(
    LLVMTypeConverter& converter, OwningRewritePatternList& patterns)
{
    mlir::populateStdToLLVMConversionPatterns(converter, patterns);
    // Add custom FuncOp conversion pattern with higher benefit than default FuncOp conversion
    // pattern (1).
    patterns.insert<CustomFuncOpConversion>(converter, /*benefit=*/100);
}
