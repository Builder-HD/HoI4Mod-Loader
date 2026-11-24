// globe (NOTES.md Part S): the shader constants' channel (globe_d3d.cpp).
#pragma once

namespace GlobeD3D {

// Queues the D3D11CreateDevice hook; init only. False if d3d11.dll has no such export.
bool Install();
// A DX11 device was seen, its constant buffer created and its context hooked.
bool Ready();
// The three float4 the vertex shaders read (Globe::Params). Any thread; uploaded on the next
// vertex shader the game sets.
void Publish(const float a[4], const float b[4], const float c[4]);

}  // namespace GlobeD3D
