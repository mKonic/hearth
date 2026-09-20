#version 450

// Vertex colour straight through, positions already in clip space. Enough to prove the
// whole path -- pipeline, vertex buffer, push constants, render target, readback -- without
// any of it depending on a shading model the test would then also have to assert.

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

layout(push_constant) uniform Push {
   vec2 offset;
   vec2 scale;
} push;

void main() {
   gl_Position = vec4(inPosition * push.scale + push.offset, 0.0, 1.0);
   vColor = inColor;
}
