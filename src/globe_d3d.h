// globe (NOTES.md Parts S and T): the shaders' channel (globe_d3d.cpp).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace GlobeD3D {

// Queues the D3D11CreateDevice hook; init only. backgroundPath: an image to load onto the device
// for the sky shader ("" for none); imagePaths: per planet, its overview image ("" for none). False if
// d3d11.dll has no such export.
bool Install(const std::wstring& backgroundPath, const std::vector<std::wstring>& imagePaths);
// A DX11 device was seen, its constant buffer created and its context hooked.
bool Ready();
// The background image is on the device.
bool HaveBackground();
// Planet i's image cell (u0, v0, du, dv) in the images texture; false if it has none on the device.
bool ImageCell(size_t i, float cell[4]);
// The planet names texture (RGBA, w x h): made on the device and bound from the next draw. Any thread.
bool SetNames(const uint8_t* rgba, int w, int h);
// The constants the shaders read (a whole Globe::Params). Any thread; uploaded on the next vertex
// shader the game sets.
void Publish(const float* data);

}  // namespace GlobeD3D
