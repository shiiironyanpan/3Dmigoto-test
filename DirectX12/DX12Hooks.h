#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

void DX12InstallDeviceHooks(ID3D12Device* device);
void DX12InstallQueueHooks(ID3D12CommandQueue* queue);
void DX12InstallCommandListHooks(ID3D12GraphicsCommandList* list);
void DX12InstallFactoryHooks(IUnknown* factory);
void DX12InstallSwapChainHooks(IDXGISwapChain* swapChain);
void DX12InstallRuntimeEntryHooks();

extern thread_local bool g_dx12Internal;
