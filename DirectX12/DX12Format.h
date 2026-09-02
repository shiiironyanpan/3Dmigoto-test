#pragma once
#include <windows.h>
#include <dxgiformat.h>
const char* DX12FormatName(DXGI_FORMAT f);
DXGI_FORMAT DX12ParseFormat(const wchar_t* text);
UINT DX12FormatSize(DXGI_FORMAT f);
