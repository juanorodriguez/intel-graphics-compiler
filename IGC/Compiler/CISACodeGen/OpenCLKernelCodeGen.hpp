/*========================== begin_copyright_notice ============================

Copyright (c) 2000-2021 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

============================= end_copyright_notice ===========================*/

#pragma once
#include "Compiler/CISACodeGen/ComputeShaderBase.hpp"

namespace IGC
{
    class KernelArg;
}

namespace IGC
{

    class COpenCLKernel : public CComputeShaderBase
    {
    public:
        friend class CShaderProgram;
        COpenCLKernel(const OpenCLProgramContext* ctx, llvm::Function*, CShaderProgram* pProgram);
        ~COpenCLKernel();

        void PreCompile() override;
        void AllocatePayload() override;
        void ParseShaderSpecificOpcode(llvm::Instruction* inst) override;
        void ExtractGlobalVariables() override {}

        bool        hasReadWriteImage(llvm::Function& F) override;
        bool        CompileSIMDSize(SIMDMode simdMode, EmitPass& EP, llvm::Function& F) override;
        SIMDStatus  checkSIMDCompileConds(SIMDMode simdMode, EmitPass& EP, llvm::Function& F);

        void        FillKernel();

        // Recomputes the binding table layout according to the present kernel args
        void RecomputeBTLayout();

        // Set m_HasTID to true if TID functions were found
        void SetHasTID();

        // Set m_HasGlobalSize to true if TID functions were found
        void SetHasGlobalSize();

        bool HasFullDispatchMask() override;

        // Returns the immediate value mapped to GlobalVariable c.
        // (GlobalVariables represent the pointer to the global,
        // which is a compile-time constant)
        unsigned int GetGlobalMappingValue(llvm::Value* c) override;
        CVariable* GetGlobalMapping(llvm::Value* c) override;

        const SOpenCLKernelInfo& getKernelInfo() const { return m_kernelInfo; }

    public:
        SOpenCLProgramInfo* m_programInfo;
        SOpenCLKernelInfo m_kernelInfo;

        unsigned int m_perWIStatelessPrivateMemSize;

        bool        GetDisableMidThreadPreemption() const { return m_disableMidThreadPreemption; }
        void        SetDisableMidthreadPreemption() { m_disableMidThreadPreemption = true; }

    protected:
        // Creates appropriate annotation based on the kernel arg
        void CreateAnnotations(IGC::KernelArg* kernelArg, uint payloadPosition);

        // Fill SOpenCLKernelInfo::m_zePayloadArgs
        // Return true: if the argument is supported in ZEBinary and it's created successfully
        // Return false: if the argument cannot be supported by ZEBinary
        bool CreateZEPayloadArguments(IGC::KernelArg* kernelArg, uint payloadPosition);

        // a helper function to get image type from kernelArg
        iOpenCL::IMAGE_MEMORY_OBJECT_TYPE getImageTypeFromKernelArg(const KernelArg& kernelArg);

        // Creates annotations for inline sampler_t objects
        void CreateInlineSamplerAnnotations();

        // Creates annotations for kernel argument information (kernel reflection)
        void CreateKernelArgInfo();

        // Creates annotations for kernel attribution information (kernel reflection)
        void CreateKernelAttributeInfo();
        std::string getVecTypeHintString(IGC::IGCMD::VectorTypeHintMetaDataHandle& vecTypeHintInfo);
        std::string getThreadGroupSizeString(IGC::IGCMD::ThreadGroupSizeMetaDataHandle& threadGroupSize, bool isHint);
        std::string getSubGroupSizeString(IGC::IGCMD::SubGroupSizeMetaDataHandle& subGroupSize);
        std::string getWorkgroupWalkOrderString(const IGC::WorkGroupWalkOrderMD& workgroupWalkOrder);
        // Create annotation for printf strings.
        void CreatePrintfStringAnnotations();

        // Load from MD and return the resource information for argument number argNo
        SOpenCLKernelInfo::SResourceInfo getResourceInfo(int argNo);

        // Load from MD and return the resource extension information for argument number argNo
        ResourceExtensionTypeEnum getExtensionInfo(int argNo);

        // Resolve the binding table index for resource resInfo (using the BTL)
        unsigned int getBTI(SOpenCLKernelInfo::SResourceInfo& resInfo);

        // Find the sum of inline local sizes used by this kernel
        unsigned int getSumFixedTGSMSizes(llvm::Function* F);

        bool m_HasTID;
        bool m_HasGlobalSize;
        bool m_disableMidThreadPreemption;

        // Maps GlobalVariables representing local address-space pointers
        // to their offsets in SLM.
        std::map<llvm::Value*, unsigned int> m_localOffsetsMap;

        OpenCLProgramContext* m_Context;

        void ClearKernelInfo();
    private:
        bool hasWorkGroupWalkOrder();
    };

}
