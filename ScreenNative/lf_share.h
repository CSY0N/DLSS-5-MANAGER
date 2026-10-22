// lf_share.h - the Spout output, as the worker calls it.
#pragma once

#include <d3d11.h>
#include <d3d12.h>

bool ShareStart(ID3D12Device *dev);
bool ShareActive();

void ShareCopy(ID3D12GraphicsCommandList *list, ID3D12Resource *src,
                     UINT w, UINT h);

void ShareSend();

void ShareStop();
