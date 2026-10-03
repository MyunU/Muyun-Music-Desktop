// 手定义的 COM IID（MinGW 无 __uuidof，IID 从官方 WebView2.h 的 MIDL_INTERFACE 注释提取）
#pragma once
#include <guiddef.h>

// --- 系统自带 ---
#include <objbase.h>

// --- WebView2 核心 ---
// ICoreWebView2  {76eceacb-0462-4d94-ac83-423a6793775e}
// ICoreWebView2_2{9E8F0CF8-E670-4B5E-B2BC-73E061E3184C}
// ICoreWebView2_3{A0D6DF20-3B92-416D-AA0C-437A9C727857}
// ICoreWebView2Environment            {b96d755e-0319-4e92-a296-23436f46a1fc}
// ICoreWebView2Controller             {4d00c0d1-9434-4eb6-8078-8697a560334f}
// ICoreWebView2Controller2            {c979903e-d4ca-4228-92eb-47ee3fa96eab}
// ICoreWebView2Settings               {e562e4f0-d7fa-43ac-8d71-c05150499f00}
// ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {4e8a3389-c9d8-4bd2-b6b5-124fee6cc14d}
// ICoreWebView2CreateCoreWebView2ControllerCompletedHandler  {6c4819f3-c9b7-4260-8127-c9f5bde7f68c}
// ICoreWebView2NavigationCompletedEventHandler  {d33a35bf-1c49-4f98-93ab-006e0533fe1c}
// ICoreWebView2WebMessageReceivedEventHandler   {57213f19-00e6-49fa-8e07-898ea01ecbd2}
// ICoreWebView2WebResourceRequestedEventHandler {ab00b74c-15f1-4646-80e8-e76341d25d71}
// ICoreWebView2WebResourceRequest   {db8fd961-2632-4e5d-9c0d-32364f300f00}  (官方文档值)
// ICoreWebView2WebResourceResponse  {6c4ca2a3-95ba-4260-bc7c-67a884470e4d}  (官方文档值)
// ICoreWebView2WebResourceContext   (无独立 IID，枚举)

#ifndef MUYUN_GUID_H
#define MUYUN_GUID_H

namespace muyun {

// 注意：这些 GUID 必须在包含本头后、用 DEFINE_GUID 之前包含 <initguid.h>
// ——我们直接用 struct GUID 常量，避免 initguid 链接问题。
inline constexpr GUID IID_ICoreWebView2_L   = {0x76eceacb,0x0462,0x4d94,{0xac,0x83,0x42,0x3a,0x67,0x93,0x77,0x5e}};
inline constexpr GUID IID_ICWV2_2_L        = {0x9e8f0cf8,0xe670,0x4b5e,{0xb2,0xbc,0x73,0xe0,0x61,0xe3,0x18,0x4c}};
inline constexpr GUID IID_ICWV2_3_L        = {0xa0d6df20,0x3b92,0x416d,{0xaa,0x0c,0x43,0x7a,0x9c,0x72,0x78,0x57}};
inline constexpr GUID IID_Env_L            = {0xb96d755e,0x0319,0x4e92,{0xa2,0x96,0x23,0x43,0x6f,0x46,0xa1,0xfc}};
inline constexpr GUID IID_Ctrl_L           = {0x4d00c0d1,0x9434,0x4eb6,{0x80,0x78,0x86,0x97,0xa5,0x60,0x33,0x4f}};
inline constexpr GUID IID_Ctrl2_L          = {0xc979903e,0xd4ca,0x4228,{0x92,0xeb,0x47,0xee,0x3f,0xa9,0x6e,0xab}};
inline constexpr GUID IID_Settings_L       = {0xe562e4f0,0xd7fa,0x43ac,{0x8d,0x71,0xc0,0x51,0x50,0x49,0x9f,0x00}};
inline constexpr GUID IID_EnvHandler_L     = {0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};
inline constexpr GUID IID_CtrlHandler_L    = {0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};
inline constexpr GUID IID_NavDoneHandler_L = {0xd33a35bf,0x1c49,0x4f98,{0x93,0xab,0x00,0x6e,0x05,0x33,0xfe,0x1c}};
inline constexpr GUID IID_MsgHandler_L     = {0x57213f19,0x00e6,0x49fa,{0x8e,0x07,0x89,0x8e,0xa0,0x1e,0xcb,0xd2}};
inline constexpr GUID IID_ResReqHandler_L  = {0xab00b74c,0x15f1,0x4646,{0x80,0xe8,0xe7,0x63,0x41,0xd2,0x5d,0x71}};
inline constexpr GUID IID_WebResRequest_L  = {0x97055cd4,0x512c,0x4264,{0x8b,0x5f,0xe3,0xf4,0x46,0xce,0xa6,0xa5}};
inline constexpr GUID IID_WebResResponse_L = {0xaafcc94f,0xfa27,0x48fd,{0x97,0xdf,0x83,0x0e,0xf7,0x5a,0xae,0xc9}};
inline constexpr GUID IID_ExScriptDone_L   = {0x49511172,0xcc67,0x4bca,{0x99,0x23,0x13,0x71,0x12,0xf4,0xc4,0xcc}};
inline constexpr GUID IID_Zero_L           = {0,0,0,{0,0,0,0,0,0,0,0}};

} // namespace muyun
#endif
