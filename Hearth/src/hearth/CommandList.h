#pragma once

#include "hearth/Resources.h"

namespace hearth {

    struct RenderPassDesc {
        // Null target renders into the swapchain image for this frame.
        RenderTarget* target = nullptr;
        LoadOp loadOp = LoadOp::Clear;
        Color clearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        f32 clearDepth = 1.0f;
    };

    class CommandList {
    public:
        virtual ~CommandList() = default;

        virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
        virtual void EndRenderPass() = 0;

        virtual void BindPipeline(const Ref<Pipeline>& pipeline) = 0;
        virtual void BindBindGroup(const Ref<BindGroup>& group) = 0;
        virtual void BindVertexBuffer(u32 slot, const Ref<Buffer>& buffer, u64 offset = 0) = 0;
        virtual void BindIndexBuffer(const Ref<Buffer>& buffer, IndexType type = IndexType::U32,
                                     u64 offset = 0) = 0;

        virtual void SetViewport(const Viewport& vp) = 0;
        virtual void SetScissor(const ScissorRect& rect) = 0;
        virtual void SetPushConstants(const void* data, u32 size) = 0;

        virtual void Draw(u32 vertexCount, u32 instanceCount = 1,
                          u32 firstVertex = 0, u32 firstInstance = 0) = 0;
        virtual void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0,
                                 i32 vertexOffset = 0, u32 firstInstance = 0) = 0;

        // Runs the bound compute pipeline over `groupsX * groupsY * groupsZ` workgroups.
        // Counts are WORKGROUPS, not invocations: a shader with local_size_x = 64 processing
        // 1000 items dispatches ceil(1000 / 64) groups, not 1000. Must not be inside a
        // render pass.
        virtual void Dispatch(u32 groupsX, u32 groupsY = 1, u32 groupsZ = 1) = 0;

        // Makes everything written before this point visible to everything recorded after it.
        // Deliberately conservative -- it names every access type rather than asking a caller
        // to work out which stages to pair, because getting that wrong produces a result that
        // is correct on the machine it was written on and garbage elsewhere. Between a compute
        // dispatch and the draw that reads its output, this is the call.
        virtual void MemoryBarrier() = 0;
    };

}
