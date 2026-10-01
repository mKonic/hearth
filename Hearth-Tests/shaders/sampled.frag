#version 450

// One texture, no arrays: what every device can run, descriptor indexing or not.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

void main() {
   outColor = texture(uTexture, vUV);
}
