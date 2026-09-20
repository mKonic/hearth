#version 450

// `discard` makes glslc emit the DemoteToHelperInvocation capability, which fails module
// creation unless the device feature is enabled. This test exists because a real consumer
// hit exactly that and the pixel assertions could not see it.

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
   if (vColor.a < 0.5) discard;
   outColor = vColor;
}
