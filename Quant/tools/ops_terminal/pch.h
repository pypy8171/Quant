#pragma once
// 운영단말 공용 헤더. winsock2.h를 afx보다 먼저 넣어야 windows.h의 winsock.h와 충돌하지 않는다.
#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>

#include <afxwin.h>
#include <afxcmn.h>
#include <afxdialogex.h>
#include <afxext.h>

#include <string>
#include <vector>
