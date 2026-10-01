#version 450

// flat.vert with the depth taken from a push constant, so a test can place a primitive at a
// known depth and assert on the depth test, the compare op and the bias.

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

layout(push_constant) uniform Push {
   vec2 offset;
   vec2 scale;
   float depth;
} push;

void main() {
   gl_Position = vec4(inPosition * push.scale + push.offset, push.depth, 1.0);
   vColor = inColor;
}
