#include "engine/rhi/UniqueHandle.hpp"

#include "engine/rhi/Device.hpp"

namespace rhi::detail {

void destroyHandle(Device& device, BufferHandle handle) {
    device.destroyBuffer(handle);
}
void destroyHandle(Device& device, TextureHandle handle) {
    device.destroyTexture(handle);
}
void destroyHandle(Device& device, SamplerHandle handle) {
    device.destroySampler(handle);
}
void destroyHandle(Device& device, ShaderHandle handle) {
    device.destroyShader(handle);
}
void destroyHandle(Device& device, PipelineHandle handle) {
    device.destroyPipeline(handle);
}
void destroyHandle(Device& device, BindGroupHandle handle) {
    device.destroyBindGroup(handle);
}
void destroyHandle(Device& device, FramebufferHandle handle) {
    device.destroyFramebuffer(handle);
}

} // namespace rhi::detail
