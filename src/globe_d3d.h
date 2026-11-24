// globe (NOTES.md Part S): the shaders' channel (globe_d3d.cpp).
#pragma once

#include <string>

namespace GlobeD3D {

// Queues the D3D11CreateDevice hook; init only. backgroundPath: an image to load onto the device
// for the sky shader ("" for none). False if d3d11.dll has no such export.
bool Install(const std::wstring& backgroundPath);
// A DX11 device was seen, its constant buffer created and its context hooked.
bool Ready();
// The background image is on the device.
bool HaveBackground();
// The six float4 the shaders read (Globe::Params a..f, 24 floats). Any thread; uploaded on the next
// vertex shader the game sets.
void Publish(const float* data);

}  // namespace GlobeD3D
