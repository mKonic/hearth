#pragma once

#include "hearth/Types.h"

namespace hearth {

    // Resource interfaces are virtual at *resource* granularity, never per draw. A frame is a
    // handful of these objects and some draws, so the dispatch cost is noise; putting the seam any
    // lower -- a virtual call per quad -- would make it the hot path.

    struct BufferDesc {
        u64 size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        MemoryKind memory = MemoryKind::HostVisible;
        std::string debugName;
    };

    class Buffer {
    public:
        virtual ~Buffer() = default;
        virtual void Upload(const void* data, u64 size, u64 offset = 0) = 0;
        // HostVisible only, and persistently mapped: a batcher writes straight into this every
        // frame rather than paying a map/unmap pair per flush. Null on a DeviceLocal buffer.
        virtual void* Map() = 0;
        virtual u64  Size() const = 0;
        virtual const BufferDesc& Desc() const = 0;
    };

    struct TextureDesc {
        u32 width = 1, height = 1;
        Format format = Format::RGBA8_UNORM;
        TextureUsage usage = TextureUsage::Sampled;
        Filter minFilter = Filter::Linear, magFilter = Filter::Linear;
        AddressMode addressMode = AddressMode::ClampToEdge;
        std::string debugName;
    };

    class Texture {
    public:
        virtual ~Texture() = default;
        virtual void Upload(const void* pixels, u64 size) = 0;
        virtual void UploadRegion(const void* pixels, u32 x, u32 y, u32 w, u32 h) = 0;
        virtual u32 Width()  const = 0;
        virtual u32 Height() const = 0;
        virtual const TextureDesc& Desc() const = 0;

        f32 AspectRatio() const {
            return Height() ? static_cast<f32>(Width()) / static_cast<f32>(Height()) : 1.0f;
        }
    };

    struct ShaderDesc {
        std::vector<u32> spirv;        // SPIR-V words, as glslc emits them
        ShaderStage stage = ShaderStage::Vertex;
        std::string entryPoint = "main";
        std::string debugName;
    };

    class Shader {
    public:
        virtual ~Shader() = default;
        virtual ShaderStage Stage() const = 0;
    };

    struct PipelineDesc {
        Ref<Shader> vertex;
        Ref<Shader> fragment;
        std::vector<VertexBufferLayout> vertexBuffers;
        std::vector<BindingSlot> bindings;
        u32 pushConstantSize = 0;
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        BlendMode blend = BlendMode::AlphaStraight;
        bool depthTest = false;
        bool depthWrite = false;
        // Must match the target this pipeline renders into. With dynamic rendering there is no
        // VkRenderPass to check it against, so a mismatch is a validation error at draw time
        // rather than at creation -- pass Device::SwapchainFormat() unless you mean otherwise.
        Format colorFormat = Format::BGRA8_UNORM;
        Format depthFormat = Format::Undefined;
        std::string debugName;
    };

    // A compute pipeline is one shader and its bindings. It shares Pipeline with the
    // graphics kind because everything downstream -- bind groups, push constants, the
    // descriptor pool -- treats them identically; only the bind point and the dispatch
    // differ, and CommandList works that out from IsCompute().
    struct ComputePipelineDesc {
        Ref<Shader> compute;
        std::vector<BindingSlot> bindings;
        u32 pushConstantSize = 0;
        std::string debugName;
    };

    class Pipeline {
    public:
        virtual ~Pipeline() = default;

        virtual bool IsCompute() const = 0;
        virtual const std::vector<BindingSlot>& Bindings() const = 0;
        virtual const std::string& DebugName() const = 0;

        // Graphics only. Asserts on a compute pipeline, which has no vertex layout, no blend
        // state and no attachment formats to report.
        virtual const PipelineDesc& Desc() const = 0;
    };

    // A concrete set of resources bound to a pipeline's binding slots.
    struct BindGroupEntry {
        u32 binding = 0;
        BindingType type = BindingType::UniformBuffer;
        Ref<Buffer> buffer;
        u64 bufferOffset = 0, bufferRange = 0;     // range 0 = the whole buffer
        // StorageTexture binds exactly one, and it must have been created with
        // TextureUsage::Storage.
        // More than one for an array binding. A slot declared with count N that receives fewer
        // than N textures has the remainder filled with the first entry, so a partially populated
        // sampler array still validates -- reading an unwritten descriptor is undefined behaviour
        // even in the arms of the shader that never execute.
        std::vector<Ref<Texture>> textures;
    };

    class BindGroup {
    public:
        virtual ~BindGroup() = default;
        // Rewrites the descriptors in place. A batcher that discovers a new texture mid-frame
        // updates its array this way instead of allocating a second set per frame.
        virtual void Update(const std::vector<BindGroupEntry>& entries) = 0;
    };

    // An offscreen target to draw into and later sample: an editor viewport, a layer effect, a
    // frame grab.
    class RenderTarget {
    public:
        virtual ~RenderTarget() = default;
        virtual u32 Width()  const = 0;
        virtual u32 Height() const = 0;
        virtual Ref<Texture> ColorTexture() const = 0;
        virtual void Resize(u32 width, u32 height) = 0;

        // Copies the colour attachment back to CPU memory, tightly packed, in the target's own
        // colorFormat -- so a caller that wants RGBA asks for an RGBA target and does no
        // channel swizzling of its own. Blocking, and not a per-frame path: this waits for the
        // GPU. `size` must be width * height * FormatSize(colorFormat).
        virtual void ReadPixels(void* outPixels, u64 size) = 0;
    };

    struct RenderTargetDesc {
        u32 width = 1, height = 1;
        Format colorFormat = Format::RGBA8_UNORM;
        Format depthFormat = Format::Undefined;
        std::string debugName;
    };

}
