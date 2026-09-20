#version 450

// Writes two different colours to two attachments, so a test can tell them apart and catch
// an attachment order or blend-state replication mistake.

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outA;
layout(location = 1) out vec4 outB;

void main() {
   outA = vColor;
   outB = vec4(vColor.b, vColor.g, vColor.r, vColor.a);   // channel-swapped
}
