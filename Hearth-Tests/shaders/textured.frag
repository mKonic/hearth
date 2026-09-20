#version 450
#extension GL_EXT_nonuniform_qualifier : require

// A four-slot sampler array indexed by a push constant, which is the shape a sprite batcher
// uses. The test drives it to prove array bindings and the partial-fill padding work.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTextures[4];

layout(push_constant) uniform Push {
   uint slot;
} push;

void main() {
   outColor = texture(uTextures[nonuniformEXT(push.slot)], vUV);
}
