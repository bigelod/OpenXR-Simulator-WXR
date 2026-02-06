#include "WinXrApiUDP.h"

// Minimal OpenXR WXR Runtime (D3D11/D3D12/OpenGL)
// - Implements enough of the runtime interface to let OpenXR apps start and render into runtime-owned swapchains
// - Opens a desktop window and presents the app's submitted images side-by-side
// - Supports D3D11, D3D12, and OpenGL graphics APIs

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_OPENGL

#include <windows.h>
#include <WinUser.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>

//----------------
//OXRWXR CHANGE:
//---------------- 
// New includes
#include <Winsock2.h>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <ws2tcpip.h>
// New defines
#define D3DX12_COLOR_F(r, g, b, a) { (FLOAT)(r), (FLOAT)(g), (FLOAT)(b), (FLOAT)(a) }
#define D3D11_COLOR_ARGB(a, r, g, b) ((UINT)(((a) << 24) | ((r) << 16) | ((g) << 8) | (b)))


// OpenGL headers - minimal definitions for what we need
#include <GL/gl.h>

// OpenGL extension constants and types (from glext.h / wglext.h)
// We define these inline since Windows doesn't ship with glext.h
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8                   0x8C43
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                          0x8058
#endif
#ifndef GL_BGRA
#define GL_BGRA                           0x80E1
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F                        0x881A
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F                        0x8814
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2                       0x8059
#endif
#ifndef GL_DEPTH_COMPONENT32F
#define GL_DEPTH_COMPONENT32F             0x8CAC
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8               0x88F0
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16              0x81A5
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL                  0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8              0x84FA
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY               0x8C1A
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                    0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0              0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#endif

// Function pointer types for GL extension functions
typedef void (APIENTRY* PFNGLTEXIMAGE3DPROC)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels);
typedef void (APIENTRY* PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint* framebuffers);
typedef void (APIENTRY* PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint* framebuffers);
typedef void (APIENTRY* PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void (APIENTRY* PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum(APIENTRY* PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum target);
typedef void (APIENTRY* PFNGLREADPIXELSPROC)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);

// GL function pointers (loaded at runtime)
static PFNGLTEXIMAGE3DPROC g_glTexImage3D = nullptr;
static PFNGLGENFRAMEBUFFERSPROC g_glGenFramebuffers = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC g_glDeleteFramebuffers = nullptr;
static PFNGLBINDFRAMEBUFFERPROC g_glBindFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC g_glFramebufferTexture2D = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC g_glCheckFramebufferStatus = nullptr;
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <chrono>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <loader_interfaces.h>
#include "mcp_integration.h"
#include "ui_enhancements.h"

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "opengl32.lib")

// D3D12 helper - calculates subresource index (from d3dx12.h)
inline UINT D3D12CalcSubresource(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) {
	return MipSlice + ArraySlice * MipLevels + PlaneSlice * MipLevels * ArraySize;
}


// Simple logging (debug output + file log)
static FILE* g_LogFile = nullptr;
static void EnsureLogFile() {
	if (g_LogFile) return;
	char base[MAX_PATH]{};
	DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", base, (DWORD)sizeof(base));
	char path[MAX_PATH]{};
	if (len > 0 && len < sizeof(base)) {
		snprintf(path, sizeof(path), "%s\\OxrWXR-Simulator", base);
		CreateDirectoryA(path, nullptr);
		snprintf(path, sizeof(path), "%s\\OxrWXR-Simulator\\openxr_wxr.log", base);
	}
	else {
		snprintf(path, sizeof(path), ".\\openxr_wxr.log");
	}
	fopen_s(&g_LogFile, path, "a");
}
static void Log(const char* msg) {
	OutputDebugStringA(msg);
	EnsureLogFile();
	if (g_LogFile) { fputs(msg, g_LogFile); if (msg[0] && msg[strlen(msg) - 1] != '\n') fputc('\n', g_LogFile); fflush(g_LogFile); }
}
static void Log(const std::string& msg) { Log(msg.c_str()); }
static void Logf(const char* fmt, ...) {
	char buf[2048];
	va_list ap; va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Log(buf);
}

// Helper to load glTexImage3D (OpenGL 1.2+ function not in Windows GL headers)
static bool EnsureGLTexImage3D() {
	if (g_glTexImage3D) return true;
	g_glTexImage3D = (PFNGLTEXIMAGE3DPROC)wglGetProcAddress("glTexImage3D");
	if (!g_glTexImage3D) {
		Log("[OXRWXR] Failed to load glTexImage3D");
		return false;
	}
	return true;
}

static bool EnsureGLFramebufferFuncs() {
	if (g_glGenFramebuffers) return true;
	g_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)wglGetProcAddress("glGenFramebuffers");
	g_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)wglGetProcAddress("glDeleteFramebuffers");
	g_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
	g_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)wglGetProcAddress("glFramebufferTexture2D");
	g_glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)wglGetProcAddress("glCheckFramebufferStatus");
	if (!g_glGenFramebuffers || !g_glDeleteFramebuffers || !g_glBindFramebuffer ||
		!g_glFramebufferTexture2D || !g_glCheckFramebufferStatus) {
		Log("[OXRWXR] Failed to load GL framebuffer functions");
		return false;
	}
	return true;
}

static XrQuaternionf QuatFromYawPitch(float yaw, float pitch) {
	const float cy = cosf(yaw * 0.5f);
	const float sy = sinf(yaw * 0.5f);
	const float cp = cosf(pitch * 0.5f);
	const float sp = sinf(pitch * 0.5f);
	XrQuaternionf q{};
	q.x = sp * cy;
	q.y = cp * sy;
	q.z = -sp * sy;
	q.w = cp * cy;
	return q;
}

static XrQuaternionf QuatFromYawPitchRoll(float yaw, float pitch, float roll) {
	// Create rotations
	float cy = cosf(yaw * 0.5f);
	float sy = sinf(yaw * 0.5f);
	float cp = cosf(pitch * 0.5f);
	float sp = sinf(pitch * 0.5f);
	float cr = cosf(roll * 0.5f);
	float sr = sinf(roll * 0.5f);

	// Combine rotations (yaw * pitch * roll)
	XrQuaternionf q;
	q.w = cy * cp * cr + sy * sp * sr;
	q.x = cy * sp * cr + sy * cp * sr;
	q.y = sy * cp * cr - cy * sp * sr;
	q.z = cy * cp * sr - sy * sp * cr;
	return q;
}


// Helper function to convert a typed format to typeless
static DXGI_FORMAT ToTypeless(DXGI_FORMAT format) {
	switch (format) {
		// R8G8B8A8 family
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
		return DXGI_FORMAT_R8G8B8A8_TYPELESS;

		// B8G8R8A8 family
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_TYPELESS;

		// R16G16B16A16 family
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
		return DXGI_FORMAT_R16G16B16A16_TYPELESS;

		// R32G32B32A32 family
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return DXGI_FORMAT_R32G32B32A32_TYPELESS;

		// R10G10B10A2 family
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
		return DXGI_FORMAT_R10G10B10A2_TYPELESS;

		// Already typeless or depth formats - return as-is
	default:
		return format;
	}
}



//----------------
//OXRWXR CHANGE:
//---------------- 
// Create global variables
static WinXrApiUDP* udpReader;

static std::string hmdMake;
static std::string hmdModel;

static XrVector3f prevLHandPos[5];
static XrVector3f prevRHandPos[5];
static int lastPosFrame = 0;
static int maxPosBuffer = 1;

static float fovVarA = 1.0f;
static float fovVarB = 0.0f;
static float fovVarC = 1.0f;
static float fovVarD = 0.0f;
static float fovVarE = 104.5f;
static float fovVarF = 104.5f;

static float IPDVal;
static float FOVH;
static float FOVV;
static float FOVTotal = 1.0472f;

static bool LTrigger = false;
static bool LGrip = false;
static bool LClick = false;
static bool RTrigger = false;
static bool RGrip = false;
static bool RClick = false;
static bool L_X = false;
static bool L_Y = false;
static bool R_A = false;
static bool R_A_Once = false;
static bool R_B = false;
static bool R_B_Once = false;
static bool L_Menu = false;
static bool R_ThumbDown = false;
static bool R_ThumbUp = false;
static bool UpsideDownHandsFix = false;

static XrVector3f HMDPos;
static XrVector3f LHandPos;
static XrVector3f RHandPos;
static XrVector4f HMDQuat;
static XrVector4f LHandQuat;
static XrVector4f RHandQuat;
static XrVector2f LThumbstick;
static XrVector2f RThumbstick;

static int OpenXRFrameID = 0;
static int OpenXRFrameWait = 0;

static XrVector2f makeXrVector2f(float x, float y) {
	XrVector2f vec;
	vec.x = x;
	vec.y = y;
	return vec;
}

static XrVector3f makeXrVector3f(float x, float y, float z) {
	XrVector3f vec;
	vec.x = x;
	vec.y = y;
	vec.z = z;
	return vec;
}

static XrVector4f makeXrVector4f(float x, float y, float z, float w) {
	XrVector4f quat;
	quat.x = x;
	quat.y = y;
	quat.z = z;
	quat.w = w;
	return quat;
}

static XrVector4f QuaternionMultiply(const XrVector4f& q1, const XrVector4f& q2) {
	return XrVector4f{
		q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y,
		q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
		q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w,
		q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z };
}

// Runtime state
namespace rt {

	struct Swapchain;

	// Forward declarations
	void PushState(XrSession s, XrSessionState newState);

	// Global adapter LUID that we'll use consistently
	static LUID g_adapterLuid = {};
	static bool g_adapterLuidSet = false;

	// Global persistent window that survives session creation/destruction
	static HWND g_persistentWindow = nullptr;
	static std::mutex g_windowMutex;
	static ComPtr<IDXGISwapChain1> g_persistentSwapchain;
	static UINT g_persistentWidth = 1920; // TODO: Adjust to fit screen always?
	static UINT g_persistentHeight = 540;
	static bool g_windowClassRegistered = false;

	struct Instance {
		XrInstance handle{ (XrInstance)1 };
		std::vector<std::string> enabledExtensions;
	};

	struct Session {
		XrSession handle{ (XrSession)1 };
		XrSessionState state{ XR_SESSION_STATE_IDLE };
		ComPtr<ID3D11Device> d3d11Device;
		ComPtr<ID3D11DeviceContext> d3d11Context;
		// DX12 support
		ComPtr<ID3D12Device> d3d12Device;
		ComPtr<ID3D12CommandQueue> d3d12Queue;
		bool usesD3D12{ false };
		// OpenGL support
		HDC glDC{ nullptr };
		HGLRC glRC{ nullptr };
		bool usesOpenGL{ false };

		// DX12 preview resources
		ComPtr<IDXGISwapChain3> previewSwapchain12;
		ComPtr<ID3D12DescriptorHeap> previewRTVHeap;
		std::vector<ComPtr<ID3D12Resource>> previewBackbuffers;
		UINT previewRTVDescriptorSize{ 0 };
		UINT previewBackbufferCount{ 0 };
		ComPtr<ID3D12CommandAllocator> previewCmdAlloc;
		ComPtr<ID3D12GraphicsCommandList> previewCmdList;
		ComPtr<ID3D12Fence> previewFence;
		HANDLE previewFenceEvent{ nullptr };
		UINT64 previewFenceValue{ 0 };

		// Blit resources
		ComPtr<ID3D11VertexShader> blitVS;
		ComPtr<ID3D11PixelShader> blitPS;
		ComPtr<ID3D11SamplerState> samplerState;
		ComPtr<ID3D11RasterizerState> noCullRS;  // Rasterizer state with culling disabled
		ComPtr<ID3D11BlendState> anaglyphRedBS;
		ComPtr<ID3D11BlendState> anaglyphCyanBS;
		ComPtr<ID3D11VertexShader> solidColorVS;
		ComPtr<ID3D11PixelShader> solidColorPS;
		ComPtr<ID3D11InputLayout> simpleVertexLayout = nullptr;
		ComPtr<ID3D11Buffer> colorConstantBuffer;
		ComPtr<ID3D11Buffer> viewportConstantBuffer;
		ComPtr<ID3DBlob> solidColorVSBlob;
		ComPtr<ID3DBlob> solidColorPSBlob;


		// Desktop preview window (no thread - handled on main thread)
		HWND hwnd{ nullptr };
		std::atomic<bool> isFocused{ false };
		ComPtr<IDXGISwapChain1> previewSwapchain;
		UINT previewWidth{ 1920 };
		UINT previewHeight{ 540 };
		DXGI_FORMAT previewFormat{ DXGI_FORMAT_UNKNOWN };  // Track format for matching
		std::mutex previewMutex;
	};

	struct Swapchain {
		XrSwapchain handle{ (XrSwapchain)1 };
		DXGI_FORMAT format{ DXGI_FORMAT_R8G8B8A8_UNORM };
		uint32_t width{ 0 }, height{ 0 }, arraySize{ 2 };
		uint32_t mipCount{ 1 };
		// Backend type and images
		enum class Backend { D3D11, D3D12, OpenGL } backend{ Backend::D3D11 };
		std::vector<ComPtr<ID3D11Texture2D>> images;      // D3D11 path
		std::vector<ComPtr<ID3D12Resource>> images12;     // D3D12 path
		std::vector<D3D12_RESOURCE_STATES> imageStates12;
		std::vector<GLuint> imagesGL;                     // OpenGL path
		GLenum glInternalFormat{ GL_RGBA8 };                // OpenGL internal format
		uint32_t nextIndex{ 0 };
		uint32_t lastAcquired{ UINT32_MAX };  // Initialize to invalid
		uint32_t lastReleased{ UINT32_MAX };  // Initialize to invalid
		uint32_t imageCount{ 3 };
	};

	static Instance g_instance{};
	static Session g_session{};
	static std::unordered_map<XrSwapchain, Swapchain> g_swapchains;

	// Head tracking state for mouse look and WASD movement
	static XrVector3f g_headPos = { 0.0f, 1.7f, 0.0f };  // Start at standing eye height
	static float g_headYaw = 0.0f;    // Rotation around Y axis (left/right)
	static float g_headPitch = 0.0f;  // Rotation around X axis (up/down)
	static float g_headRoll = 0.0f;  // Rotation around Z axis (roll)
	static bool g_mouseCapture = false;
	static POINT g_lastMousePos = { 0, 0 };

	// Controller tracking state for motion controller emulation
	// Positions are relative to head position, orientation follows head by default
	struct ControllerState {
		XrVector3f posOffset;  // Offset from head position (in head-local space)
		float yawOffset;       // Additional yaw relative to head
		float pitchOffset;     // Additional pitch relative to head
		float rollOffset;      // Additional roll relative to head
		bool isTracking;       // Whether controller is "tracked"

		// Input state for button/trigger emulation
		bool triggerPressed;   // Primary trigger (fire)
		bool gripPressed;      // Grip button
		bool menuPressed;      // Menu button
		bool primaryPressed;   // Primary button (A/X)
		bool secondaryPressed; // Secondary button (B/Y)
		bool thumbstickPressed;// Thumbstick click
		float triggerValue;    // 0.0-1.0 trigger analog value
		float gripValue;       // 0.0-1.0 grip analog value
		XrVector2f thumbstick; // -1.0 to 1.0 thumbstick position

		// Velocity tracking for motion detection
		XrVector3f prevPosWorld;    // Previous frame world position
		XrVector3f linearVelocity;  // m/s in world space
		XrVector3f angularVelocity; // rad/s
		float prevYaw;              // Previous yaw for angular velocity
		float prevPitch;            // Previous pitch for angular velocity
	};
	static ControllerState g_leftController = {
		{-0.2f, -0.3f, -0.4f}, 0.0f, -0.3f, true,  // Position/orientation
		false, false, false, false, false, false,   // Button states
		0.0f, 0.0f, 0.0f, {0.0f, 0.0f},                   // Analog values
		{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f  // Velocity tracking
	};
	static ControllerState g_rightController = {
		{0.2f, -0.3f, -0.4f}, 0.0f, -0.3f, true,   // Position/orientation
		false, false, false, false, false, false,   // Button states
		0.0f, 0.0f, 0.0f, {0.0f, 0.0f},                   // Analog values
		{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f  // Velocity tracking
	};

	// Map XrSpace handles to controller type (0=none, 1=left grip, 2=left aim, 3=right grip, 4=right aim)
	static std::unordered_map<XrSpace, int> g_controllerSpaces;

	// Map XrPath to path string for controller detection
	static std::unordered_map<XrPath, std::string> g_pathStrings;

	// Map XrAction to action name for input mapping
	static std::unordered_map<XrAction, std::string> g_actionNames;

	// Map XrAction to which hand it's bound to (0=both/any, 1=left, 2=right)
	static std::unordered_map<XrAction, int> g_actionHand;

	// Time tracking for velocity calculation
	static XrTime g_lastFrameTime = 0;

	// Get controller world pose (combines head pose with controller offset)
	static void GetControllerPose(const ControllerState& ctrl, XrPosef* outPose, bool isRight) {
		//----------------
		//OXRWXR CHANGE:
		//---------------- 
		// We get real position + rotation now
		
		// Controller orientation = head orientation + controller offsets
		//float totalYaw = g_headYaw + ctrl.yawOffset;
		//float totalPitch = g_headPitch + ctrl.pitchOffset;
		//float totalRoll = g_headRoll + ctrl.rollOffset;
		//outPose->orientation = QuatFromYawPitchRoll(totalYaw, totalPitch, totalRoll); //QuatFromYawPitch(totalYaw, totalPitch);

		// Controller position = head position + rotated offset
		// Rotate the offset by head yaw only (not pitch) for natural hand movement
		//XrQuaternionf headYawQ = QuatFromYawPitch(g_headYaw, 0.0f);

		// Simple rotation of offset by head yaw
		//float cosY = cosf(g_headYaw);
		//float sinY = sinf(g_headYaw);
		//outPose->position.x = g_headPos.x + ctrl.posOffset.x * cosY - ctrl.posOffset.z * sinY;
		//outPose->position.y = g_headPos.y + ctrl.posOffset.y;
		//outPose->position.z = g_headPos.z + ctrl.posOffset.x * sinY + ctrl.posOffset.z * cosY;

		if (isRight) {
			outPose->orientation = { RHandQuat.x, RHandQuat.y, RHandQuat.z, RHandQuat.w };
			outPose->position = RHandPos;
		}
		else {
			outPose->orientation = { LHandQuat.x, LHandQuat.y, LHandQuat.z, LHandQuat.w };
			outPose->position = LHandPos;
		}
	}

	// Helper function to create quaternion from yaw and pitch
	XrQuaternionf QuatFromYawPitch(float yaw, float pitch) {
		// Create rotation: first yaw around Y, then pitch around X
		float cy = cosf(yaw * 0.5f);
		float sy = sinf(yaw * 0.5f);
		float cp = cosf(pitch * 0.5f);
		float sp = sinf(pitch * 0.5f);

		// Combine rotations (yaw * pitch)
		XrQuaternionf q;
		q.w = cy * cp;
		q.x = cy * sp;
		q.y = sy * cp;
		q.z = -sy * sp;
		return q;
	}

	// Helper function to create quaternion from yaw and pitch and roll
	XrQuaternionf QuatFromYawPitchRoll(float yaw, float pitch, float roll) {
		// Create rotations
		float cy = cosf(yaw * 0.5f);
		float sy = sinf(yaw * 0.5f);
		float cp = cosf(pitch * 0.5f);
		float sp = sinf(pitch * 0.5f);
		float cr = cosf(roll * 0.5f);
		float sr = sinf(roll * 0.5f);

		// Combine rotations (yaw * pitch * roll)
		XrQuaternionf q;
		q.w = cy * cp * cr + sy * sp * sr;
		q.x = cy * sp * cr + sy * cp * sr;
		q.y = sy * cp * cr - cy * sp * sr;
		q.z = cy * cp * sr - sy * sp * cr;
		return q;
	}

	// Rotate a vector by a quaternion (q * v * q^-1)
	static inline XrVector3f RotateVectorByQuaternion(const XrQuaternionf& q, const XrVector3f& v) {
		XrQuaternionf qv{ v.x, v.y, v.z, 0.0f };
		XrQuaternionf qinv{ -q.x, -q.y, -q.z, q.w };
		auto mul = [](const XrQuaternionf& a, const XrQuaternionf& b) -> XrQuaternionf {
			return XrQuaternionf{
				a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
				a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
				a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
				a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
			};
			};
		XrQuaternionf t = mul(q, qv);
		XrQuaternionf r = mul(t, qinv);
		return XrVector3f{ r.x, r.y, r.z };
	}

	// Initialize shader resources for blitting
	bool InitBlitResources(Session& s) {
		if (s.blitVS && s.blitPS && s.samplerState && s.noCullRS &&
			s.anaglyphRedBS && s.anaglyphCyanBS) {
			return true;
		}

		// Compile shaders
		const char* shaderSource = R"(
        Texture2D txDiffuse : register(t0);
        SamplerState samLinear : register(s0);

        struct VS_OUTPUT {
            float4 Pos : SV_POSITION;
            float2 Tex : TEXCOORD;
        };

        // Vertex Shader (generates fullscreen quad with correct UV mapping)
        VS_OUTPUT VSMain(uint vertexId : SV_VertexID) {
            VS_OUTPUT output;
            // Generate (0,0), (2,0), (0,2), (2,2) pattern
            float2 xy = float2((vertexId << 1) & 2, vertexId & 2);

            // Clip-space position: xy goes 0-2, we need -1 to 1
            // x: 0->-1, 2->1  means x_clip = xy.x - 1
            // y: 0->1, 2->-1  means y_clip = 1 - xy.y
            output.Pos = float4(xy.x - 1.0, 1.0 - xy.y, 0.0, 1.0);

            // Normalized UVs (0-1 range, not 0-2)
            output.Tex = xy * 0.5;

            return output;
        }

        // Pixel Shader - GPU handles sRGB conversion automatically with proper formats
        float4 PSMain(VS_OUTPUT input) : SV_TARGET {
            return txDiffuse.Sample(samLinear, input.Tex);
        }
    )";

		ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
		HRESULT hr;
		UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

		// Compile VS
		hr = D3DCompile(shaderSource, strlen(shaderSource), "BlitShader", nullptr, nullptr,
			"VSMain", "vs_5_0", compileFlags, 0, vsBlob.GetAddressOf(), errorBlob.GetAddressOf());
		if (FAILED(hr)) {
			Logf("[OXRWXR] Failed to compile VS: %s", errorBlob ? (char*)errorBlob->GetBufferPointer() : "Unknown error");
			return false;
		}
		hr = s.d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
			nullptr, s.blitVS.GetAddressOf());
		if (FAILED(hr)) { Logf("[OXRWXR] Failed to create VS: 0x%08X", hr); return false; }

		// Compile PS
		hr = D3DCompile(shaderSource, strlen(shaderSource), "BlitShader", nullptr, nullptr,
			"PSMain", "ps_5_0", compileFlags, 0, psBlob.GetAddressOf(), errorBlob.GetAddressOf());
		if (FAILED(hr)) {
			Logf("[OXRWXR] Failed to compile PS: %s", errorBlob ? (char*)errorBlob->GetBufferPointer() : "Unknown error");
			return false;
		}
		hr = s.d3d11Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
			nullptr, s.blitPS.GetAddressOf());
		if (FAILED(hr)) { Logf("[OXRWXR] Failed to create PS: 0x%08X", hr); return false; }

		//----------------
		//OXRWXR CHANGE:
		//---------------- 
		// OpenXR Frame Data Shader
		const char* solidColorShaderSource = R"(
			cbuffer ViewportBuffer : register(b1) {
				float2 viewportSize;
			};

			struct VertexInput
			{
				float2 pos : POSITION;
			};

			cbuffer ColorBuffer : register(b0) {
				float4 color;
			};

			struct PixelInput
			{
				float4 pos : SV_POSITION;
			};

			PixelInput VSMain(VertexInput input)
			{
				PixelInput output;
				output.pos = float4(input.pos.x / viewportSize.x * 2 - 1, 1 - input.pos.y / viewportSize.y * 2, 0, 1);
				return output;
			}

			float4 PSMain(PixelInput input) : SV_TARGET {
				return color;
			}
		)";

		// Compile vertex shader
		hr = D3DCompile(solidColorShaderSource, strlen(solidColorShaderSource), "SolidColorShader", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &s.solidColorVSBlob, &errorBlob);
		if (FAILED(hr))
		{
			Logf("[WinXrApi] Failed to compile VS: %s", errorBlob ? (char*)errorBlob->GetBufferPointer() : "Unknown error");
			return false;
		}

		// Create vertex shader
		hr = s.d3d11Device->CreateVertexShader(s.solidColorVSBlob->GetBufferPointer(), s.solidColorVSBlob->GetBufferSize(), nullptr, s.solidColorVS.GetAddressOf());
		if (FAILED(hr))
		{
			Logf("[WinXrApi] Failed to create VS: 0x%08X", hr);
			return false;
		}

		// Compile pixel shader
		hr = D3DCompile(solidColorShaderSource, strlen(solidColorShaderSource), "SolidColorShader", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &s.solidColorPSBlob, &errorBlob);
		if (FAILED(hr))
		{
			Logf("[WinXrApi] Failed to compile PS: %s", errorBlob ? (char*)errorBlob->GetBufferPointer() : "Unknown error");
			return false;
		}

		// Create pixel shader
		hr = s.d3d11Device->CreatePixelShader(s.solidColorPSBlob->GetBufferPointer(), s.solidColorPSBlob->GetBufferSize(), nullptr, s.solidColorPS.GetAddressOf());
		if (FAILED(hr))
		{
			Logf("[WinXrApi] Failed to create PS: 0x%08X", hr);
			return false;
		}

		// Create Sampler State
		D3D11_SAMPLER_DESC sampDesc = {};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampDesc.MinLOD = 0;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		hr = s.d3d11Device->CreateSamplerState(&sampDesc, s.samplerState.GetAddressOf());
		if (FAILED(hr)) { Logf("[OXRWXR] Failed to create SamplerState: 0x%08X", hr); return false; }

		// Create Rasterizer State with culling disabled
		D3D11_RASTERIZER_DESC rsDesc{};
		rsDesc.FillMode = D3D11_FILL_SOLID;
		rsDesc.CullMode = D3D11_CULL_NONE;  // Disable culling to prevent triangles from being discarded
		rsDesc.FrontCounterClockwise = FALSE;
		rsDesc.DepthClipEnable = TRUE;
		rsDesc.ScissorEnable = FALSE;
		rsDesc.MultisampleEnable = FALSE;
		rsDesc.AntialiasedLineEnable = FALSE;
		hr = s.d3d11Device->CreateRasterizerState(&rsDesc, s.noCullRS.GetAddressOf());
		if (FAILED(hr)) { Logf("[OXRWXR] Failed to create RasterizerState: 0x%08X", hr); return false; }

		// Create blend states for anaglyph rendering
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.AlphaToCoverageEnable = FALSE;
		blendDesc.IndependentBlendEnable = FALSE;
		blendDesc.RenderTarget[0].BlendEnable = FALSE;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
		hr = s.d3d11Device->CreateBlendState(&blendDesc, s.anaglyphRedBS.GetAddressOf());
		if (FAILED(hr)) { Logf("[OXRWXR] Failed to create anaglyph red blend state: 0x%08X", hr); return false; }

		blendDesc.RenderTarget[0].RenderTargetWriteMask =
			D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		hr = s.d3d11Device->CreateBlendState(&blendDesc, s.anaglyphCyanBS.GetAddressOf());
		if (FAILED(hr)) { Logf("[OXRWXR] Failed to create anaglyph cyan blend state: 0x%08X", hr); return false; }

		Log("[OXRWXR] Blit resources initialized successfully.");
		return true;
	}

	static void ResetD3D12PreviewResources(rt::Session& s) {
		s.previewSwapchain12.Reset();
		s.previewRTVHeap.Reset();
		s.previewBackbuffers.clear();
		s.previewBackbufferCount = 0;
		s.previewRTVDescriptorSize = 0;
		s.previewCmdAlloc.Reset();
		s.previewCmdList.Reset();
		s.previewFence.Reset();
		s.previewFenceValue = 0;
		if (s.previewFenceEvent) {
			CloseHandle(s.previewFenceEvent);
			s.previewFenceEvent = nullptr;
		}
	}

	static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		switch (msg) {
		case WM_CLOSE:
			if (rt::g_session.handle != XR_NULL_HANDLE) {
				rt::PushState(rt::g_session.handle, XR_SESSION_STATE_EXITING);
			}
			Log("[OXRWXR] WndProc: WM_CLOSE received");
			DestroyWindow(hWnd);
			return 0;
		case WM_DESTROY:
			// DON'T call PostQuitMessage - we're a DLL, not the main app!
			// PostQuitMessage would tell the host application to exit.
			Log("[OXRWXR] WndProc: WM_DESTROY received");
			return 0;
		case WM_ACTIVATE:
			if (LOWORD(wParam) != WA_INACTIVE) {
				rt::g_session.isFocused = true;
				Log("[OXRWXR] WndProc: WM_ACTIVATE -> focused");
				// Push FOCUSED state if we were VISIBLE
				if (rt::g_session.state == XR_SESSION_STATE_VISIBLE) {
					rt::PushState(rt::g_session.handle, XR_SESSION_STATE_FOCUSED);
				}
			}
			else {
				rt::g_session.isFocused = false;
				Log("[OXRWXR] WndProc: WM_ACTIVATE -> unfocused");
				rt::g_mouseCapture = false;  // Release mouse capture when window loses focus
				ReleaseCapture();
				// Push VISIBLE state if we were FOCUSED
				if (rt::g_session.state == XR_SESSION_STATE_FOCUSED) {
					rt::PushState(rt::g_session.handle, XR_SESSION_STATE_VISIBLE);
				}
			}
			return 0;
		case WM_LBUTTONDOWN:
			Logf("[OXRWXR] WM_LBUTTONDOWN: focused=%d", rt::g_session.isFocused.load());
			if (rt::g_session.isFocused) {
				/*rt::g_mouseCapture = true;
				SetCapture(hWnd);
				GetCursorPos(&rt::g_lastMousePos);
				ShowCursor(FALSE);
				Log("[OXRWXR] Mouse captured for look control");*/
			}
			return 0;
		case WM_LBUTTONUP:
			if (rt::g_mouseCapture) {
				rt::g_mouseCapture = false;
				ReleaseCapture();
				ShowCursor(TRUE);
			}
			return 0;
		case WM_MOUSEMOVE:
			if (rt::g_mouseCapture) {
				POINT currentPos;
				GetCursorPos(&currentPos);

				// Calculate delta
				int deltaX = currentPos.x - rt::g_lastMousePos.x;
				int deltaY = currentPos.y - rt::g_lastMousePos.y;

				// Update yaw and pitch (with sensitivity)
				const float sensitivity = 0.002f;
				rt::g_headYaw -= deltaX * sensitivity;
				rt::g_headPitch -= deltaY * sensitivity;  // Inverted for natural feel

				// Clamp pitch to avoid gimbal lock
				const float maxPitch = 1.5f;  // ~85 degrees
				if (rt::g_headPitch > maxPitch) rt::g_headPitch = maxPitch;
				if (rt::g_headPitch < -maxPitch) rt::g_headPitch = -maxPitch;

				// Reset cursor to center of window to avoid hitting screen edges
				RECT rect;
				GetWindowRect(hWnd, &rect);
				int centerX = (rect.left + rect.right) / 2;
				int centerY = (rect.top + rect.bottom) / 2;
				SetCursorPos(centerX, centerY);
				rt::g_lastMousePos.x = centerX;
				rt::g_lastMousePos.y = centerY;
			}
			return 0;
		case WM_COMMAND:
			if (ui::HandleMenuCommand(hWnd, wParam,
				[]() { /* Resize handled by presentProjection based on zoom */ },
				//[]() { mcp::g_screenshotRequested = true; },
				[]() { rt::g_headPos = { 0, 1.7f, 0 }; rt::g_headYaw = 0; rt::g_headPitch = 0; }
			)) {
				return 0;
			}
			break;
		case WM_KEYDOWN:
			if (!rt::g_mouseCapture) {
				if (ui::HandleKeyboardShortcut(hWnd, wParam,
					[]() { /* Resize handled by presentProjection based on zoom */ },
					//[]() { mcp::g_screenshotRequested = true; },
					[]() { rt::g_headPos = { 0, 1.7f, 0 }; rt::g_headYaw = 0; rt::g_headPitch = 0; }
				)) {
					return 0;
				}
			}
			break;
		case WM_MOUSEWHEEL:
			/*if (ui::HandleMouseWheel(hWnd, GET_WHEEL_DELTA_WPARAM(wParam), nullptr)) {
				return 0;
			}*/
			break;
		default:
			break;
		}
		return DefWindowProc(hWnd, msg, wParam, lParam);
	}

	static void ensurePreview(Session& s) {
		if (s.hwnd) return;
		WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"OpenXR WXR";
		RegisterClassW(&wc);
		s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"OpenXR WXR", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			CW_USEDEFAULT, CW_USEDEFAULT, (int)s.previewWidth, (int)s.previewHeight, nullptr, nullptr, wc.hInstance, nullptr);
		Logf("[OXRWXR] ensurePreview: hwnd=%p size=%ux%u usesD3D12=%d", s.hwnd, s.previewWidth, s.previewHeight, s.usesD3D12);

		// Make sure window is shown and updated
		if (s.hwnd) {
			ShowWindow(s.hwnd, SW_SHOW);
			UpdateWindow(s.hwnd);

			// Check if window has focus
			if (GetForegroundWindow() == s.hwnd) {
				s.isFocused = true;
				Log("[OXRWXR] Window created with focus");
			}
			else {
				s.isFocused = false;
				Log("[OXRWXR] Window created without focus");
			}
		}

		// Swapchain creation is now handled in ensurePreviewSized for both D3D11 and D3D12
	}

	// Window thread functions removed - window now handled on main thread

} // namespace rt

// ----------------- OpenXR runtime exports -----------------

static XrResult XRAPI_PTR xrGetInstanceProcAddr_runtime(XrInstance, const char* name, PFN_xrVoidFunction* fn);

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
	XrNegotiateRuntimeRequest* runtimeRequest) {
	try {
		EnsureLogFile();
		Log("\n[OXRWXR] ========== OpenXR WXR Runtime Starting ==========\n");
		if (!loaderInfo || !runtimeRequest) {
			Log("[OXRWXR] xrNegotiateLoaderRuntimeInterface: ERROR - null parameters");
			return XR_ERROR_INITIALIZATION_FAILED;
		}

		Logf("[OXRWXR] xrNegotiateLoaderRuntimeInterface: loaderInfo=%p, runtimeRequest=%p", loaderInfo, runtimeRequest);
		Logf("[OXRWXR]   Loader minInterfaceVersion=%u, maxInterfaceVersion=%u, minApiVersion=0x%X, maxApiVersion=0x%X",
			loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion,
			loaderInfo->minApiVersion, loaderInfo->maxApiVersion);

		runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
		runtimeRequest->getInstanceProcAddr = xrGetInstanceProcAddr_runtime;
		runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;

		Logf("[OXRWXR] xrNegotiateLoaderRuntimeInterface: SUCCESS - runtimeApiVersion=0x%X (%u)",
			runtimeRequest->runtimeApiVersion, runtimeRequest->runtimeApiVersion);
		return XR_SUCCESS;
	}
	catch (...) {
		Log("[OXRWXR] xrNegotiateLoaderRuntimeInterface: EXCEPTION caught!");
		return XR_ERROR_INITIALIZATION_FAILED;
	}
}

// xrGetD3D12GraphicsRequirementsKHR (XR_KHR_D3D12_enable)
static XrResult XRAPI_PTR xrGetD3D12GraphicsRequirementsKHR_runtime(
	XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D12KHR* req) {
	Logf("[OXRWXR] xrGetD3D12GraphicsRequirementsKHR called: instance=%p, systemId=%llu, req=%p",
		instance, (unsigned long long)systemId, req);
	if (!req) return XR_ERROR_VALIDATION_FAILURE;

	memset(req, 0, sizeof(*req));
	req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
	req->next = nullptr;

	Microsoft::WRL::ComPtr<IDXGIFactory1> f;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(f.GetAddressOf()));
	if (FAILED(hr)) return XR_ERROR_RUNTIME_FAILURE;

	for (UINT i = 0;; ++i) {
		Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
		if (f->EnumAdapters1(i, a.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
		DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
		if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
		req->adapterLuid = d.AdapterLuid;
		break;
	}
	req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
	Log("[OXRWXR] xrGetD3D12GraphicsRequirementsKHR: SUCCESS");
	return XR_SUCCESS;
}
// xrGetD3D11GraphicsRequirementsKHR (XR_KHR_D3D11_enable)
static XrResult XRAPI_PTR xrGetD3D11GraphicsRequirementsKHR_runtime(
	XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D11KHR* req) {
	Logf("[OXRWXR] xrGetD3D11GraphicsRequirementsKHR called: instance=%p, systemId=%llu, req=%p",
		instance, (unsigned long long)systemId, req);
	if (!req) {
		Log("[OXRWXR] xrGetD3D11GraphicsRequirementsKHR: ERROR - null req");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	Logf("[OXRWXR] xrGetD3D11GraphicsRequirementsKHR: req struct size = %zu, expected = %zu",
		sizeof(*req), sizeof(XrGraphicsRequirementsD3D11KHR));

	// Check if the struct type is already set (Unity might pre-fill it)
	if (req->type != 0) {
		Logf("[OXRWXR] xrGetD3D11GraphicsRequirementsKHR: req->type already set to %d", req->type);
	}

	// Zero initialize the entire structure first
	memset(req, 0, sizeof(XrGraphicsRequirementsD3D11KHR));
	req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
	req->next = nullptr;

	Microsoft::WRL::ComPtr<IDXGIFactory1> f;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(f.GetAddressOf()));
	if (FAILED(hr)) {
		Logf("[OXRWXR] CreateDXGIFactory1 failed: 0x%08X", hr);
		return XR_ERROR_RUNTIME_FAILURE;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter1> bestAdapter;
	DXGI_ADAPTER_DESC1 bestDesc{};
	bool foundHardware = false;

	// Find the best hardware adapter
	for (UINT i = 0; ; ++i) {
		Microsoft::WRL::ComPtr<IDXGIAdapter1> adapt;
		if (f->EnumAdapters1(i, adapt.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
			break;

		DXGI_ADAPTER_DESC1 d{};
		adapt->GetDesc1(&d);

		// Skip software adapters
		if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			continue;

		// Use the first hardware adapter we find
		if (!foundHardware) {
			bestAdapter = adapt;
			bestDesc = d;
			foundHardware = true;

			wchar_t* descStr = d.Description;
			char descAscii[128];
			wcstombs(descAscii, descStr, sizeof(descAscii));
			descAscii[sizeof(descAscii) - 1] = '\0';
			Logf("[OXRWXR] Found hardware adapter: %s", descAscii);
			Logf("[OXRWXR]   LUID: High=%ld, Low=%lu",
				(long)d.AdapterLuid.HighPart,
				(unsigned long)d.AdapterLuid.LowPart);
			Logf("[OXRWXR]   Dedicated Video Memory: %llu MB",
				(unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024)));
		}
	}

	if (foundHardware) {
		req->adapterLuid = bestDesc.AdapterLuid;
		req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

		// Save this LUID for later validation
		rt::g_adapterLuid = bestDesc.AdapterLuid;
		rt::g_adapterLuidSet = true;

		Logf("[OXRWXR] xrGetD3D11GraphicsRequirementsKHR: Returning:");
		Logf("[OXRWXR]   type = %d (expected %d)", req->type, XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR);
		Logf("[OXRWXR]   next = %p", req->next);
		Logf("[OXRWXR]   adapterLuid.HighPart = %ld (0x%08X)",
			(long)req->adapterLuid.HighPart, (unsigned)req->adapterLuid.HighPart);
		Logf("[OXRWXR]   adapterLuid.LowPart = %lu (0x%08X)",
			(unsigned long)req->adapterLuid.LowPart, (unsigned)req->adapterLuid.LowPart);
		Logf("[OXRWXR]   minFeatureLevel = 0x%X (D3D_FEATURE_LEVEL_11_0 = 0x%X)",
			req->minFeatureLevel, D3D_FEATURE_LEVEL_11_0);

		Log("[OXRWXR] xrGetD3D11GraphicsRequirementsKHR: SUCCESS - Returning XR_SUCCESS");
		return XR_SUCCESS;
	}

	// No hardware adapter found, this is an error for VR
	Log("[OXRWXR] xrGetD3D11GraphicsRequirementsKHR: ERROR - No hardware graphics adapter found");
	return XR_ERROR_SYSTEM_INVALID;
}

// xrGetOpenGLGraphicsRequirementsKHR (XR_KHR_opengl_enable)
static XrResult XRAPI_PTR xrGetOpenGLGraphicsRequirementsKHR_runtime(
	XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsOpenGLKHR* req) {
	Logf("[OXRWXR] xrGetOpenGLGraphicsRequirementsKHR called: instance=%p, systemId=%llu, req=%p",
		instance, (unsigned long long)systemId, req);
	if (!req) {
		Log("[OXRWXR] xrGetOpenGLGraphicsRequirementsKHR: ERROR - null req");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	// Zero initialize the structure
	memset(req, 0, sizeof(XrGraphicsRequirementsOpenGLKHR));
	req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
	req->next = nullptr;

	// Minimum OpenGL version: 4.0.0 (good compatibility)
	// Maximum: 4.6.0 (latest)
	req->minApiVersionSupported = XR_MAKE_VERSION(4, 0, 0);
	req->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);

	Logf("[OXRWXR] xrGetOpenGLGraphicsRequirementsKHR: min=%d.%d.%d, max=%d.%d.%d",
		XR_VERSION_MAJOR(req->minApiVersionSupported),
		XR_VERSION_MINOR(req->minApiVersionSupported),
		XR_VERSION_PATCH(req->minApiVersionSupported),
		XR_VERSION_MAJOR(req->maxApiVersionSupported),
		XR_VERSION_MINOR(req->maxApiVersionSupported),
		XR_VERSION_PATCH(req->maxApiVersionSupported));

	Log("[OXRWXR] xrGetOpenGLGraphicsRequirementsKHR: SUCCESS");
	return XR_SUCCESS;
}

// --- Minimal implementations ---

static const char* kSupportedExtensions[] = {
	XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
	XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
	XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,  // OpenGL support
	XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,
	XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,  // UEVR uses this for UI layers
	"XR_KHR_win32_convert_performance_counter_time"    // Unity often requires this
};

static XrResult XRAPI_PTR xrEnumerateApiLayerProperties_runtime(uint32_t propertyCapacityInput,
	uint32_t* propertyCountOutput,
	XrApiLayerProperties* properties) {
	Log("[OXRWXR] xrEnumerateApiLayerProperties called");
	// Runtime doesn't provide API layers, only extensions
	if (propertyCountOutput) *propertyCountOutput = 0;
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrEnumerateInstanceExtensionProperties_runtime(const char* layerName, uint32_t propertyCapacityInput,
	uint32_t* propertyCountOutput,
	XrExtensionProperties* properties) {
	if (layerName && layerName[0] != '\0') return XR_ERROR_LAYER_INVALID;
	const uint32_t count = (uint32_t)(sizeof(kSupportedExtensions) / sizeof(kSupportedExtensions[0]));
	if (propertyCountOutput) *propertyCountOutput = count;
	if (properties && propertyCapacityInput) {
		for (uint32_t i = 0; i < propertyCapacityInput && i < count; ++i) {
			properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
			properties[i].next = nullptr;
			std::strncpy(properties[i].extensionName, kSupportedExtensions[i], XR_MAX_EXTENSION_NAME_SIZE - 1);
			properties[i].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
			properties[i].extensionVersion = 1;
			Logf("[OXRWXR] ext[%u]=%s", i, properties[i].extensionName);
		}
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateInstance_runtime(const XrInstanceCreateInfo* createInfo, XrInstance* instance) {
	//----------------
	//OXRWXR CHANGE:
	//---------------- 
	// Do first time setup for this instance
	Logf("[WinXrApi] Starting Up");
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	std::filesystem::path tmpPath = "Z:/";
	std::filesystem::path dirPath = "Z:/tmp/xr";
	std::filesystem::path confPath = "D:/oxrwxr";
	std::filesystem::path confFile = confPath / "conf.txt";
	std::filesystem::path fallbackDir = "D:/xrtemp";
	std::filesystem::path filePath = dirPath / "vr";
	std::filesystem::path fallbackFile = fallbackDir / "vr";

	std::filesystem::path versionPath = dirPath / "version";
	std::filesystem::path fallbackVersion = fallbackDir / "version";

	if (std::filesystem::exists(tmpPath) && std::filesystem::is_directory(tmpPath))
	{
		if (std::filesystem::exists(dirPath) && std::filesystem::is_directory(dirPath))
		{
			// Nothing to do
		}
		else
		{
			// Try to create the folder
			try
			{
				std::filesystem::create_directories(dirPath);
			}
			catch (const std::exception& e)
			{
				Logf("[WinXrApi] Error creating tmp/xr directory: %s", e.what());
			}
		}

		try
		{
			std::ofstream verFile(versionPath);
			if (verFile.is_open())
			{
				verFile << "0.2";
				verFile.close();
			}
			else
			{
			}
		}
		catch (const std::exception& e)
		{
			Logf("[WinXrApi] Error writing VERSION file: %s", e.what());
		}

		try
		{
			std::ofstream file(filePath);
			if (file.is_open())
			{
				file << "VR";
				file.close();
			}
			else
			{
			}
		}
		catch (const std::exception& e)
		{
			Logf("[WinXrApi] Error writing VR file: %s", e.what());
		}
	}
	else
	{
		try
		{
			std::ofstream verFile(fallbackVersion);
			if (verFile.is_open())
			{
				verFile << "0.2";
				verFile.close();
			}
			else
			{
			}
		}
		catch (const std::exception& e)
		{
			Logf("[WinXrApi] Error writing test VERSION file: %s", e.what());
		}

		try
		{
			std::ofstream file(fallbackFile);
			if (file.is_open())
			{
				file << "VR";
				file.close();
			}
			else
			{
			}
		}
		catch (const std::exception& e)
		{
			Logf("[WinXrApi] Error writing test VR file: %s", e.what());
		}

		dirPath = fallbackDir;
	}

	std::filesystem::path sysinfoPath = dirPath / "system";

	if (std::filesystem::exists(dirPath) && std::filesystem::is_directory(dirPath)) {
		if (std::filesystem::exists(sysinfoPath) && std::filesystem::is_regular_file(sysinfoPath)) {
			try {
				std::ifstream sysInfoFile(sysinfoPath);

				std::string hmdMakeStr;
				std::string hmdModelStr;

				if (sysInfoFile.is_open()) {
					std::getline(sysInfoFile, hmdMakeStr);
					std::getline(sysInfoFile, hmdModelStr);
					sysInfoFile.close();
				}

				hmdMake = hmdMakeStr;
				hmdModel = hmdModelStr;
			}
			catch (const std::filesystem::filesystem_error& e) {

			}
			catch (const std::exception& e) {

			}
		}
	}

	if (hmdMake.empty()) {
		hmdMake = "META";
	}
	else if (hmdMake == "OCULUS") {
		hmdMake = "META";
	}

	if (hmdModel.empty()) {
		hmdModel = "QUEST 3";
	}
	else if (hmdModel == "EUREKA" || hmdModel == "PANTHER") {
		hmdModel = "QUEST 3";
	}
	else if (hmdMake == "META") {
		//SEACLIFF - Quest Pro - Untested, assuming hands upside down
		//HOLLYWOOD - Quest 2 - Works! Hands upside down, performance is OK (much better with Turnip driver)
		//MONTEREY - Quest 1 - Untested, unsupported
		hmdModel = "QUEST 2";
	}

	//Look for the flag to force hand fix ON always:
	std::filesystem::path handFixFile = confPath / "handsfix.txt";

	if (std::filesystem::exists(handFixFile) && std::filesystem::is_regular_file(handFixFile)) {
		hmdMake = "FORCE FIX HANDS";
	}

	//XRTODO: define the conf data expected and allow it to be sent here
	//Load the config for the OXRWXR runtime
	if (std::filesystem::exists(confPath) && std::filesystem::is_directory(confPath)) {
		if (std::filesystem::exists(confFile) && std::filesystem::is_regular_file(confFile)) {
			try {
				std::ifstream confFileOpen(confFile);

				std::string confLine1;
				std::string confLine2;

				if (confFileOpen.is_open()) {
					std::getline(confFileOpen, confLine1);
					std::getline(confFileOpen, confLine2);
					confFileOpen.close();
				}

				//Logf("[WinXrApi] Conf found: %s, %s", confLine1, confLine2);
			}
			catch (const std::filesystem::filesystem_error& e) {

			}
			catch (const std::exception& e) {

			}
		}
	}

	Logf("[WinXrUDP] Starting UDP");
	udpReader = new WinXrApiUDP();

	std::string aerMode = "1"; //3D SBS (0 for monocular VR)

	// AER is not likely necessary for non-VR apps, they can use SBS via reshade most often
	/*if (bEnableAltEyeRendering)
	{
		aerMode = "2";
	}*/

	float targetFOVH = 104.5;
	float targetFOVW = 104.5;

	if (udpReader) {
		//Now we send the VR mode enable and target FOV of WinlatorXR at startup
		udpReader->SendData("0 0 1 " + aerMode + " " + std::to_string(fovVarE) + " " + std::to_string(fovVarF));
	}

	if (!createInfo || !instance) return XR_ERROR_VALIDATION_FAILURE;
	// applicationName may not be null-terminated
	char appName[XR_MAX_APPLICATION_NAME_SIZE + 1] = { 0 };
	memcpy(appName, createInfo->applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE);
	Logf("[OXRWXR] xrCreateInstance: app=%s version=%u",
		appName,
		createInfo->applicationInfo.applicationVersion);

	rt::g_instance = {};
	rt::g_instance.enabledExtensions.clear();

	// Validate that all requested extensions are supported
	const uint32_t supportedCount = (uint32_t)(sizeof(kSupportedExtensions) / sizeof(kSupportedExtensions[0]));
	for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
		bool supported = false;
		for (uint32_t j = 0; j < supportedCount; ++j) {
			if (strcmp(createInfo->enabledExtensionNames[i], kSupportedExtensions[j]) == 0) {
				supported = true;
				break;
			}
		}
		if (!supported) {
			Logf("[OXRWXR] xrCreateInstance: ERROR - Unsupported extension %s", createInfo->enabledExtensionNames[i]);
			return XR_ERROR_EXTENSION_NOT_PRESENT;
		}
		rt::g_instance.enabledExtensions.emplace_back(createInfo->enabledExtensionNames[i]);
		Logf("[OXRWXR]   enabledExt[%u]=%s", i, createInfo->enabledExtensionNames[i]);
	}
	rt::g_instance.handle = (XrInstance)1;  // Set a valid handle
	*instance = rt::g_instance.handle;
	Log("[OXRWXR] xrCreateInstance: SUCCESS");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroyInstance_runtime(XrInstance instance) {
	//----------------
	//OXRWXR CHANGE:
	//---------------- 
	// Shutdown XrAPI + UDP
	try
	{
		Logf("[WinXrUDP] Shutting Down UDP");
		if (udpReader) {
			udpReader->KillReceiver();
			udpReader = nullptr;
		}
	}
	catch (const std::exception& e)
	{
		Logf("[WinXrUDP] Error killing UDP receiver: %s", e.what());
	}

	Logf("[WinXrApi] Shutting Down");

	Logf("[OXRWXR] xrDestroyInstance called: instance=%p", instance);

	// Clear the global instance
	if (instance == rt::g_instance.handle) {
		Log("[OXRWXR] xrDestroyInstance: Clearing global instance");
		rt::g_instance = {};

		// MUST destroy the window before DLL unloads!
		// The OpenXR loader may unload our DLL after this call.
		// If the window stays alive, its WndProc points to unloaded code = crash.
		{
			std::lock_guard<std::mutex> lock(rt::g_windowMutex);
			if (rt::g_persistentWindow) {
				Log("[OXRWXR] xrDestroyInstance: Destroying preview window");
				DestroyWindow(rt::g_persistentWindow);
				rt::g_persistentWindow = nullptr;
			}
			rt::g_persistentSwapchain.Reset();
		}
		// Also clear session window reference
		if (rt::g_session.hwnd) {
			rt::g_session.hwnd = nullptr;
		}

		// Unregister window class so it doesn't have dangling WndProc
		if (rt::g_windowClassRegistered) {
			UnregisterClassW(L"OpenXR WXR", GetModuleHandleW(nullptr));
			rt::g_windowClassRegistered = false;
			Log("[OXRWXR] xrDestroyInstance: Window class unregistered");
		}
		Log("[OXRWXR] xrDestroyInstance: Window destroyed for safe DLL unload");
	}

	Log("[OXRWXR] xrDestroyInstance: SUCCESS - Returning XR_SUCCESS");
	Log("[OXRWXR] ========== Instance Destroyed - Waiting for new instance ==========");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetInstanceProperties_runtime(XrInstance, XrInstanceProperties* props) {
	if (!props) return XR_ERROR_VALIDATION_FAILURE;
	props->type = XR_TYPE_INSTANCE_PROPERTIES;
	props->next = nullptr;
	props->runtimeVersion = XR_MAKE_VERSION(1, 0, 27);
	strncpy(props->runtimeName, "OpenXR WXR Runtime", XR_MAX_RUNTIME_NAME_SIZE - 1);
	props->runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
	Log("[OXRWXR] xrGetInstanceProperties: returning OpenXR WXR Runtime");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetSystem_runtime(XrInstance, const XrSystemGetInfo* info, XrSystemId* systemId) {
	if (!info || !systemId) return XR_ERROR_VALIDATION_FAILURE;
	Logf("[OXRWXR] xrGetSystem: formFactor=%d", info->formFactor);
	if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
		Log("[OXRWXR] xrGetSystem: ERROR - form factor not HMD");
		return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
	}
	*systemId = (XrSystemId)1;
	Log("[OXRWXR] xrGetSystem: SUCCESS -> systemId=1");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetSystemProperties_runtime(XrInstance, XrSystemId, XrSystemProperties* props) {
	if (!props) return XR_ERROR_VALIDATION_FAILURE;
	props->type = XR_TYPE_SYSTEM_PROPERTIES;
	props->next = nullptr;
	strncpy(props->systemName, "OpenXR WXR", XR_MAX_SYSTEM_NAME_SIZE - 1);
	props->systemName[XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
	props->systemId = 1;
	props->vendorId = 0;  // 0 = unknown vendor (more standard than 0xFFFF)
	props->graphicsProperties.maxSwapchainImageWidth = 4096;
	props->graphicsProperties.maxSwapchainImageHeight = 4096;
	props->graphicsProperties.maxLayerCount = 16;
	props->trackingProperties.positionTracking = XR_TRUE;
	props->trackingProperties.orientationTracking = XR_TRUE;
	Log("[OXRWXR] xrGetSystemProperties: returning OpenXR WXR");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateViewConfigurations_runtime(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count, XrViewConfigurationType* types) {
	Logf("[OXRWXR] xrEnumerateViewConfigurations called: capacity=%u", capacity);
	if (count) *count = 1;
	if (capacity >= 1 && types) {
		types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		Log("[OXRWXR] xrEnumerateViewConfigurations: Returning PRIMARY_STEREO");
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateViewConfigurationViews_runtime(XrInstance, XrSystemId, XrViewConfigurationType viewType, uint32_t capacity, uint32_t* count, XrViewConfigurationView* views) {
	Logf("[OXRWXR] xrEnumerateViewConfigurationViews called: viewType=%d, capacity=%u", (int)viewType, capacity);
	if (count) *count = 2;
	if (capacity >= 2 && views) {
		for (uint32_t i = 0; i < 2; ++i) {
			views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
			views[i].next = nullptr;
			views[i].recommendedImageRectWidth = 1280;
			views[i].recommendedImageRectHeight = 720;
			views[i].recommendedSwapchainSampleCount = 1;
			views[i].maxImageRectWidth = 4096; views[i].maxImageRectHeight = 4096; views[i].maxSwapchainSampleCount = 1;
		}
		Log("[OXRWXR] xrEnumerateViewConfigurationViews: Returned 2 views (1280x720 recommended)");
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateEnvironmentBlendModes_runtime(
	XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity, uint32_t* count, XrEnvironmentBlendMode* modes) {
	if (count) *count = 1;
	if (capacity >= 1 && modes) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateSession_runtime(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
	static int sessionCount = 0;
	sessionCount++;
	Log("[OXRWXR] ============================================");
	Logf("[OXRWXR] xrCreateSession called (call #%d, instance=%llu)", sessionCount, (unsigned long long)instance);
	Log("[OXRWXR] ============================================");
	if (!info || !session) return XR_ERROR_VALIDATION_FAILURE;

	// Check if we already have an active session
	if (rt::g_session.handle != XR_NULL_HANDLE && rt::g_session.state != XR_SESSION_STATE_IDLE) {
		Logf("[OXRWXR] xrCreateSession: ERROR - Session already exists (handle=%llu, state=%d)",
			(unsigned long long)rt::g_session.handle, rt::g_session.state);
		// For now, reset the existing session to allow the new one
		// Reset session manually
		rt::g_session.handle = XR_NULL_HANDLE;
		rt::g_session.state = XR_SESSION_STATE_IDLE;
		rt::g_session.d3d11Device.Reset();
		rt::g_session.d3d11Context.Reset();
		rt::g_session.previewSwapchain.Reset();
		rt::g_session.usesD3D12 = false;
		rt::g_session.d3d12Device.Reset();
		rt::g_session.d3d12Queue.Reset();
		rt::ResetD3D12PreviewResources(rt::g_session);
		rt::g_session.previewWidth = 1920;
		rt::g_session.previewHeight = 540;
		rt::g_session.isFocused = false;
	}
	// Accept D3D11 and D3D12
	const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(info->next);
	while (entry) {
		if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
			const auto* b = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);

			// Log the device details
			ComPtr<IDXGIDevice> dxgiDevice;
			if (SUCCEEDED(b->device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
				ComPtr<IDXGIAdapter> adapter;
				if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
					DXGI_ADAPTER_DESC desc;
					adapter->GetDesc(&desc);
					Logf("[OXRWXR] xrCreateSession: App D3D11 device LUID=%llu/%llu",
						(unsigned long long)desc.AdapterLuid.HighPart,
						(unsigned long long)desc.AdapterLuid.LowPart);
				}
			}

			// Use sessionCount to generate unique handles
			rt::g_session.handle = (XrSession)(uintptr_t)(0x1000 + sessionCount);
			rt::g_session.d3d11Device = b->device;
			rt::g_session.usesD3D12 = false;
			rt::g_session.d3d12Device.Reset();
			rt::g_session.d3d12Queue.Reset();
			rt::ResetD3D12PreviewResources(rt::g_session);
			rt::g_session.state = XR_SESSION_STATE_IDLE;
			b->device->GetImmediateContext(rt::g_session.d3d11Context.GetAddressOf());
			// Window will be created lazily on first frame
			*session = rt::g_session.handle;
			Logf("[OXRWXR] xrCreateSession: SUCCESS (D3D11, handle=%llu)", (unsigned long long)rt::g_session.handle);
			// Push READY event into queue
			rt::PushState(rt::g_session.handle, XR_SESSION_STATE_READY);
			return XR_SUCCESS;
		}
		else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
			const auto* b12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
			rt::g_session.usesD3D12 = true;
			rt::g_session.d3d12Device = b12->device;
			rt::g_session.d3d12Queue = b12->queue;
			rt::g_session.d3d11Device.Reset();
			rt::g_session.d3d11Context.Reset();
			rt::g_session.previewSwapchain.Reset();
			rt::g_session.handle = (XrSession)(uintptr_t)(0x1000 + sessionCount);
			*session = rt::g_session.handle;
			Logf("[OXRWXR] xrCreateSession: SUCCESS (D3D12, handle=%llu)", (unsigned long long)rt::g_session.handle);
			rt::PushState(rt::g_session.handle, XR_SESSION_STATE_READY);
			return XR_SUCCESS;
		}
		else if (entry->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
			const auto* bGL = reinterpret_cast<const XrGraphicsBindingOpenGLWin32KHR*>(entry);
			rt::g_session.usesOpenGL = true;
			rt::g_session.usesD3D12 = false;
			rt::g_session.glDC = bGL->hDC;
			rt::g_session.glRC = bGL->hGLRC;
			rt::g_session.d3d11Device.Reset();
			rt::g_session.d3d11Context.Reset();
			rt::g_session.d3d12Device.Reset();
			rt::g_session.d3d12Queue.Reset();
			rt::g_session.previewSwapchain.Reset();
			rt::g_session.handle = (XrSession)(uintptr_t)(0x1000 + sessionCount);
			*session = rt::g_session.handle;
			Logf("[OXRWXR] xrCreateSession: SUCCESS (OpenGL, handle=%llu, hDC=%p, hGLRC=%p)",
				(unsigned long long)rt::g_session.handle, bGL->hDC, bGL->hGLRC);
			rt::PushState(rt::g_session.handle, XR_SESSION_STATE_READY);
			return XR_SUCCESS;
		}
		entry = entry->next;
	}
	Log("[OXRWXR] xrCreateSession: ERROR - No supported graphics binding found (D3D11/D3D12/OpenGL)");
	return XR_ERROR_GRAPHICS_DEVICE_INVALID;
}


static XrResult XRAPI_PTR xrDestroySession_runtime(XrSession s) {
	Logf("[OXRWXR] xrDestroySession called (handle=%llu)", (unsigned long long)s);
	if (s != rt::g_session.handle) {
		Logf("[OXRWXR] xrDestroySession: ERROR - Invalid handle (expected %llu)",
			(unsigned long long)rt::g_session.handle);
		return XR_ERROR_HANDLE_INVALID;
	}

	// Transfer window and swapchain to global persistent storage
	// Unity likes to create/destroy sessions rapidly for compatibility checks
	{
		std::lock_guard<std::mutex> lock(rt::g_windowMutex);
		if (rt::g_session.hwnd && !rt::g_persistentWindow) {
			rt::g_persistentWindow = rt::g_session.hwnd;
			rt::g_persistentSwapchain = rt::g_session.previewSwapchain;
			rt::g_persistentWidth = rt::g_session.previewWidth;
			rt::g_persistentHeight = rt::g_session.previewHeight;
			Log("[OXRWXR] xrDestroySession: Preserving window and swapchain for next session");
		}
		else if (rt::g_session.hwnd == rt::g_persistentWindow) {
			// Already using persistent window, just update the swapchain
			rt::g_persistentSwapchain = rt::g_session.previewSwapchain;
			rt::g_persistentWidth = rt::g_session.previewWidth;
			rt::g_persistentHeight = rt::g_session.previewHeight;
			Log("[OXRWXR] xrDestroySession: Updating persistent swapchain");
		}
	}

	// Reset session but don't destroy the window
	rt::g_session.handle = XR_NULL_HANDLE;
	rt::g_session.state = XR_SESSION_STATE_IDLE;
	rt::g_session.d3d11Device.Reset();
	rt::g_session.d3d11Context.Reset();
	rt::g_session.usesD3D12 = false;
	rt::g_session.d3d12Device.Reset();
	rt::g_session.d3d12Queue.Reset();
	rt::ResetD3D12PreviewResources(rt::g_session);
	// Reset OpenGL state
	rt::g_session.usesOpenGL = false;
	rt::g_session.glDC = nullptr;
	rt::g_session.glRC = nullptr;
	rt::g_session.hwnd = nullptr;  // Clear from session but window still exists
	rt::g_session.previewWidth = 1920;
	rt::g_session.previewHeight = 540;
	rt::g_session.isFocused = false;
	Log("[OXRWXR] xrDestroySession: SUCCESS");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateSwapchainFormats_runtime(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats) {
	// OpenGL path - return GL internal formats
	if (rt::g_session.usesOpenGL) {
		const int64_t supportedFormats[] = {
			GL_SRGB8_ALPHA8,          // sRGB
			GL_RGBA8,                 // Standard RGBA
			GL_RGBA16F,               // HDR format
			GL_RGBA32F,               // High precision
			GL_RGB10_A2,              // HDR10 format
			GL_DEPTH_COMPONENT32F,    // Depth buffer
			GL_DEPTH24_STENCIL8,      // Depth + stencil
			GL_DEPTH_COMPONENT16      // 16-bit depth
		};
		const uint32_t formatCount = sizeof(supportedFormats) / sizeof(supportedFormats[0]);

		if (count) *count = formatCount;
		if (capacity > 0 && formats) {
			uint32_t copyCount = (capacity < formatCount) ? capacity : formatCount;
			for (uint32_t i = 0; i < copyCount; ++i) {
				formats[i] = supportedFormats[i];
			}
			Logf("[OXRWXR] xrEnumerateSwapchainFormats(OpenGL): Returned %u formats (first: 0x%X)", copyCount, (int)formats[0]);
		}
		return XR_SUCCESS;
	}

	// D3D11/D3D12 path - return DXGI formats
	const int64_t supportedFormats[] = {
		DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  // Unity often prefers sRGB
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  // UEVR uses this
		DXGI_FORMAT_B8G8R8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_FLOAT,   // HDR format
		DXGI_FORMAT_R32G32B32A32_FLOAT,   // High precision
		DXGI_FORMAT_R10G10B10A2_UNORM,    // HDR10 format
		DXGI_FORMAT_D32_FLOAT_S8X24_UINT, // UEVR's preferred depth format
		DXGI_FORMAT_D32_FLOAT,            // Depth buffer
		DXGI_FORMAT_D24_UNORM_S8_UINT,    // Depth + stencil
		DXGI_FORMAT_D16_UNORM             // 16-bit depth
	};
	const uint32_t formatCount = sizeof(supportedFormats) / sizeof(supportedFormats[0]);

	if (count) *count = formatCount;
	if (capacity > 0 && formats) {
		uint32_t copyCount = (capacity < formatCount) ? capacity : formatCount;
		for (uint32_t i = 0; i < copyCount; ++i) {
			formats[i] = supportedFormats[i];
		}
		Logf("[OXRWXR] xrEnumerateSwapchainFormats: Returned %u formats (first: %d)", copyCount, (int)formats[0]);
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateSwapchain_runtime(XrSession, const XrSwapchainCreateInfo* ci, XrSwapchain* sc) {
	Log("[OXRWXR] ============================================");
	Logf("[OXRWXR] xrCreateSwapchain called: format=%d, size=%ux%u, arraySize=%u, mipCount=%u, sampleCount=%u, usageFlags=0x%X",
		ci ? (int)ci->format : -1,
		ci ? ci->width : 0,
		ci ? ci->height : 0,
		ci ? ci->arraySize : 0,
		ci ? ci->mipCount : 0,
		ci ? ci->sampleCount : 0,
		ci ? ci->usageFlags : 0);

	// Log specific usage flags
	if (ci && ci->usageFlags) {
		if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
			Log("[OXRWXR]   - COLOR_ATTACHMENT");
		if (ci->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
			Log("[OXRWXR]   - DEPTH_STENCIL_ATTACHMENT");
		if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
			Log("[OXRWXR]   - UNORDERED_ACCESS");
		if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)
			Log("[OXRWXR]   - TRANSFER_SRC");
		if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)
			Log("[OXRWXR]   - TRANSFER_DST");
		if (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)
			Log("[OXRWXR]   - SAMPLED");
		if (ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
			Log("[OXRWXR]   - MUTABLE_FORMAT");
	}

	Log("[OXRWXR] ============================================");
	if (!ci || !sc) return XR_ERROR_VALIDATION_FAILURE;
	rt::Swapchain chain{};
	chain.handle = (XrSwapchain)(uintptr_t)(rt::g_swapchains.size() + 2);
	chain.format = (DXGI_FORMAT)ci->format;  // Store the original requested format
	chain.width = ci->width;
	chain.height = ci->height;
	chain.arraySize = ci->arraySize ? ci->arraySize : 1;
	chain.lastAcquired = UINT32_MAX;  // No image acquired yet
	chain.lastReleased = UINT32_MAX;  // No image released yet
	// Create textures on appropriate backend
	if (rt::g_session.usesD3D12) {
		chain.backend = rt::Swapchain::Backend::D3D12;
		chain.imageCount = 3;
		for (uint32_t i = 0; i < chain.imageCount; ++i) {
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			rd.Alignment = 0;
			rd.Width = chain.width;
			rd.Height = chain.height;
			rd.DepthOrArraySize = (UINT)chain.arraySize;
			rd.MipLevels = (UINT)(ci->mipCount ? ci->mipCount : 1);
			chain.mipCount = rd.MipLevels;
			rd.Format = chain.format;
			rd.SampleDesc.Count = (UINT)(ci->sampleCount ? ci->sampleCount : 1);
			rd.SampleDesc.Quality = 0;
			rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			rd.Flags = D3D12_RESOURCE_FLAG_NONE;
			if (!(chain.format == DXGI_FORMAT_D32_FLOAT || chain.format == DXGI_FORMAT_D24_UNORM_S8_UINT || chain.format == DXGI_FORMAT_D16_UNORM)) {
				if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
					rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
				if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
					rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			}
			D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_STATES init =
				(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
				? D3D12_RESOURCE_STATE_RENDER_TARGET
				: D3D12_RESOURCE_STATE_COMMON;
			ComPtr<ID3D12Resource> res;
			HRESULT hr = rt::g_session.d3d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init, nullptr, IID_PPV_ARGS(res.GetAddressOf()));
			if (FAILED(hr)) {
				Logf("[OXRWXR] CreateCommittedResource(D3D12)[%u] FAILED: 0x%08X", i, (unsigned)hr);
				return XR_ERROR_RUNTIME_FAILURE;
			}
			chain.images12.push_back(res);
			chain.imageStates12.push_back(init);
		}
		rt::g_swapchains.emplace(chain.handle, std::move(chain));
		*sc = chain.handle;
		Logf("[OXRWXR] xrCreateSwapchain(D3D12): sc=%p fmt=%d %ux%u array=%u samples=%u", *sc, (int)ci->format, ci->width, ci->height, ci->arraySize, ci->sampleCount);
		return XR_SUCCESS;
	}
	// OpenGL path
	if (rt::g_session.usesOpenGL) {
		chain.backend = rt::Swapchain::Backend::OpenGL;
		chain.imageCount = 3;

		// Check the current GL context state
		HGLRC currentRC = wglGetCurrentContext();
		HDC currentDC = wglGetCurrentDC();
		Logf("[OXRWXR] OpenGL swapchain: currentRC=%p, currentDC=%p, sessionRC=%p, sessionDC=%p",
			currentRC, currentDC, rt::g_session.glRC, rt::g_session.glDC);

		// If the app's context is already current, use it directly
		// Otherwise, switch to the app's context
		HGLRC prevRC = currentRC;
		HDC prevDC = currentDC;
		bool contextSwitched = false;

		if (currentRC != rt::g_session.glRC) {
			Log("[OXRWXR] Context mismatch - switching to app's context");
			if (!wglMakeCurrent(rt::g_session.glDC, rt::g_session.glRC)) {
				Logf("[OXRWXR] xrCreateSwapchain(OpenGL): wglMakeCurrent failed (error=%lu)", GetLastError());
				return XR_ERROR_RUNTIME_FAILURE;
			}
			contextSwitched = true;
		}
		else {
			Log("[OXRWXR] App's GL context is already current - good!");
		}

		// Log the GL version and renderer for debugging
		const char* glVersion = (const char*)glGetString(GL_VERSION);
		const char* glRenderer = (const char*)glGetString(GL_RENDERER);
		Logf("[OXRWXR] GL context: version=%s, renderer=%s",
			glVersion ? glVersion : "(null)", glRenderer ? glRenderer : "(null)");

		// Map format to OpenGL internal format and pixel format
		// The format from createInfo is a GL internal format (e.g. GL_RGBA8) when using OpenGL path
		auto GLFormatToPixelFormat = [](GLenum internalFmt) -> GLenum {
			switch (internalFmt) {
			case GL_SRGB8_ALPHA8:
			case GL_RGBA8:
				return GL_RGBA;
			case GL_RGBA16F:
			case GL_RGBA32F:
				return GL_RGBA;
			case GL_RGB10_A2:
				return GL_RGBA;
			case GL_DEPTH_COMPONENT32F:
			case GL_DEPTH_COMPONENT16:
				return GL_DEPTH_COMPONENT;
			case GL_DEPTH24_STENCIL8:
				return GL_DEPTH_STENCIL;
			default:
				return GL_RGBA;  // Default fallback
			}
			};

		// ci->format already contains the GL internal format
		GLenum glInternalFormat = (GLenum)ci->format;
		GLenum glFormat = GLFormatToPixelFormat(glInternalFormat);
		chain.glInternalFormat = glInternalFormat;

		bool isDepthFormat = (glInternalFormat == GL_DEPTH_COMPONENT32F ||
			glInternalFormat == GL_DEPTH24_STENCIL8 ||
			glInternalFormat == GL_DEPTH_COMPONENT16);

		// Load glTexImage3D if needed for array textures
		if (chain.arraySize > 1 && !EnsureGLTexImage3D()) {
			wglMakeCurrent(prevDC, prevRC);
			return XR_ERROR_RUNTIME_FAILURE;
		}

		// Create OpenGL textures
		for (uint32_t i = 0; i < chain.imageCount; ++i) {
			GLuint tex;
			glGenTextures(1, &tex);

			if (chain.arraySize > 1) {
				// Use texture array
				glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
				g_glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, glInternalFormat,
					chain.width, chain.height, chain.arraySize,
					0, glFormat, GL_UNSIGNED_BYTE, nullptr);
				glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
			}
			else {
				// Use regular 2D texture
				glBindTexture(GL_TEXTURE_2D, tex);
				if (isDepthFormat) {
					GLenum type = (glInternalFormat == GL_DEPTH24_STENCIL8) ? GL_UNSIGNED_INT_24_8 : GL_FLOAT;
					glTexImage2D(GL_TEXTURE_2D, 0, glInternalFormat,
						chain.width, chain.height, 0,
						glFormat, type, nullptr);
				}
				else {
					// DEBUG: Initialize with bright green to verify texture pipeline
					std::vector<uint8_t> initData(chain.width * chain.height * 4);
					for (size_t p = 0; p < initData.size(); p += 4) {
						initData[p + 0] = 0;    // R
						initData[p + 1] = 255;  // G - bright green
						initData[p + 2] = 0;    // B
						initData[p + 3] = 255;  // A
					}
					glTexImage2D(GL_TEXTURE_2D, 0, glInternalFormat,
						chain.width, chain.height, 0,
						GL_RGBA, GL_UNSIGNED_BYTE, initData.data());
					Logf("[OXRWXR] Initialized tex %u with GREEN data", tex);
				}
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glBindTexture(GL_TEXTURE_2D, 0);
			}

			GLenum err = glGetError();
			if (err != GL_NO_ERROR) {
				Logf("[OXRWXR] OpenGL texture creation error[%u]: 0x%X", i, err);
				if (contextSwitched) wglMakeCurrent(prevDC, prevRC);
				return XR_ERROR_RUNTIME_FAILURE;
			}

			// Verify the texture is valid
			GLboolean isValid = glIsTexture(tex);
			chain.imagesGL.push_back(tex);
			Logf("[OXRWXR] Created GL texture[%u]: %u (format=0x%X, valid=%d)", i, tex, glInternalFormat, isValid);

			// DEBUG: Immediately read back the texture to verify it has the correct content
			if (!isDepthFormat && EnsureGLFramebufferFuncs()) {
				GLuint verifyFBO = 0;
				g_glGenFramebuffers(1, &verifyFBO);
				g_glBindFramebuffer(GL_FRAMEBUFFER, verifyFBO);
				g_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

				if (g_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
					uint8_t pixel[4] = { 0 };
					glReadPixels(chain.width / 2, chain.height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
					Logf("[OXRWXR]   Immediate readback tex=%u: pixel=[%d,%d,%d,%d]",
						tex, pixel[0], pixel[1], pixel[2], pixel[3]);
				}
				else {
					Logf("[OXRWXR]   Immediate readback tex=%u: FBO incomplete", tex);
				}

				g_glBindFramebuffer(GL_FRAMEBUFFER, 0);
				g_glDeleteFramebuffers(1, &verifyFBO);
			}
		}

		// Restore previous context only if we switched
		if (contextSwitched) {
			Log("[OXRWXR] Restoring previous GL context");
			wglMakeCurrent(prevDC, prevRC);
		}

		rt::g_swapchains.emplace(chain.handle, std::move(chain));
		*sc = chain.handle;
		Logf("[OXRWXR] xrCreateSwapchain(OpenGL): sc=%p fmt=%d %ux%u array=%u imageCount=%u",
			*sc, (int)ci->format, ci->width, ci->height, ci->arraySize, chain.imageCount);
		return XR_SUCCESS;
	}
	// D3D11 path
	D3D11_TEXTURE2D_DESC td{};

	// Determine if this is a depth format
	bool isDepthFormat = (chain.format == DXGI_FORMAT_D32_FLOAT ||
		chain.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
		chain.format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
		chain.format == DXGI_FORMAT_D16_UNORM);

	// For depth formats that need to be sampled, we must use typeless format
	bool needsTypelessDepth = isDepthFormat && (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT);

	// Convert depth format to typeless when sampling is needed
	auto ToTypelessDepth = [](DXGI_FORMAT fmt) -> DXGI_FORMAT {
		switch (fmt) {
		case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
		case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
		case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
		default: return fmt;
		}
		};

	// Use typeless format for color textures to allow both UNORM and SRGB views
	// Use typeless format for depth textures when sampling is needed
	if (isDepthFormat) {
		td.Format = needsTypelessDepth ? ToTypelessDepth(chain.format) : chain.format;
	}
	else {
		td.Format = ToTypeless(chain.format);
	}

	td.Width = chain.width;
	td.Height = chain.height;
	td.ArraySize = chain.arraySize ? chain.arraySize : 1;  // Ensure at least 1
	td.MipLevels = ci->mipCount ? ci->mipCount : 1;  // Ensure at least 1
	chain.mipCount = td.MipLevels;
	td.SampleDesc.Count = ci->sampleCount ? ci->sampleCount : 1;
	td.SampleDesc.Quality = 0;  // Must be 0 for non-MSAA
	// Set bind flags based on format type

	if (isDepthFormat) {
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		// Typeless depth formats can also be shader resources
		if (needsTypelessDepth) {
			td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
		}
	}
	else {
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		// Add unordered access if requested
		if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) {
			td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
		}
	}

	td.Usage = D3D11_USAGE_DEFAULT;
	td.CPUAccessFlags = 0;

	// Set MiscFlags based on what Unity might need
	td.MiscFlags = 0;

	// Log the texture description for debugging
	Logf("[OXRWXR] Creating swapchain textures: Format=%d, %ux%u, Array=%u, Mips=%u, Samples=%u",
		td.Format, td.Width, td.Height, td.ArraySize, td.MipLevels, td.SampleDesc.Count);
	chain.imageCount = 3;
	for (uint32_t i = 0; i < chain.imageCount; ++i) {
		ComPtr<ID3D11Texture2D> tex;
		HRESULT hr = rt::g_session.d3d11Device->CreateTexture2D(&td, nullptr, tex.GetAddressOf());
		if (FAILED(hr)) {
			Logf("[OXRWXR] CreateTexture2D[%u] FAILED: hr=0x%08X", i, (unsigned)hr);
			Logf("[OXRWXR]   Format=%d, Size=%ux%u, Array=%u, Mips=%u, Samples=%u, BindFlags=0x%X",
				td.Format, td.Width, td.Height, td.ArraySize, td.MipLevels, td.SampleDesc.Count, td.BindFlags);

			// Try to provide more specific error info
			if (hr == E_INVALIDARG) {
				Log("[OXRWXR]   ERROR: E_INVALIDARG - Invalid texture parameters");
				// Check common issues
				if (td.ArraySize == 0) Log("[OXRWXR]   - ArraySize is 0");
				if (td.Width == 0 || td.Height == 0) Log("[OXRWXR]   - Invalid dimensions");
				if (td.MipLevels == 0) Log("[OXRWXR]   - MipLevels is 0");
			}
			return XR_ERROR_RUNTIME_FAILURE;
		}
		Logf("[OXRWXR] Created swapchain texture[%u]: %p", i, tex.Get());
		chain.images.push_back(std::move(tex));
	}
	rt::g_swapchains.emplace(chain.handle, std::move(chain));
	*sc = chain.handle;
	Logf("[OXRWXR] xrCreateSwapchain: sc=%p fmt=%d %ux%u array=%u samples=%u", *sc, (int)ci->format, ci->width, ci->height, ci->arraySize, ci->sampleCount);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateSwapchainImages_runtime(XrSwapchain sc, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
	auto it = rt::g_swapchains.find(sc); if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
	if (it->second.backend == rt::Swapchain::Backend::D3D12) {
		const uint32_t n = (uint32_t)it->second.images12.size();
		if (count) *count = n;
		if (capacity >= n && images) {
			auto* arr = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
			for (uint32_t i = 0; i < n; ++i) { arr[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR; arr[i].texture = it->second.images12[i].Get(); }
		}
		Logf("[OXRWXR] xrEnumerateSwapchainImages(D3D12): sc=%p count=%u", sc, n);
		return XR_SUCCESS;
	}
	else if (it->second.backend == rt::Swapchain::Backend::OpenGL) {
		const uint32_t n = (uint32_t)it->second.imagesGL.size();
		if (count) *count = n;
		if (capacity >= n && images) {
			auto* arr = reinterpret_cast<XrSwapchainImageOpenGLKHR*>(images);
			for (uint32_t i = 0; i < n; ++i) {
				arr[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
				arr[i].image = it->second.imagesGL[i];
			}
			// DEBUG: Log the texture IDs being returned AND verify content still matches
			Logf("[OXRWXR] xrEnumerateSwapchainImages(OpenGL): sc=%p texIDs=[%u,%u,%u]",
				sc, n > 0 ? arr[0].image : 0, n > 1 ? arr[1].image : 0, n > 2 ? arr[2].image : 0);

			// DEBUG: Read first texture to verify it still has content
			if (n > 0 && EnsureGLFramebufferFuncs()) {
				GLuint checkTex = arr[0].image;
				GLuint checkFBO = 0;
				g_glGenFramebuffers(1, &checkFBO);
				g_glBindFramebuffer(GL_FRAMEBUFFER, checkFBO);
				g_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, checkTex, 0);
				if (g_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
					uint8_t pixel[4] = { 0 };
					glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
					Logf("[OXRWXR]   After enumerate: tex=%u pixel=[%d,%d,%d,%d]",
						checkTex, pixel[0], pixel[1], pixel[2], pixel[3]);
				}
				g_glBindFramebuffer(GL_FRAMEBUFFER, 0);
				g_glDeleteFramebuffers(1, &checkFBO);
			}
		}
		else {
			Logf("[OXRWXR] xrEnumerateSwapchainImages(OpenGL): sc=%p count=%u (query only)", sc, n);
		}
		return XR_SUCCESS;
	}
	else {
		const uint32_t n = (uint32_t)it->second.images.size();
		if (count) *count = n;
		if (capacity >= n && images) {
			auto* arr = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
			for (uint32_t i = 0; i < n; ++i) { arr[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR; arr[i].texture = it->second.images[i].Get(); }
		}
		Logf("[OXRWXR] xrEnumerateSwapchainImages(D3D11): sc=%p count=%u", sc, n);
		return XR_SUCCESS;
	}
}

static XrResult XRAPI_PTR xrAcquireSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
	auto it = rt::g_swapchains.find(sc); if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
	auto& ch = it->second;
	uint32_t i = ch.nextIndex;
	ch.nextIndex = (ch.nextIndex + 1) % ch.imageCount;
	ch.lastAcquired = i;  // Track what we just gave to the app
	if (index) *index = i;

	static int acquireCount = 0;
	if (++acquireCount % 60 == 1) {  // Log every 60 calls
		Logf("[OXRWXR] xrAcquireSwapchainImage: sc=%p idx=%u (format=%d, %ux%u)",
			sc, i, (int)ch.format, ch.width, ch.height);
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrWaitSwapchainImage_runtime(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
static XrResult XRAPI_PTR xrReleaseSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageReleaseInfo*) {
	auto it = rt::g_swapchains.find(sc);
	if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
	auto& ch = it->second;
	// The app just released the image it acquired earlier
	ch.lastReleased = ch.lastAcquired;

	static int releaseCount = 0;
	bool shouldLog = (++releaseCount <= 10);
	if (shouldLog || releaseCount % 60 == 1) {
		Logf("[OXRWXR] xrReleaseSwapchainImage: sc=%p released=%u", sc, ch.lastReleased);

		// DEBUG: Read texture content at release time to verify it has content
		if (ch.backend == rt::Swapchain::Backend::OpenGL && !ch.imagesGL.empty() && ch.lastReleased < ch.imagesGL.size()) {
			GLuint glTex = ch.imagesGL[ch.lastReleased];

			// Check and log current GL context
			HGLRC currentRC = wglGetCurrentContext();
			HDC currentDC = wglGetCurrentDC();
			Logf("[OXRWXR]   At release: currentRC=%p, currentDC=%p, sessionRC=%p, sessionDC=%p",
				currentRC, currentDC, rt::g_session.glRC, rt::g_session.glDC);

			// Ensure the session's GL context is current before reading
			HGLRC savedRC = currentRC;
			HDC savedDC = currentDC;
			if (currentRC != rt::g_session.glRC && rt::g_session.glRC && rt::g_session.glDC) {
				Logf("[OXRWXR]   At release: Switching to session GL context");
				wglMakeCurrent(rt::g_session.glDC, rt::g_session.glRC);
			}

			// Read a single pixel to see what's there
			glFinish();  // Make sure rendering is done

			// Try memory barrier to ensure texture writes are visible
			// GL_TEXTURE_UPDATE_BARRIER_BIT = 0x00000100
			// GL_FRAMEBUFFER_BARRIER_BIT = 0x00000400
			// GL_ALL_BARRIER_BITS = 0xFFFFFFFF
			typedef void (APIENTRY* PFNGLMEMORYBARRIERPROC)(GLbitfield);
			static PFNGLMEMORYBARRIERPROC glMemoryBarrier = nullptr;
			if (!glMemoryBarrier) {
				glMemoryBarrier = (PFNGLMEMORYBARRIERPROC)wglGetProcAddress("glMemoryBarrier");
			}
			if (glMemoryBarrier) {
				glMemoryBarrier(0xFFFFFFFF);  // GL_ALL_BARRIER_BITS
			}

			// Clear any pending GL errors
			while (glGetError() != GL_NO_ERROR) {}

			// Check if texture is valid
			GLboolean isValidTex = glIsTexture(glTex);

			// First, check what FBO is currently bound (might be game's FBO)
			GLint currentBoundFBO = 0;
			glGetIntegerv(0x8CA6, &currentBoundFBO);  // GL_FRAMEBUFFER_BINDING = 0x8CA6

			// Read from current FBO (game's)
			uint8_t currentFboPixel[4] = { 0 };
			if (currentBoundFBO != 0) {
				glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, currentFboPixel);
			}

			GLenum glError1 = glGetError();

			// Method 1: Read via new FBO attached to swapchain texture
			if (EnsureGLFramebufferFuncs()) {
				GLuint readFBO = 0;
				g_glGenFramebuffers(1, &readFBO);
				g_glBindFramebuffer(GL_FRAMEBUFFER, readFBO);
				g_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);

				uint8_t fboPixel[4] = { 0 };
				if (g_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
					glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fboPixel);
				}

				g_glBindFramebuffer(GL_FRAMEBUFFER, 0);
				g_glDeleteFramebuffers(1, &readFBO);

				// Check if texture exists and has expected properties
				GLint texWidth = 0, texHeight = 0, texFormat = 0;
				glBindTexture(GL_TEXTURE_2D, glTex);
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texWidth);
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texHeight);
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &texFormat);

				// Method 2: Read via glGetTexImage (need full buffer for entire texture)
				uint8_t texPixel[4] = { 0 };
				if (texWidth > 0 && texHeight > 0) {
					size_t bufSize = (size_t)texWidth * texHeight * 4;
					if (bufSize <= 16 * 1024 * 1024) {  // Limit to 16MB
						std::vector<uint8_t> texData(bufSize);
						glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texData.data());
						// Get first pixel
						texPixel[0] = texData[0];
						texPixel[1] = texData[1];
						texPixel[2] = texData[2];
						texPixel[3] = texData[3];
					}
				}
				glBindTexture(GL_TEXTURE_2D, 0);

				GLenum glError2 = glGetError();
				Logf("[OXRWXR]   At release: tex=%u isValid=%d err1=0x%X err2=0x%X boundFBO=%d currPx=[%d,%d,%d,%d] newPx=[%d,%d,%d,%d] texPx=[%d,%d,%d,%d] size=%dx%d fmt=0x%X",
					glTex, (int)isValidTex, glError1, glError2, currentBoundFBO,
					currentFboPixel[0], currentFboPixel[1], currentFboPixel[2], currentFboPixel[3],
					fboPixel[0], fboPixel[1], fboPixel[2], fboPixel[3],
					texPixel[0], texPixel[1], texPixel[2], texPixel[3],
					texWidth, texHeight, texFormat);
			}

			// Restore original GL context if we switched
			if (savedRC != rt::g_session.glRC && savedRC && savedDC) {
				wglMakeCurrent(savedDC, savedRC);
			}
		}
	}
	return XR_SUCCESS;
}

// Helper struct to save and restore D3D11 context state using RAII
struct D3D11StateBackup {
	D3D11StateBackup(ID3D11DeviceContext* ctx) : ctx_(ctx) {
		// IA
		ctx_->IAGetInputLayout(&ia_input_layout);
		ctx_->IAGetPrimitiveTopology(&ia_primitive_topology);
		// RS
		rs_num_viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ctx_->RSGetViewports(&rs_num_viewports, rs_viewports);
		rs_num_scissor_rects = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ctx_->RSGetScissorRects(&rs_num_scissor_rects, rs_scissor_rects);
		ctx_->RSGetState(&rs_state);
		// OM
		ctx_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, om_rtvs, &om_dsv);
		ctx_->OMGetBlendState(&om_blend_state, om_blend_factor, &om_sample_mask);
		ctx_->OMGetDepthStencilState(&om_depth_stencil_state, &om_stencil_ref);
		// Shaders - MUST initialize class instance counts before calling GetShader
		ps_num_class_instances = 256;  // Initialize to array capacity
		ctx_->PSGetShader(&ps_shader, ps_class_instances, &ps_num_class_instances);
		ctx_->PSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, ps_samplers);
		ctx_->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, ps_srvs);
		vs_num_class_instances = 256;  // Initialize to array capacity
		ctx_->VSGetShader(&vs_shader, vs_class_instances, &vs_num_class_instances);
	}

	~D3D11StateBackup() {
		// Restore state
		ctx_->IASetInputLayout(ia_input_layout);
		ctx_->IASetPrimitiveTopology(ia_primitive_topology);
		ctx_->RSSetViewports(rs_num_viewports, rs_viewports);
		ctx_->RSSetScissorRects(rs_num_scissor_rects, rs_scissor_rects);
		ctx_->RSSetState(rs_state);
		ctx_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, om_rtvs, om_dsv);
		ctx_->OMSetBlendState(om_blend_state, om_blend_factor, om_sample_mask);
		ctx_->OMSetDepthStencilState(om_depth_stencil_state, om_stencil_ref);
		ctx_->PSSetShader(ps_shader, ps_class_instances, ps_num_class_instances);
		ctx_->PSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, ps_samplers);
		ctx_->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, ps_srvs);
		ctx_->VSSetShader(vs_shader, vs_class_instances, vs_num_class_instances);

		// Release COM references
		if (ia_input_layout) ia_input_layout->Release();
		if (rs_state) rs_state->Release();
		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (om_rtvs[i]) om_rtvs[i]->Release();
		if (om_dsv) om_dsv->Release();
		if (om_blend_state) om_blend_state->Release();
		if (om_depth_stencil_state) om_depth_stencil_state->Release();
		if (ps_shader) ps_shader->Release();
		for (UINT i = 0; i < ps_num_class_instances; ++i) if (ps_class_instances[i]) ps_class_instances[i]->Release();
		for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i) if (ps_samplers[i]) ps_samplers[i]->Release();
		for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) if (ps_srvs[i]) ps_srvs[i]->Release();
		if (vs_shader) vs_shader->Release();
		for (UINT i = 0; i < vs_num_class_instances; ++i) if (vs_class_instances[i]) vs_class_instances[i]->Release();
	}

private:
	ID3D11DeviceContext* ctx_;
	// IA State
	ID3D11InputLayout* ia_input_layout = nullptr;
	D3D11_PRIMITIVE_TOPOLOGY ia_primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	// RS State  
	UINT rs_num_viewports = 0, rs_num_scissor_rects = 0;
	D3D11_VIEWPORT rs_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	D3D11_RECT rs_scissor_rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	ID3D11RasterizerState* rs_state = nullptr;
	// OM State
	ID3D11RenderTargetView* om_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
	ID3D11DepthStencilView* om_dsv = nullptr;
	ID3D11BlendState* om_blend_state = nullptr;
	FLOAT om_blend_factor[4] = { 0.0f };
	UINT om_sample_mask = 0;
	ID3D11DepthStencilState* om_depth_stencil_state = nullptr;
	UINT om_stencil_ref = 0;
	// PS State
	ID3D11PixelShader* ps_shader = nullptr;
	ID3D11ClassInstance* ps_class_instances[256] = { nullptr };
	UINT ps_num_class_instances = 0;
	ID3D11SamplerState* ps_samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = { nullptr };
	ID3D11ShaderResourceView* ps_srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = { nullptr };
	// VS State
	ID3D11VertexShader* vs_shader = nullptr;
	ID3D11ClassInstance* vs_class_instances[256] = { nullptr };
	UINT vs_num_class_instances = 0;
};

namespace rt {
	static XrSessionState g_state = XR_SESSION_STATE_IDLE;
	static std::vector<XrEventDataBuffer> g_eventQueue;
	void PushState(XrSession s, XrSessionState ns) {
		g_state = ns;
		g_session.state = ns;
		const char* stateName = "UNKNOWN";
		switch (ns) {
		case XR_SESSION_STATE_IDLE: stateName = "IDLE"; break;
		case XR_SESSION_STATE_READY: stateName = "READY"; break;
		case XR_SESSION_STATE_SYNCHRONIZED: stateName = "SYNCHRONIZED"; break;
		case XR_SESSION_STATE_VISIBLE: stateName = "VISIBLE"; break;
		case XR_SESSION_STATE_FOCUSED: stateName = "FOCUSED"; break;
		case XR_SESSION_STATE_STOPPING: stateName = "STOPPING"; break;
		case XR_SESSION_STATE_LOSS_PENDING: stateName = "LOSS_PENDING"; break;
		case XR_SESSION_STATE_EXITING: stateName = "EXITING"; break;
		}
		Logf("[OXRWXR] PushState: Session %llu -> %s", (unsigned long long)s, stateName);

		XrEventDataSessionStateChanged e{ XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
		e.session = s; e.state = ns; e.time = 0;

		XrEventDataBuffer buf{};
		buf.type = XR_TYPE_EVENT_DATA_BUFFER;  // Set the base type
		std::memcpy(&buf, &e, sizeof(e));
		g_eventQueue.push_back(buf);
		Logf("[OXRWXR] Event queue now has %zu events", g_eventQueue.size());
	}
}
static XrResult XRAPI_PTR xrPollEvent_runtime(XrInstance, XrEventDataBuffer* b) {
	static int pollCount = 0;
	pollCount++;

	if (pollCount <= 5) {  // Log first few polls
		Logf("[OXRWXR] xrPollEvent called (#%d), queue size=%zu", pollCount, rt::g_eventQueue.size());
	}

	if (!b) return XR_ERROR_VALIDATION_FAILURE;
	if (rt::g_eventQueue.empty()) {
		if (pollCount <= 5) {
			Log("[OXRWXR] xrPollEvent: No events available (XR_EVENT_UNAVAILABLE)");
		}
		return XR_EVENT_UNAVAILABLE;
	}
	*b = rt::g_eventQueue.front();
	rt::g_eventQueue.erase(rt::g_eventQueue.begin());

	// Log what event we're delivering
	const XrEventDataBaseHeader* header = reinterpret_cast<const XrEventDataBaseHeader*>(b);
	if (header->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
		const XrEventDataSessionStateChanged* stateEvent = reinterpret_cast<const XrEventDataSessionStateChanged*>(b);
		const char* stateName = "UNKNOWN";
		switch (stateEvent->state) {
		case XR_SESSION_STATE_IDLE: stateName = "IDLE"; break;
		case XR_SESSION_STATE_READY: stateName = "READY"; break;
		case XR_SESSION_STATE_SYNCHRONIZED: stateName = "SYNCHRONIZED"; break;
		case XR_SESSION_STATE_VISIBLE: stateName = "VISIBLE"; break;
		case XR_SESSION_STATE_FOCUSED: stateName = "FOCUSED"; break;
		case XR_SESSION_STATE_STOPPING: stateName = "STOPPING"; break;
		case XR_SESSION_STATE_LOSS_PENDING: stateName = "LOSS_PENDING"; break;
		case XR_SESSION_STATE_EXITING: stateName = "EXITING"; break;
		}
		Logf("[OXRWXR] xrPollEvent: Delivering SESSION_STATE_CHANGED -> %s (session=%llu, %zu events left)",
			stateName, (unsigned long long)stateEvent->session, rt::g_eventQueue.size());
	}
	else {
		Logf("[OXRWXR] xrPollEvent: Delivering event type %d (%zu events left)", header->type, rt::g_eventQueue.size());
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrBeginSession_runtime(XrSession s, const XrSessionBeginInfo*) {
	Log("[OXRWXR] ============================================");
	Logf("[OXRWXR] xrBeginSession called (session=%llu)", (unsigned long long)s);
	Log("[OXRWXR] Session started - moving to SYNCHRONIZED/VISIBLE states");
	Log("[OXRWXR] ============================================");
	rt::PushState(s, XR_SESSION_STATE_SYNCHRONIZED);
	rt::PushState(s, XR_SESSION_STATE_VISIBLE);
	// Only push FOCUSED if window is actually active/focused
	if (rt::g_session.hwnd && rt::g_session.isFocused) {
		rt::PushState(s, XR_SESSION_STATE_FOCUSED);
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrEndSession_runtime(XrSession s) { Log("[OXRWXR] xrEndSession"); rt::PushState(s, XR_SESSION_STATE_STOPPING); rt::PushState(s, XR_SESSION_STATE_IDLE); return XR_SUCCESS; }
static XrResult XRAPI_PTR xrRequestExitSession_runtime(XrSession s) { rt::PushState(s, XR_SESSION_STATE_EXITING); return XR_SUCCESS; }
static XrResult XRAPI_PTR xrWaitFrame_runtime(XrSession, const XrFrameWaitInfo*, XrFrameState* s) {
	if (!s) return XR_ERROR_VALIDATION_FAILURE;
	// Message pump so the preview window stays responsive
	MSG msg; while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
	static LARGE_INTEGER freq = []() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
	static double periodSec = 1.0 / 90.0;
	static long long periodNs = (long long)(periodSec * 1e9);
	static double nextTick = []() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart; }();

	//----------------
	//OXRWXR CHANGE:
	//---------------- 
	// Now we pass 6DOF data always
	std::string txt = udpReader->GetRetData();

	//Example return data:
	//client0 0.213 0.287 -0.933 0.035 0.0 0.0 -0.008 -0.229 -0.173 0.095 -0.296 0.947 -0.077 0.0 0.0 0.154 -0.240 -0.140 0.146 -0.072 0.048 0.985 0.037 0.006 -0.017 0.0678 99.00 103.40 224 TFFFFFFFFFTTTFFFFFT

	std::istringstream iss(txt);
	std::string client;
	std::vector<float> floats(28);
	int openXRFrameID;
	std::string buttonString;

	std::locale c_locale("C");
	iss.imbue(c_locale);

	// Parse client string
	iss >> client;

	// Parse float values
	for (auto& f : floats) {
		iss >> f;
	}

	// Parse integer value and last string
	iss >> openXRFrameID >> buttonString; //>> hmdString;

	OpenXRFrameID = openXRFrameID;

	//Logf("UDP string: %s", txt.c_str());

	std::vector<bool> buttonBools;

	if (buttonString.empty()) {
		buttonString = "FFFFFFFFFFFFFFFFFFF";
	}

	for (char c : buttonString) {
		if (c == 'F') {
			buttonBools.push_back(false);
		}
		else if (c == 'T') {
			buttonBools.push_back(true);
		}
	}

	//FLOATS:
	//Left Hand Quaternion X, Left Hand Quaternion Y, Left Hand Quaternion Z, Left Hand Quaternion W, Left Hand Thumbstick X, Left Hand Thumbstick Y, 
	//Left Hand X Position, Left Hand Y Position, Left Hand Z Position,
	//Right Hand Quaternion X, Right Hand Quaternion Y, Right Hand Quaternion Z, Right Hand Quaternion W, Right Hand Thumbstick X, Right Hand Thumbstick Y, 
	//Right Hand X Position, Right Hand Y Position, Right Hand Z Position,
	//HMD Quaternion X, HMD Quaternion Y, HMD Quaternion Z, HMD Quaternion W, HMD X Position, HMD Y position, HMD Z Position, 
	//Current IPD, Current FOV Horizontal, Current FOV Vertical, XR Frame ID, Button String

	//BUTTONS:
	//L_GRIP, L_MENU, L_THUMBSTICK_PRESS, L_THUMBSTICK_LEFT, L_THUMBSTICK_RIGHT, L_THUMBSTICK_UP, L_THUMBSTICK_DOWN, L_TRIGGER, L_X, L_Y, 
	//R_A, R_B, R_GRIP, R_THUMBSTICK_PRESS, R_THUMBSTICK_LEFT, R_THUMBSTICK_RIGHT, R_THUMBSTICK_UP, R_THUMBSTICK_DOWN, R_TRIGGER

	LHandQuat = makeXrVector4f(floats[0], floats[1], floats[2], floats[3]);
	LHandPos = makeXrVector3f(floats[6], floats[7], floats[8]);
	LThumbstick = makeXrVector2f(floats[4], floats[5]);

	RHandQuat = makeXrVector4f(floats[9], floats[10], floats[11], floats[12]);
	RHandPos = makeXrVector3f(floats[15], floats[16], floats[17]);
	RThumbstick = makeXrVector2f(floats[13], floats[14]);

	HMDQuat = makeXrVector4f(floats[18], floats[19], floats[20], floats[21]);
	HMDPos = makeXrVector3f(floats[22], floats[23], floats[24]);

	IPDVal = floats[25];
	FOVH = (floats[26] + 30.0f); // * 0.80f;
	FOVV = (floats[27] - 20.0f); // * 0.80f;

	//Alternate attempt at FOV manipulation
	FOVH = (fovVarA * floats[26]) + fovVarB;
	FOVV = (fovVarC * floats[27]) + fovVarD;

	FOVTotal = FOVH / FOVV;

	LTrigger = buttonBools[7];
	LGrip = buttonBools[0];
	LClick = buttonBools[2];
	RTrigger = buttonBools[18];
	RGrip = buttonBools[12];
	RClick = buttonBools[13];
	L_X = buttonBools[8];
	L_Y = buttonBools[9];
	R_A = buttonBools[10];
	R_B = buttonBools[11];
	L_Menu = buttonBools[1];

	R_ThumbUp = buttonBools[16];
	R_ThumbDown = buttonBools[17];

	//PICO and Quest 2 are both tested to need the hand orientation flipped, assume the same for Quest Pro for now
	if (hmdMake == "PICO" || hmdModel == "QUEST 2" || hmdMake == "PLAY FOR DREAM" || hmdMake == "FORCE FIX HANDS") {
		UpsideDownHandsFix = true;
	}
	else {
		UpsideDownHandsFix = false;
	}

	//Brute force melee gesture logging
	//prevLHandPos[lastPosFrame] = LHandPos;
	//prevRHandPos[lastPosFrame] = RHandPos;

	lastPosFrame += 1;
	if (lastPosFrame > 4) {
		lastPosFrame = 0;
	}


	//rt::g_headRoll += moveSpeed * deltaTime;


	// Handle WASD keyboard input for movement (relative to head orientation)
	//if (rt::g_session.isFocused) {
	const float moveSpeed = 3.0f;  // meters per second
	float deltaTime = (float)periodSec;

	//----------------
	//OXRWXR CHANGE:
	//---------------- 
	// Update position and rotation of the head
	/*XrVector4f quat = XrVector4f{ HMDQuat.x, HMDQuat.y, -HMDQuat.z, HMDQuat.w };

	auto quatToEuler = [](const XrVector4f& q) {
		float roll = atan2f(2 * (q.w * q.x + q.y * q.z), 1 - 2 * (q.x * q.x + q.y * q.y));
		float pitch = asinf(std::max(-1.0f, std::min(1.0f, 2 * (q.w * q.y - q.z * q.x))));
		float yaw = atan2f(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z));
		return std::make_tuple(yaw, pitch, roll);
		};*/

	//std::tie(rt::g_headPitch, rt::g_headYaw, rt::g_headRoll) = quatToEuler(quat);

	//XrQuaternionf headQ = { quat.x, quat.y, quat.z, quat.w }; //rt::QuatFromYawPitchRoll(rt::g_headYaw, rt::g_headPitch, rt::g_headRoll);
	//XrVector3f fwd = rt::RotateVectorByQuaternion(headQ, XrVector3f{ 0.0f, 0.0f, -1.0f });
	//XrVector3f right = rt::RotateVectorByQuaternion(headQ, XrVector3f{ 1.0f, 0.0f, 0.0f });

	rt::g_headPos = HMDPos;

	/*if (GetAsyncKeyState('W') & 0x8000) {
		rt::g_headPos.x += fwd.x * moveSpeed * deltaTime;
		rt::g_headPos.y += fwd.y * moveSpeed * deltaTime;
		rt::g_headPos.z += fwd.z * moveSpeed * deltaTime;
	}
	if (GetAsyncKeyState('S') & 0x8000) {
		rt::g_headPos.x -= fwd.x * moveSpeed * deltaTime;
		rt::g_headPos.y -= fwd.y * moveSpeed * deltaTime;
		rt::g_headPos.z -= fwd.z * moveSpeed * deltaTime;
	}
	if (GetAsyncKeyState('A') & 0x8000) {
		rt::g_headPos.x -= right.x * moveSpeed * deltaTime;
		rt::g_headPos.y -= right.y * moveSpeed * deltaTime;
		rt::g_headPos.z -= right.z * moveSpeed * deltaTime;
	}
	if (GetAsyncKeyState('D') & 0x8000) {
		rt::g_headPos.x += right.x * moveSpeed * deltaTime;
		rt::g_headPos.y += right.y * moveSpeed * deltaTime;
		rt::g_headPos.z += right.z * moveSpeed * deltaTime;
	}*/
	if (GetAsyncKeyState('Q') & 0x8000) {
		rt::g_headPos.y -= moveSpeed * deltaTime;
	}
	if (GetAsyncKeyState('E') & 0x8000) {
		rt::g_headPos.y += moveSpeed * deltaTime;
	}

	// ========================================
	// Automatic Controller Animation Mode
	// ========================================
	// Press 'M' to toggle automatic motion - controller moves in a pattern
	static bool autoMotionEnabled = true;  // Start with auto motion ON for testing
	static bool mKeyWasPressed = false;
	static float animTime = 0.0f;

	bool mKeyPressed = (GetAsyncKeyState('M') & 0x8000) != 0;
	if (mKeyPressed && !mKeyWasPressed) {
		autoMotionEnabled = !autoMotionEnabled;
		Logf("[OXRWXR] Auto motion %s", autoMotionEnabled ? "ENABLED" : "DISABLED");
	}
	mKeyWasPressed = mKeyPressed;

	if (autoMotionEnabled) {
		animTime += deltaTime;

		// Move controller in a figure-8 pattern around the camera
		// X: side to side (left/right)
		// Y: up and down
		// Z: forward and back
		float radius = 0.4f;  // 40cm radius of motion
		float speed = 0.5f;   // Complete cycle every ~12 seconds

		// Figure-8 pattern
		rt::g_rightController.posOffset.x = radius * sinf(animTime * speed * 2.0f);
		rt::g_rightController.posOffset.y = -0.2f + 0.3f * sinf(animTime * speed);  // Oscillate between -0.5 and +0.1
		rt::g_rightController.posOffset.z = -0.4f + radius * sinf(animTime * speed) * cosf(animTime * speed);

		// Also rotate the controller
		rt::g_rightController.yawOffset = 0.5f * sinf(animTime * speed * 1.5f);
		rt::g_rightController.pitchOffset = -0.3f + 0.3f * cosf(animTime * speed);
	}

	// Right controller manipulation (numpad keys) - only when auto motion is off
	// Numpad 8/2: Move controller forward/back (in local space)
	// Numpad 4/6: Move controller left/right (in local space)
	// Numpad +/-: Move controller up/down
	// Numpad 7/9: Rotate controller yaw
	// Numpad 1/3: Rotate controller pitch
	const float ctrlMoveSpeed = 0.5f * deltaTime;
	const float ctrlRotSpeed = 1.0f * deltaTime;

	if (!autoMotionEnabled) {
		if (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) {
			rt::g_rightController.posOffset.z -= ctrlMoveSpeed;  // Forward
		}
		if (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) {
			rt::g_rightController.posOffset.z += ctrlMoveSpeed;  // Back
		}
		if (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) {
			rt::g_rightController.posOffset.x -= ctrlMoveSpeed;  // Left
		}
		if (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) {
			rt::g_rightController.posOffset.x += ctrlMoveSpeed;  // Right
		}
		if (GetAsyncKeyState(VK_ADD) & 0x8000) {
			rt::g_rightController.posOffset.y += ctrlMoveSpeed;  // Up
		}
		if (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) {
			rt::g_rightController.posOffset.y -= ctrlMoveSpeed;  // Down
		}
		if (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) {
			rt::g_rightController.yawOffset -= ctrlRotSpeed;  // Rotate left
		}
		if (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) {
			rt::g_rightController.yawOffset += ctrlRotSpeed;  // Rotate right
		}
		if (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) {
			rt::g_rightController.pitchOffset += ctrlRotSpeed;  // Pitch down
		}
		if (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) {
			rt::g_rightController.pitchOffset -= ctrlRotSpeed;  // Pitch up
		}
	}
	// Numpad 5: Reset controller to default position
	if (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) {
		rt::g_rightController.posOffset = { 0.2f, -0.3f, -0.4f };
		rt::g_rightController.yawOffset = 0.0f;
		rt::g_rightController.pitchOffset = -0.3f;
		autoMotionEnabled = false;
	}

	// ========================================
	// Controller Button/Trigger Emulation
	// ========================================
	// Right controller buttons (main hand for most games)
	// Space = Trigger (fire weapon)
	// F = Grip (grab)
	// Tab = Menu
	// R = Primary button (A)
	// T = Secondary button (B)
	// Enter = Thumbstick click
	rt::g_rightController.triggerPressed = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
	rt::g_rightController.triggerValue = rt::g_rightController.triggerPressed ? 1.0f : 0.0f;
	rt::g_rightController.gripPressed = (GetAsyncKeyState('F') & 0x8000) != 0;
	rt::g_rightController.gripValue = rt::g_rightController.gripPressed ? 1.0f : 0.0f;
	rt::g_rightController.menuPressed = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
	rt::g_rightController.primaryPressed = (GetAsyncKeyState('R') & 0x8000) != 0;
	rt::g_rightController.secondaryPressed = (GetAsyncKeyState('T') & 0x8000) != 0;
	rt::g_rightController.thumbstickPressed = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;

	// Left controller buttons (off-hand)
	// Left Ctrl = Trigger
	// Left Alt = Grip
	// G = Menu (left hand)
	// V = Primary (X)
	// B = Secondary (Y)
	rt::g_leftController.triggerPressed = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
	rt::g_leftController.triggerValue = rt::g_leftController.triggerPressed ? 1.0f : 0.0f;
	rt::g_leftController.gripPressed = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
	rt::g_leftController.gripValue = rt::g_leftController.gripPressed ? 1.0f : 0.0f;
	rt::g_leftController.menuPressed = (GetAsyncKeyState('G') & 0x8000) != 0;
	rt::g_leftController.primaryPressed = (GetAsyncKeyState('V') & 0x8000) != 0;
	rt::g_leftController.secondaryPressed = (GetAsyncKeyState('B') & 0x8000) != 0;

	// Right controller thumbstick (Arrow keys)
	rt::g_rightController.thumbstick = { 0.0f, 0.0f };
	if (GetAsyncKeyState(VK_UP) & 0x8000) rt::g_rightController.thumbstick.y = 1.0f;
	if (GetAsyncKeyState(VK_DOWN) & 0x8000) rt::g_rightController.thumbstick.y = -1.0f;
	if (GetAsyncKeyState(VK_LEFT) & 0x8000) rt::g_rightController.thumbstick.x = -1.0f;
	if (GetAsyncKeyState(VK_RIGHT) & 0x8000) rt::g_rightController.thumbstick.x = 1.0f;

	// Left controller thumbstick (IJKL keys)
	rt::g_leftController.thumbstick = { 0.0f, 0.0f };
	if (GetAsyncKeyState('I') & 0x8000) rt::g_leftController.thumbstick.y = 1.0f;
	if (GetAsyncKeyState('K') & 0x8000) rt::g_leftController.thumbstick.y = -1.0f;
	if (GetAsyncKeyState('J') & 0x8000) rt::g_leftController.thumbstick.x = -1.0f;
	if (GetAsyncKeyState('L') & 0x8000) rt::g_leftController.thumbstick.x = 1.0f;

	// ========================================
	// Velocity Tracking for Motion Controls
	// ========================================
	// Calculate controller world positions
	XrPosef rightPose, leftPose;
	rt::GetControllerPose(rt::g_rightController, &rightPose, true);
	rt::GetControllerPose(rt::g_leftController, &leftPose, false);

	// Calculate linear velocity from position delta
	if (deltaTime > 0.0f) {
		// Right controller velocity
		rt::g_rightController.linearVelocity.x = (rightPose.position.x - rt::g_rightController.prevPosWorld.x) / deltaTime;
		rt::g_rightController.linearVelocity.y = (rightPose.position.y - rt::g_rightController.prevPosWorld.y) / deltaTime;
		rt::g_rightController.linearVelocity.z = (rightPose.position.z - rt::g_rightController.prevPosWorld.z) / deltaTime;

		// Left controller velocity
		rt::g_leftController.linearVelocity.x = (leftPose.position.x - rt::g_leftController.prevPosWorld.x) / deltaTime;
		rt::g_leftController.linearVelocity.y = (leftPose.position.y - rt::g_leftController.prevPosWorld.y) / deltaTime;
		rt::g_leftController.linearVelocity.z = (leftPose.position.z - rt::g_leftController.prevPosWorld.z) / deltaTime;

		// Angular velocity from yaw/pitch delta
		//float totalRightYaw = rt::g_headYaw + rt::g_rightController.yawOffset;
		//float totalRightPitch = rt::g_headPitch + rt::g_rightController.pitchOffset;
		rt::g_rightController.angularVelocity.x = (rt::g_rightController.pitchOffset - rt::g_rightController.prevPitch) / deltaTime;
		rt::g_rightController.angularVelocity.y = (rt::g_rightController.yawOffset - rt::g_rightController.prevYaw) / deltaTime;
		rt::g_rightController.angularVelocity.z = 0.0f;

		//float totalLeftYaw = rt::g_headYaw + rt::g_leftController.yawOffset;
		//float totalLeftPitch = rt::g_headPitch + rt::g_leftController.pitchOffset;
		rt::g_leftController.angularVelocity.x = (rt::g_leftController.pitchOffset - rt::g_leftController.prevPitch) / deltaTime;
		rt::g_leftController.angularVelocity.y = (rt::g_leftController.yawOffset - rt::g_leftController.prevYaw) / deltaTime;
		rt::g_leftController.angularVelocity.z = 0.0f;

		// Update previous state for next frame
		rt::g_rightController.prevPosWorld = rightPose.position;
		rt::g_rightController.prevYaw = rt::g_rightController.yawOffset;
		rt::g_rightController.prevPitch = rt::g_rightController.pitchOffset;

		rt::g_leftController.prevPosWorld = leftPose.position;
		rt::g_leftController.prevYaw = rt::g_leftController.yawOffset;
		rt::g_leftController.prevPitch = rt::g_leftController.pitchOffset;
	}

	// Check for MCP head pose commands (for automated testing)
	/*mcp::HeadPoseCommand cmd = mcp::CheckHeadPoseCommand();
	if (cmd.valid) {
		rt::g_headPos.x = cmd.x;
		rt::g_headPos.y = cmd.y;
		rt::g_headPos.z = cmd.z;
		rt::g_headYaw = cmd.yaw;
		rt::g_headPitch = cmd.pitch;
		mcp::WriteCommandAck("head_pose", true);
	}*/
	//}

	for (;;) {
		LARGE_INTEGER now; QueryPerformanceCounter(&now);
		double dt = (nextTick - (double)now.QuadPart) / (double)freq.QuadPart;
		if (dt <= 0.0) break;
		double ms = dt * 1000.0;
		if (ms > 5.0) ms = 5.0;
		if (ms < 0.0) ms = 0.0;
		Sleep((DWORD)ms);
	}
	nextTick += periodSec * (double)freq.QuadPart;
	LARGE_INTEGER now; QueryPerformanceCounter(&now);
	// Convert QPC to nanoseconds using double to avoid overflow on MSVC
	XrTime nowTime = (XrTime)((double)now.QuadPart * 1000000000.0 / (double)freq.QuadPart);
	s->type = XR_TYPE_FRAME_STATE; s->shouldRender = XR_TRUE; s->predictedDisplayPeriod = periodNs; s->predictedDisplayTime = nowTime + periodNs;
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrBeginFrame_runtime(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }

static void ensurePreviewSized(rt::Session& s, UINT width, UINT height, DXGI_FORMAT format) {
	if (!s.usesD3D12) {
		if (s.previewSwapchain && s.previewWidth == width && s.previewHeight == height && s.previewFormat == format) return;
	}
	else {
		if (s.previewSwapchain12 && s.previewWidth == width && s.previewHeight == height && s.previewFormat == format) return;
	}

	// IMPORTANT: Release ALL swapchain references before creating a new one
	// DXGI only allows one swapchain per window
	s.previewSwapchain.Reset();
	rt::ResetD3D12PreviewResources(s);
	{
		std::lock_guard<std::mutex> lock(rt::g_windowMutex);
		rt::g_persistentSwapchain.Reset();
	}
	s.previewWidth = width;
	s.previewHeight = height;
	s.previewFormat = format;

	// Register window class if not done (use global flag)
	if (!rt::g_windowClassRegistered) {
		WNDCLASSW wc{};
		wc.lpfnWndProc = rt::WndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"OpenXR WXR";
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		RegisterClassW(&wc);
		rt::g_windowClassRegistered = true;
	}

	if (!s.hwnd) {
		// Check if we have a persistent window from a previous session
		{
			std::lock_guard<std::mutex> lock(rt::g_windowMutex);
			if (rt::g_persistentWindow && IsWindow(rt::g_persistentWindow)) {
				s.hwnd = rt::g_persistentWindow;
				// DON'T clear g_persistentWindow - keep it for reference

				// CRITICAL: Update WndProc to point to this DLL's function
				// After DLL reload, the old WndProc pointer is invalid!
				SetWindowLongPtrW(s.hwnd, GWLP_WNDPROC, (LONG_PTR)rt::WndProc);
				Log("[OXRWXR] Updated window WndProc to new DLL address");

				// Reuse swapchain if compatible
				if (rt::g_persistentSwapchain && rt::g_persistentWidth == width &&
					rt::g_persistentHeight == height && s.previewFormat == format) {
					s.previewSwapchain = rt::g_persistentSwapchain;
					s.previewWidth = rt::g_persistentWidth;
					s.previewHeight = rt::g_persistentHeight;
					s.previewFormat = format;
					Log("[OXRWXR] Reusing existing window AND swapchain from previous session");
					return;  // Everything is already set up
				}

				Log("[OXRWXR] Reusing existing window from previous session (recreating swapchain)");

				// IMPORTANT: Release the old persistent swapchain before creating a new one
				// DXGI only allows one swapchain per window
				rt::g_persistentSwapchain.Reset();

				// Apply dark theme if not already applied
				ui::ApplyDarkTheme(s.hwnd);

				// Just resize the existing window if needed
				RECT rc = { 0, 0, (LONG)width, (LONG)height };
				AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
				SetWindowPos(s.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW);

				// Make sure it's visible
				ShowWindow(s.hwnd, SW_SHOW);
				UpdateWindow(s.hwnd);
			}
		}

		// Create new window if we don't have one
		if (!s.hwnd) {
			RECT rc = { 0, 0, (LONG)width, (LONG)height };
			AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
			s.hwnd = CreateWindowExW(0, L"OpenXR WXR", L"OpenXR WXR (Mouse Look + WASD)", WS_OVERLAPPEDWINDOW,
				100, 100, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
			if (!s.hwnd) {
				Log("[OXRWXR] Failed to create preview window!");
				return;
			}
			ShowWindow(s.hwnd, SW_SHOW);
			UpdateWindow(s.hwnd);
			SetForegroundWindow(s.hwnd);
			SetWindowPos(s.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
			Logf("[OXRWXR] Created new preview window: hwnd=%p size=%ux%u", s.hwnd, width, height);

			// Apply dark theme and menu
			ui::ApplyDarkTheme(s.hwnd);
			ui::g_uiState.windowWidth = width;
			ui::g_uiState.windowHeight = height;



			//----------------
			//OXRWXR CHANGE:
			//---------------- 
			//  Make the window fullscreen borderless
			RECT rc2;
			GetWindowRect(s.hwnd, &rc2);

			bool fullScreenDisplay = true;

			if (fullScreenDisplay) {
				rc2.left -= rc2.left;
				rc2.top -= rc2.top;

				int screenWidth = GetSystemMetrics(SM_CXSCREEN);
				int screenHeight = GetSystemMetrics(SM_CYSCREEN);
				rc2.right = screenWidth;
				rc2.bottom = screenHeight;
			}

			// Modify window style
			LONG_PTR style = GetWindowLongPtr(s.hwnd, GWL_STYLE);
			style &= ~(WS_THICKFRAME | WS_BORDER | WS_CAPTION);
			SetWindowLongPtr(s.hwnd, GWL_STYLE, style);

			// Update window position and size
			SetWindowPos(s.hwnd, HWND_TOP,
				rc2.left, rc2.top,
				rc2.right - rc2.left, rc2.bottom - rc2.top,
				SWP_FRAMECHANGED);

			SetForegroundWindow(s.hwnd);

			// Also save to persistent storage right away
			{
				std::lock_guard<std::mutex> lock(rt::g_windowMutex);
				rt::g_persistentWindow = s.hwnd;
				Log("[OXRWXR] Saved new window to persistent storage");
			}
		}
	}
	else {
		// Resize existing window
		RECT rc = { 0, 0, (LONG)width, (LONG)height };
		AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
		SetWindowPos(s.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW);
		Logf("[OXRWXR] Resized preview window: hwnd=%p size=%ux%u", s.hwnd, width, height);
	}
	if (!s.usesD3D12) {
		ComPtr<IDXGIDevice> dxgiDev; s.d3d11Device.As(&dxgiDev);
		ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(adapter.GetAddressOf());
		ComPtr<IDXGIFactory2> factory; adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.Format = format;  // Use the format from the XR swapchain
		desc.Width = width;
		desc.Height = height;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.SampleDesc.Count = 1;
		HRESULT hr = factory->CreateSwapChainForHwnd(s.d3d11Device.Get(), s.hwnd, &desc, nullptr, nullptr, s.previewSwapchain.GetAddressOf());
		Logf("[OXRWXR] ensurePreviewSized(DX11): hr=0x%08X swapchain=%p format=%d", (unsigned)hr, s.previewSwapchain.Get(), format);
		if (FAILED(hr)) {
			Logf("[OXRWXR] ERROR: Failed to create DX11 preview swapchain with format %d", format);
		}
		return;
	}
	else {
		// DX12 preview swapchain
		ComPtr<IDXGIFactory4> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
			Log("[OXRWXR] DX12 preview: CreateDXGIFactory1 failed");
			return;
		}
		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.Width = width;
		desc.Height = height;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		ComPtr<IDXGISwapChain1> sc1;
		HRESULT hr = factory->CreateSwapChainForHwnd(s.d3d12Queue.Get(), s.hwnd, &desc, nullptr, nullptr, sc1.GetAddressOf());
		if (FAILED(hr)) {
			Logf("[OXRWXR] DX12 preview: CreateSwapChainForHwnd failed 0x%08X", (unsigned)hr);
			return;
		}
		sc1.As(&s.previewSwapchain12);
		s.previewBackbufferCount = desc.BufferCount;
		s.previewBackbuffers.clear();
		s.previewBackbuffers.resize(desc.BufferCount);

		// Create RTV heap
		D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{}; rtvDesc.NumDescriptors = desc.BufferCount; rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(s.d3d12Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(s.previewRTVHeap.GetAddressOf())))) {
			Log("[OXRWXR] DX12 preview: CreateDescriptorHeap RTV failed"); return;
		}
		s.previewRTVDescriptorSize = s.d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = s.previewRTVHeap->GetCPUDescriptorHandleForHeapStart();
		for (UINT i = 0; i < desc.BufferCount; ++i) {
			if (FAILED(s.previewSwapchain12->GetBuffer(i, IID_PPV_ARGS(s.previewBackbuffers[i].GetAddressOf())))) {
				Logf("[OXRWXR] DX12 preview: GetBuffer %u failed", i); return;
			}
			s.d3d12Device->CreateRenderTargetView(s.previewBackbuffers[i].Get(), nullptr, rtvHandle);
			rtvHandle.ptr += s.previewRTVDescriptorSize;
		}
		// Command allocator/list
		s.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(s.previewCmdAlloc.GetAddressOf()));
		s.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.previewCmdAlloc.Get(), nullptr, IID_PPV_ARGS(s.previewCmdList.GetAddressOf()));
		s.previewCmdList->Close();
		// Fence
		s.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(s.previewFence.GetAddressOf()));
		s.previewFenceValue = 1;
		s.previewFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		Log("[OXRWXR] DX12 preview swapchain initialized");
		return;
	}
}

static void blitViewToHalf(rt::Session& s, rt::Swapchain& chain, uint32_t srcIndex, uint32_t arraySlice,
	const XrRect2Di& rect, ID3D11RenderTargetView* rtv,
	const D3D11_VIEWPORT& vp, ID3D11BlendState* blendState, bool isSyncEye = false) {

	//----------------
	//OXRWXR CHANGE:
	//---------------- 
	// Red sync for (DX11)
	//XRTODO PULL THIS VALUE FROM OXR FRAME ID
	int redIntensity = OpenXRFrameID;

	if (!rtv) {
		Log("[OXRWXR] blitViewToHalf: rtv is null!");
		return;
	}

	if (!rt::InitBlitResources(s)) {
		Log("[OXRWXR] Cannot blit, blit resources failed to initialize.");
		return;
	}

	// Check if we have a valid image
	if (srcIndex >= chain.images.size()) {
		Logf("[OXRWXR] blitViewToHalf: srcIndex %u >= images.size() %zu", srcIndex, chain.images.size());
		return;
	}
	if (!chain.images[srcIndex]) {
		Logf("[OXRWXR] blitViewToHalf: images[%u] is null", srcIndex);
		return;
	}

	// Prepare source texture
	ComPtr<ID3D11Texture2D> sourceTexture = chain.images[srcIndex];
	D3D11_TEXTURE2D_DESC srcDesc;
	sourceTexture->GetDesc(&srcDesc);

	// Skip depth formats - they can't be rendered to the preview window
	bool isDepthFormat = (srcDesc.Format == DXGI_FORMAT_D32_FLOAT ||
		srcDesc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
		srcDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
		srcDesc.Format == DXGI_FORMAT_D16_UNORM ||
		srcDesc.Format == DXGI_FORMAT_R32_TYPELESS ||
		srcDesc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
		srcDesc.Format == DXGI_FORMAT_R24G8_TYPELESS ||
		srcDesc.Format == DXGI_FORMAT_R16_TYPELESS);
	if (isDepthFormat) {
		// Depth swapchains are for depth testing, not preview rendering
		static int depthSkipCount = 0;
		if (++depthSkipCount % 60 == 1) {
			Logf("[OXRWXR] blitViewToHalf: Skipping depth format %d", srcDesc.Format);
		}
		return;
	}

	//----------------
	//OXRWXR CHANGE:
	//---------------- 
	// Red sync pixel (DX11) order flipping
	bool flipColorOrder = false;

	// Choose a typed format for SRV and temp texture  
	// Preserve sRGB if original was sRGB to enable auto-conversion
	DXGI_FORMAT typedFormat = srcDesc.Format;
	switch (srcDesc.Format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		typedFormat = (chain.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
		break;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		typedFormat = (chain.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
		flipColorOrder = true;
		break;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		typedFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		break;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		typedFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		break;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		typedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
		break;
	default:
		break; // Already typed or unknown
	}


	// ALWAYS create a temp texture to avoid SRV/RTV binding conflicts
	// The app may still have this texture bound as an RTV, and D3D11 will null the SRV if we try to use it directly
	ComPtr<ID3D11Texture2D> viewTexture;

	// Always make an SRV-only single-slice, single-sample texture
	// Use the typed format to avoid CreateShaderResourceView failures
	D3D11_TEXTURE2D_DESC tempDesc = {};
	tempDesc.Width = srcDesc.Width;
	tempDesc.Height = srcDesc.Height;
	tempDesc.MipLevels = 1;
	tempDesc.ArraySize = 1;
	tempDesc.Format = typedFormat;  // Make temp texture in same format as SRV (may be *_SRGB)
	tempDesc.SampleDesc.Count = 1;
	tempDesc.SampleDesc.Quality = 0;
	tempDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	tempDesc.Usage = D3D11_USAGE_DEFAULT;
	tempDesc.CPUAccessFlags = 0;
	tempDesc.MiscFlags = 0;

	HRESULT hr = s.d3d11Device->CreateTexture2D(&tempDesc, nullptr, viewTexture.GetAddressOf());
	if (FAILED(hr)) {
		Logf("[OXRWXR] Failed to create temp texture for blit: 0x%08X", hr);
		return;
	}

	// Copy the correct subresource (handles array slice)
	UINT srcSubresource = D3D11CalcSubresource(0, arraySlice, chain.mipCount);

	// Handle imageRect cropping for better visual accuracy
	// Apps can submit sub-rects, and sampling the full texture can show mostly black areas
	// Skip cropping when showFullRender is enabled to show the entire swapchain
	int32_t rectX = rect.offset.x;
	int32_t rectY = rect.offset.y;
	int32_t rectW = rect.extent.width;
	int32_t rectH = rect.extent.height;
	const int32_t srcW = (int32_t)srcDesc.Width;
	const int32_t srcH = (int32_t)srcDesc.Height;
	bool rectValid = rectW > 0 && rectH > 0;
	bool rectClamped = false;
	if (rectValid) {
		if (rectX < 0) { rectW += rectX; rectX = 0; rectClamped = true; }
		if (rectY < 0) { rectH += rectY; rectY = 0; rectClamped = true; }
		if (rectX >= srcW || rectY >= srcH) { rectValid = false; }
		if (rectValid && rectX + rectW > srcW) { rectW = srcW - rectX; rectClamped = true; }
		if (rectValid && rectY + rectH > srcH) { rectH = srcH - rectY; rectClamped = true; }
		if (rectW <= 0 || rectH <= 0) { rectValid = false; }
	}
	if (!rectValid && (rect.extent.width != 0 || rect.extent.height != 0)) {
		static int invalidRectCount = 0;
		if (++invalidRectCount % 60 == 1) {
			Logf("[OXRWXR] Invalid imageRect; using full texture (offset=%d,%d extent=%d,%d size=%dx%d)",
				rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height, srcW, srcH);
		}
	}
	bool shouldCrop = !ui::g_uiState.showFullRender &&
		srcDesc.SampleDesc.Count == 1 &&
		rectValid &&
		(rectW < srcW || rectH < srcH || rectX != 0 || rectY != 0);
	if (shouldCrop) {

		// Recreate temp texture with rect size for cropped copying
		tempDesc.Width = rectW;
		tempDesc.Height = rectH;
		tempDesc.Format = typedFormat;  // Ensure format stays correct
		viewTexture.Reset();
		HRESULT hr = s.d3d11Device->CreateTexture2D(&tempDesc, nullptr, viewTexture.GetAddressOf());
		if (FAILED(hr)) {
			Logf("[OXRWXR] Failed to create cropped temp texture: 0x%08X", hr);
			return;
		}

		// Copy only the specified rect
		D3D11_BOX box{};
		box.left = rectX;
		box.top = rectY;
		box.right = rectX + rectW;
		box.bottom = rectY + rectH;
		box.front = 0;
		box.back = 1;

		s.d3d11Context->CopySubresourceRegion(viewTexture.Get(), 0, 0, 0, 0, sourceTexture.Get(), srcSubresource, &box);

		if (rectClamped) {
			Logf("[OXRWXR] Applied clamped imageRect: %dx%d from (%d,%d)",
				rectW, rectH, rectX, rectY);
		}
		else {
			Logf("[OXRWXR] Applied imageRect cropping: %dx%d from (%d,%d)",
				rectW, rectH, rectX, rectY);
		}
	}
	else {
		// Copy full texture or handle MSAA
		if (srcDesc.SampleDesc.Count > 1) {
			// If app used MSAA, resolve it first using the typed format
			s.d3d11Context->ResolveSubresource(viewTexture.Get(), 0, sourceTexture.Get(), srcSubresource, typedFormat);
		}
		else {
			// Otherwise just copy the full texture
			s.d3d11Context->CopySubresourceRegion(viewTexture.Get(), 0, 0, 0, 0, sourceTexture.Get(), srcSubresource, nullptr);
		}
	}

	//----------------
	//OXRWXR CHANGE:
	//---------------- 
	// Red sync pixel (DX11)
	if (isSyncEye) {
		D3D11_BOX redBox;
		redBox.left = 0;
		redBox.top = 0;
		redBox.right = 10;
		redBox.bottom = 10;
		redBox.front = 0;
		redBox.back = 1;

		UINT pitch = 40; // 10 * 4
		BYTE data[10 * 10 * 4];

		if (flipColorOrder) {
			for (int i = 0; i < 10 * 10; i++) {
				data[i * 4 + 0] = 0; // B
				data[i * 4 + 1] = 0;   // G
				data[i * 4 + 2] = OpenXRFrameID;   // R
				data[i * 4 + 3] = 255; // A
			}
		}
		else {
			for (int i = 0; i < 10 * 10; i++) {
				data[i * 4 + 0] = OpenXRFrameID; // R
				data[i * 4 + 1] = 0;   // G
				data[i * 4 + 2] = 0;   // B
				data[i * 4 + 3] = 255; // A
			}
		}

		s.d3d11Context->UpdateSubresource(viewTexture.Get(), 0, &redBox, data, pitch, 0);
	}

	// Create Shader Resource View
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = typedFormat; // Use the proper typed format
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;

	ComPtr<ID3D11ShaderResourceView> srv;
	hr = s.d3d11Device->CreateShaderResourceView(viewTexture.Get(), &srvDesc, srv.GetAddressOf());
	if (FAILED(hr)) {
		Logf("[OXRWXR] Failed to create SRV: 0x%08X", hr);
		return;
	}

	s.d3d11Context->RSSetViewports(1, &vp);

	// Set shaders and resources
	s.d3d11Context->VSSetShader(s.blitVS.Get(), nullptr, 0);
	s.d3d11Context->PSSetShader(s.blitPS.Get(), nullptr, 0);

	ID3D11ShaderResourceView* srvs[] = { srv.Get() };
	s.d3d11Context->PSSetShaderResources(0, 1, srvs);
	ID3D11SamplerState* samplers[] = { s.samplerState.Get() };
	s.d3d11Context->PSSetSamplers(0, 1, samplers);

	// Set pipeline state
	s.d3d11Context->IASetInputLayout(nullptr);
	s.d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	s.d3d11Context->OMSetBlendState(blendState, nullptr, 0xFFFFFFFF);
	s.d3d11Context->OMSetDepthStencilState(nullptr, 0);
	s.d3d11Context->RSSetState(s.noCullRS.Get());  // Use no-cull rasterizer state to prevent triangle culling

	// Bind render target
	ID3D11RenderTargetView* rtvs[1] = { rtv };
	s.d3d11Context->OMSetRenderTargets(1, rtvs, nullptr);

	// Draw fullscreen quad
	s.d3d11Context->Draw(4, 0);

	// Unbind SRV to avoid conflicts with future RTV usage
	ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
	s.d3d11Context->PSSetShaderResources(0, 1, nullSRV);

	static int debugCount = 0;
	if (++debugCount % 120 == 1) {
		Logf("[OXRWXR] blitViewToHalf: srcIdx=%u slice=%u typedFmt=%d srcFmt=%d",
			srcIndex, arraySlice, typedFormat, srcDesc.Format);
		Logf("[OXRWXR]   viewport: x=%.0f y=%.0f w=%.0f h=%.0f", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
		Logf("[OXRWXR]   srcSize: %ux%u, tempSize: %ux%u", srcDesc.Width, srcDesc.Height, tempDesc.Width, tempDesc.Height);
	}
}

// D3D12 blit function - copies swapchain textures to preview backbuffer
static void blitD3D12ToPreview(rt::Session& s,
	rt::Swapchain& chainL, uint32_t leftIdx, uint32_t leftSlice,
	rt::Swapchain* chainR, uint32_t rightIdx, uint32_t rightSlice,
	ui::DisplayLayout layout, ui::ViewMode viewMode) {
	if (!s.previewSwapchain12 || !s.previewCmdList || !s.previewCmdAlloc) {
		Log("[OXRWXR] blitD3D12ToPreview: Missing D3D12 preview resources");
		return;
	}

	// Skip depth-only swapchains
	bool isDepthFormat = (chainL.format == DXGI_FORMAT_D32_FLOAT ||
		chainL.format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
		chainL.format == DXGI_FORMAT_D16_UNORM);
	if (isDepthFormat) {
		return;
	}

	// Wait for previous frame to finish
	if (s.previewFence->GetCompletedValue() < s.previewFenceValue - 1) {
		s.previewFence->SetEventOnCompletion(s.previewFenceValue - 1, s.previewFenceEvent);
		WaitForSingleObject(s.previewFenceEvent, 1000);
	}

	// Get current backbuffer index
	UINT bbIndex = s.previewSwapchain12->GetCurrentBackBufferIndex();
	ID3D12Resource* backbuffer = s.previewBackbuffers[bbIndex].Get();
	if (!backbuffer) {
		Log("[OXRWXR] blitD3D12ToPreview: No backbuffer");
		return;
	}

	// Create a render target view descriptor heap
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = 1;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	s.d3d12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&s.previewRTVHeap));

	// Create a render target view descriptor for backbuffer
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	s.d3d12Device->CreateRenderTargetView(backbuffer, &rtvDesc, s.previewRTVHeap->GetCPUDescriptorHandleForHeapStart());

	// Reset command allocator and list
	HRESULT hr = s.previewCmdAlloc->Reset();
	if (FAILED(hr)) {
		Logf("[OXRWXR] blitD3D12ToPreview: CmdAlloc Reset failed 0x%08X", hr);
		return;
	}
	hr = s.previewCmdList->Reset(s.previewCmdAlloc.Get(), nullptr);
	if (FAILED(hr)) {
		Logf("[OXRWXR] blitD3D12ToPreview: CmdList Reset failed 0x%08X", hr);
		return;
	}

	// Transition backbuffer to copy dest
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = backbuffer;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	s.previewCmdList->ResourceBarrier(1, &barrier);

	auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES newState) {
		if (!res || state == newState) return;
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.StateBefore = state;
		b.Transition.StateAfter = newState;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		s.previewCmdList->ResourceBarrier(1, &b);
		state = newState;
		};

	auto calcSubresource = [](const rt::Swapchain& chain, uint32_t slice, const char* label) -> UINT {
		uint32_t arraySize = chain.arraySize ? chain.arraySize : 1;
		uint32_t mipLevels = chain.mipCount ? chain.mipCount : 1;
		if (slice >= arraySize) {
			Logf("[OXRWXR] blitD3D12ToPreview: %s slice %u out of range (arraySize=%u)", label, slice, arraySize);
			return UINT_MAX;
		}
		return D3D12CalcSubresource(0, slice, 0, mipLevels, arraySize);
		};

	auto copyEye = [&](rt::Swapchain& chain, uint32_t idx, uint32_t slice,
		UINT dstX, UINT dstY, const char* label, bool isSyncEye = false) -> bool {
			//----------------
			//OXRWXR CHANGE:
			//---------------- 
			// Now with red sync for (DX12)
			//XRTODO: Pull this from OpenXR Frame ID, and adjust so it is on a 1.0 scale instead (value / 255.0f?)
			float redIntensity = (OpenXRFrameID / 255.0f);

			if (idx >= chain.images12.size() || !chain.images12[idx]) return false;
			if (chain.imageStates12.size() <= idx) {
				Logf("[OXRWXR] blitD3D12ToPreview: %s missing state tracking", label);
				return false;
			}
			UINT subresource = calcSubresource(chain, slice, label);
			if (subresource == UINT_MAX) return false;

			ID3D12Resource* srcTex = chain.images12[idx].Get();
			D3D12_RESOURCE_STATES prevState = chain.imageStates12[idx];
			transition(srcTex, chain.imageStates12[idx], D3D12_RESOURCE_STATE_COPY_SOURCE);

			D3D12_TEXTURE_COPY_LOCATION dst = {};
			dst.pResource = backbuffer;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = 0;

			D3D12_TEXTURE_COPY_LOCATION src = {};
			src.pResource = srcTex;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = subresource;

			D3D12_BOX srcBox = {};
			srcBox.left = 0;
			srcBox.top = 0;
			srcBox.front = 0;
			srcBox.right = chain.width;
			srcBox.bottom = chain.height;
			srcBox.back = 1;

			s.previewCmdList->CopyTextureRegion(&dst, dstX, dstY, 0, &src, &srcBox);

			if (isSyncEye) {
				D3D12_RESOURCE_STATES backbufferState = D3D12_RESOURCE_STATE_PRESENT;
				transition(backbuffer, backbufferState, D3D12_RESOURCE_STATE_RENDER_TARGET);

				float red[4] = { redIntensity, 0.0f, 0.0f, 1.0f };
				D3D12_RECT rect = { 0, 0, 10, 10 };
				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = s.previewRTVHeap->GetCPUDescriptorHandleForHeapStart();
				s.previewCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
				s.previewCmdList->ClearRenderTargetView(rtvHandle, red, 1, &rect);

				transition(backbuffer, backbufferState, D3D12_RESOURCE_STATE_PRESENT);
			}

			transition(srcTex, chain.imageStates12[idx], prevState);
			return true;
		};

	const bool singleEye = (viewMode != ui::ViewMode::BothEyes);
	bool forceSingleEye = false;
	static bool loggedAnaglyph = false;
	ui::DisplayLayout effectiveLayout = layout;
	if (!singleEye && layout == ui::DisplayLayout::Anaglyph) {
		if (!loggedAnaglyph) {
			Log("[OXRWXR] blitD3D12ToPreview: Anaglyph not supported on D3D12 path; showing left eye only");
			loggedAnaglyph = true;
		}
		forceSingleEye = true;
	}
	if (effectiveLayout == ui::DisplayLayout::Anaglyph) {
		effectiveLayout = ui::DisplayLayout::SideBySide;
	}

	const bool hasLeft = leftIdx < chainL.images12.size() && chainL.images12[leftIdx];
	const bool hasRight = chainR && rightIdx < chainR->images12.size() && chainR->images12[rightIdx];

	// Single-eye mode: render selected eye full-screen
	if (singleEye || forceSingleEye) {
		if (viewMode == ui::ViewMode::RightEyeOnly && hasRight) {
			copyEye(*chainR, rightIdx, rightSlice, 0, 0, "R", true);
		}
		else if (hasLeft) {
			copyEye(chainL, leftIdx, leftSlice, 0, 0, "L", true);
		}
		else if (hasRight) {
			copyEye(*chainR, rightIdx, rightSlice, 0, 0, "R", true);
		}
	}
	else {
		UINT rightX = (effectiveLayout == ui::DisplayLayout::OverUnder) ? 0 : (UINT)(s.previewWidth / 2);
		UINT rightY = (effectiveLayout == ui::DisplayLayout::OverUnder) ? (UINT)(s.previewHeight / 2) : 0;

		if (hasLeft) {
			copyEye(chainL, leftIdx, leftSlice, 0, 0, "L", true);
		}
		if (hasRight) {
			copyEye(*chainR, rightIdx, rightSlice, rightX, rightY, "R", false);
		}
		else if (hasLeft) {
			copyEye(chainL, leftIdx, leftSlice, rightX, rightY, "L", false);
		}
	}

	// Transition backbuffer back to present
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	s.previewCmdList->ResourceBarrier(1, &barrier);

	// Close and execute
	s.previewCmdList->Close();
	ID3D12CommandList* cmdLists[] = { s.previewCmdList.Get() };
	s.d3d12Queue->ExecuteCommandLists(1, cmdLists);

	// Signal fence
	s.d3d12Queue->Signal(s.previewFence.Get(), s.previewFenceValue++);

	static int blitCount = 0;
	if (++blitCount % 60 == 1) {
		Logf("[OXRWXR] blitD3D12ToPreview: Copied L[%u] R[%u] to backbuffer %u", leftIdx, rightIdx, bbIndex);
	}
}

// Flag to track if Present should be called (deferred until all layers rendered)
static bool g_presentPending = false;

static void presentProjection(rt::Session& s, const XrCompositionLayerProjection& proj, bool skipPresent = false) {
	Log("[OXRWXR] ============================================");
	Logf("[OXRWXR] presentProjection called: viewCount=%u, skipPresent=%d", proj.viewCount, (int)skipPresent);
	Log("[OXRWXR] RENDERING FRAME TO PREVIEW WINDOW");
	Log("[OXRWXR] ============================================");
	if (proj.viewCount < 1) {
		Log("[OXRWXR] presentProjection: No views, returning");
		return;
	}
	const auto& vL = proj.views[0];
	auto itL = rt::g_swapchains.find(vL.subImage.swapchain);
	if (itL == rt::g_swapchains.end()) {
		Log("[OXRWXR] presentProjection: Left swapchain not found");
		return;
	}
	auto& chL = itL->second;
	uint32_t width = chL.width, height = chL.height;
	const rt::Swapchain* chRPtr = &chL;
	if (proj.viewCount > 1) {
		const auto& vR = proj.views[1];
		auto itR = rt::g_swapchains.find(vR.subImage.swapchain);
		if (itR != rt::g_swapchains.end()) {
			chRPtr = &itR->second;
			if (itR->second.width > width) width = itR->second.width;
			if (itR->second.height > height) height = itR->second.height;
		}
	}
	{
		std::lock_guard<std::mutex> lock(s.previewMutex);

		// OpenGL preview path - read pixels from GL textures and display via D3D11
		if (s.usesOpenGL) {
			static int glFrameCount = 0;
			glFrameCount++;

			if (glFrameCount % 60 == 1) {
				Logf("[OXRWXR] GL PREVIEW: frame=%d, width=%u, height=%u", glFrameCount, width, height);
			}

			// Make the app's GL context current
			HGLRC savedRC = wglGetCurrentContext();
			HDC savedDC = wglGetCurrentDC();

			if (glFrameCount % 60 == 1) {
				Logf("[OXRWXR] GL PREVIEW: savedRC=%p, savedDC=%p, s.glRC=%p, s.glDC=%p",
					savedRC, savedDC, s.glRC, s.glDC);
			}

			if (s.glRC && s.glDC) {
				BOOL result = wglMakeCurrent(s.glDC, s.glRC);
				if (glFrameCount % 60 == 1) {
					Logf("[OXRWXR] GL PREVIEW: wglMakeCurrent result=%d", result);
				}
			}
			else {
				if (glFrameCount % 60 == 1) {
					Log("[OXRWXR] GL PREVIEW: WARNING - s.glRC or s.glDC is null!");
				}
			}

			// Read pixel data from GL textures into CPU buffers
			std::vector<uint8_t> leftPixels(width * height * 4);
			std::vector<uint8_t> rightPixels(width * height * 4);

			// Get left eye texture
			GLuint leftTex = 0;
			if (chL.imagesGL.size() > 0) {
				uint32_t idx = chL.lastReleased;
				if (idx == UINT32_MAX || idx >= chL.imageCount) idx = chL.lastAcquired;
				if (idx != UINT32_MAX && idx < chL.imagesGL.size()) {
					leftTex = chL.imagesGL[idx];
				}
			}

			// Get right eye texture
			GLuint rightTex = 0;
			if (chRPtr && chRPtr->imagesGL.size() > 0) {
				uint32_t idx = chRPtr->lastReleased;
				if (idx == UINT32_MAX || idx >= chRPtr->imageCount) idx = chRPtr->lastAcquired;
				if (idx != UINT32_MAX && idx < chRPtr->imagesGL.size()) {
					rightTex = chRPtr->imagesGL[idx];
				}
			}

			// Read left eye pixels using glGetTexImage
			if (leftTex != 0) {
				glBindTexture(GL_TEXTURE_2D, leftTex);
				glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, leftPixels.data());
			}

			// Read right eye pixels
			if (rightTex != 0) {
				glBindTexture(GL_TEXTURE_2D, rightTex);
				glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rightPixels.data());
			}

			// Flip images vertically - OpenGL has Y=0 at bottom, D3D expects Y=0 at top
			auto flipImageVertically = [](std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
				const uint32_t rowSize = width * 4;
				std::vector<uint8_t> tempRow(rowSize);
				for (uint32_t y = 0; y < height / 2; y++) {
					uint8_t* topRow = pixels.data() + y * rowSize;
					uint8_t* bottomRow = pixels.data() + (height - 1 - y) * rowSize;
					memcpy(tempRow.data(), topRow, rowSize);
					memcpy(topRow, bottomRow, rowSize);
					memcpy(bottomRow, tempRow.data(), rowSize);
				}
				};
			if (leftTex != 0) flipImageVertically(leftPixels, width, height);
			if (rightTex != 0) flipImageVertically(rightPixels, width, height);

			// MCP Integration - check for screenshot requests and capture (OpenGL path)
			//mcp::CheckScreenshotRequest();
			//if (mcp::g_screenshotRequested) {
			//	if (mcp::g_screenshotLayer == "quad") {
			//		// Capture only quad layer
			//		mcp::CaptureQuadScreenshot();
			//	}
			//	else if (mcp::g_screenshotLayer == "all") {
			//		// Capture both projection and quad layers
			//		mcp::CaptureScreenshotGL(
			//			leftTex != 0 ? leftPixels.data() : nullptr,
			//			rightTex != 0 ? rightPixels.data() : nullptr,
			//			width, height);
			//		// Also capture quad layer separately
			//		if (mcp::g_quadLayerCaptured) {
			//			std::string quadPath = mcp::GetSimulatorDataPath() + "\\screenshot_quad.bmp";
			//			mcp::SavePixelsToBMP(mcp::g_quadLayerPixels.data(),
			//				mcp::g_quadLayerWidth, mcp::g_quadLayerHeight, quadPath.c_str());
			//		}
			//		mcp::g_screenshotRequested = false;
			//	}
			//	else {
			//		// Default: capture projection layer
			//		mcp::CaptureScreenshotGL(
			//			leftTex != 0 ? leftPixels.data() : nullptr,
			//			rightTex != 0 ? rightPixels.data() : nullptr,
			//			width, height);
			//	}
			//}

			// Restore original GL context
			if (savedRC) {
				wglMakeCurrent(savedDC, savedRC);
			}

			// Log progress and check if pixel data is valid
			if (glFrameCount % 60 == 1) {
				Logf("[OXRWXR] OpenGL frame #%d - leftTex=%u, rightTex=%u, size=%ux%u",
					glFrameCount, leftTex, rightTex, width, height);

				// Check if pixels are all zero (black) or have content
				uint32_t leftSum = 0, rightSum = 0;
				for (size_t i = 0; i < std::min((size_t)1000, leftPixels.size()); i++) {
					leftSum += leftPixels[i];
				}
				for (size_t i = 0; i < std::min((size_t)1000, rightPixels.size()); i++) {
					rightSum += rightPixels[i];
				}
				Logf("[OXRWXR] GL PREVIEW: leftPixelSum=%u, rightPixelSum=%u (first 1000 bytes)", leftSum, rightSum);

				// Print first few pixel values (RGBA)
				if (leftPixels.size() >= 16) {
					Logf("[OXRWXR] GL PREVIEW: First 4 pixels (RGBA): [%d,%d,%d,%d] [%d,%d,%d,%d] [%d,%d,%d,%d] [%d,%d,%d,%d]",
						leftPixels[0], leftPixels[1], leftPixels[2], leftPixels[3],
						leftPixels[4], leftPixels[5], leftPixels[6], leftPixels[7],
						leftPixels[8], leftPixels[9], leftPixels[10], leftPixels[11],
						leftPixels[12], leftPixels[13], leftPixels[14], leftPixels[15]);
				}

				// Check a pixel in the middle of the image
				size_t midOffset = (height / 2) * width * 4 + (width / 2) * 4;
				if (midOffset + 4 <= leftPixels.size()) {
					Logf("[OXRWXR] GL PREVIEW: Middle pixel (RGBA): [%d,%d,%d,%d]",
						leftPixels[midOffset], leftPixels[midOffset + 1], leftPixels[midOffset + 2], leftPixels[midOffset + 3]);
				}
			}

			// Now create/use D3D11 preview if we don't have one yet
			if (!s.d3d11Device) {
				// Create D3D11 device for preview window
				D3D_FEATURE_LEVEL featureLevel;
				UINT flags = 0;
#ifdef _DEBUG
				flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
				HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
					nullptr, 0, D3D11_SDK_VERSION,
					&s.d3d11Device, &featureLevel, &s.d3d11Context);
				if (FAILED(hr)) {
					Logf("[OXRWXR] Failed to create D3D11 device for GL preview: 0x%08X", hr);
					return;
				}
				Log("[OXRWXR] Created D3D11 device for OpenGL preview");

				// Force shader recompilation by resetting blit resources
				s.blitVS.Reset();
				s.blitPS.Reset();
				s.samplerState.Reset();
				s.noCullRS.Reset();
				s.anaglyphRedBS.Reset();
				s.anaglyphCyanBS.Reset();
				Log("[OXRWXR] Reset blit resources for fresh shader compilation");
			}

			// Use the standard preview path now that we have a D3D11 device
			DXGI_FORMAT displayFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			const auto viewMode = ui::g_uiState.viewMode;
			const auto layout = ui::g_uiState.displayLayout;
			int targetWidth = (int)width;
			int targetHeight = (int)height;
			ui::CalculateWindowSize((int)width, (int)height, targetWidth, targetHeight);

			if (glFrameCount % 60 == 1) {
				Logf("[OXRWXR] GL PREVIEW: targetSize=%dx%d, calling ensurePreviewSized", targetWidth, targetHeight);
			}

			ensurePreviewSized(s, (UINT)targetWidth, (UINT)targetHeight, displayFormat);

			if (!s.previewSwapchain) {
				Log("[OXRWXR] GL PREVIEW: ERROR - previewSwapchain is NULL after ensurePreviewSized!");
				return;
			}

			// Get the backbuffer
			ComPtr<ID3D11Texture2D> bb;
			if (FAILED(s.previewSwapchain->GetBuffer(0, IID_PPV_ARGS(bb.GetAddressOf())))) {
				Log("[OXRWXR] Failed to get preview swapchain buffer for GL preview");
				return;
			}

			// Create staging textures to upload GL pixel data
			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = width;
			texDesc.Height = height;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			D3D11_SUBRESOURCE_DATA initData = {};
			initData.pSysMem = leftPixels.data();
			initData.SysMemPitch = width * 4;

			ComPtr<ID3D11Texture2D> leftTex2D;
			s.d3d11Device->CreateTexture2D(&texDesc, &initData, &leftTex2D);

			initData.pSysMem = rightPixels.data();
			ComPtr<ID3D11Texture2D> rightTex2D;
			s.d3d11Device->CreateTexture2D(&texDesc, &initData, &rightTex2D);

			// Use shader-based rendering for proper side-by-side display
			const bool singleEye = (viewMode != ui::ViewMode::BothEyes);
			const bool showLeft = (viewMode != ui::ViewMode::RightEyeOnly);
			const bool showRight = (viewMode != ui::ViewMode::LeftEyeOnly);

			// Initialize blit resources if not already done
			if (!rt::InitBlitResources(s)) {
				Log("[OXRWXR] OpenGL preview: Failed to init blit resources");
				return;
			}

			// Create render target view for the backbuffer
			ComPtr<ID3D11RenderTargetView> rtv;
			if (FAILED(s.d3d11Device->CreateRenderTargetView(bb.Get(), nullptr, rtv.GetAddressOf()))) {
				Log("[OXRWXR] OpenGL preview: Failed to create RTV");
				return;
			}

			// Create SRVs for the uploaded textures
			ComPtr<ID3D11ShaderResourceView> leftSRV, rightSRV;
			if (leftTex2D) {
				HRESULT hr = s.d3d11Device->CreateShaderResourceView(leftTex2D.Get(), nullptr, leftSRV.GetAddressOf());
				if (FAILED(hr) && glFrameCount % 60 == 1) {
					Logf("[OXRWXR] GL PREVIEW: CreateSRV for left failed: 0x%08X", hr);
				}
			}
			if (rightTex2D) {
				HRESULT hr = s.d3d11Device->CreateShaderResourceView(rightTex2D.Get(), nullptr, rightSRV.GetAddressOf());
				if (FAILED(hr) && glFrameCount % 60 == 1) {
					Logf("[OXRWXR] GL PREVIEW: CreateSRV for right failed: 0x%08X", hr);
				}
			}

			if (glFrameCount % 60 == 1) {
				Logf("[OXRWXR] GL PREVIEW: leftTex2D=%p rightTex2D=%p leftSRV=%p rightSRV=%p",
					leftTex2D.Get(), rightTex2D.Get(), leftSRV.Get(), rightSRV.Get());
			}

			// Clear the render target
			ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
			s.d3d11Context->OMSetRenderTargets(1, rtvs, nullptr);
			const float clearColor[4] = { 0.1f, 0.1f, 0.2f, 1.0f };  // Dark blue
			s.d3d11Context->ClearRenderTargetView(rtv.Get(), clearColor);

			// Setup viewports for left and right eyes
			D3D11_VIEWPORT fullVp = {};
			fullVp.TopLeftX = 0.0f;
			fullVp.TopLeftY = 0.0f;
			fullVp.Width = (float)s.previewWidth;
			fullVp.Height = (float)s.previewHeight;
			fullVp.MinDepth = 0.0f;
			fullVp.MaxDepth = 1.0f;

			D3D11_VIEWPORT leftVp = fullVp;
			D3D11_VIEWPORT rightVp = fullVp;
			if (!singleEye) {
				if (layout == ui::DisplayLayout::SideBySide) {
					leftVp.Width = (float)s.previewWidth / 2.0f;
					rightVp.Width = (float)s.previewWidth / 2.0f;
					rightVp.TopLeftX = (float)s.previewWidth / 2.0f;
				}
				else if (layout == ui::DisplayLayout::OverUnder) {
					leftVp.Height = (float)s.previewHeight / 2.0f;
					rightVp.Height = (float)s.previewHeight / 2.0f;
					rightVp.TopLeftY = (float)s.previewHeight / 2.0f;
				}
			}

			// Helper lambda to blit a texture to a viewport
			auto blitTexture = [&](ID3D11ShaderResourceView* srv, const D3D11_VIEWPORT& vp) {
				if (!srv) {
					if (glFrameCount % 60 == 1) Log("[OXRWXR] GL PREVIEW: blitTexture - SRV is null!");
					return;
				}

				if (glFrameCount % 60 == 1) {
					Logf("[OXRWXR] GL PREVIEW: blitTexture - vp=(%.0f,%.0f,%.0f,%.0f) srv=%p",
						vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, srv);
				}

				// Ensure render target is bound
				ID3D11RenderTargetView* currentRTVs[1] = { rtv.Get() };
				s.d3d11Context->OMSetRenderTargets(1, currentRTVs, nullptr);

				s.d3d11Context->RSSetViewports(1, &vp);
				s.d3d11Context->VSSetShader(s.blitVS.Get(), nullptr, 0);
				s.d3d11Context->PSSetShader(s.blitPS.Get(), nullptr, 0);

				ID3D11ShaderResourceView* srvs[] = { srv };
				s.d3d11Context->PSSetShaderResources(0, 1, srvs);
				ID3D11SamplerState* samplers[] = { s.samplerState.Get() };
				s.d3d11Context->PSSetSamplers(0, 1, samplers);

				s.d3d11Context->IASetInputLayout(nullptr);
				s.d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				s.d3d11Context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
				s.d3d11Context->OMSetDepthStencilState(nullptr, 0);
				s.d3d11Context->RSSetState(s.noCullRS.Get());

				s.d3d11Context->Draw(4, 0);

				// Unbind SRV
				ID3D11ShaderResourceView* nullSRV[] = { nullptr };
				s.d3d11Context->PSSetShaderResources(0, 1, nullSRV);
				};

			//----------------
			//OXRWXR CHANGE:
			//---------------- 
			// Helper lambda to blit a solid red quad (OPENGL)
			auto blitRedQuad = [&](const D3D11_VIEWPORT& vp, int redVal = 255) {
				// Define a simple red pixel shader
				struct Vertex {
					float x, y, z;
				};
				Vertex vertices[] = {
					{ 0.0f, 0.0f, 0.0f },
					{ 10.0f, 0.0f, 0.0f },
					{ 0.0f, 10.0f, 0.0f },
					{ 10.0f, 10.0f, 0.0f }
				};

				// Create a vertex buffer
				ID3D11Buffer* vertexBuffer = nullptr;
				D3D11_BUFFER_DESC bd = {};
				bd.ByteWidth = sizeof(vertices);
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				bd.Usage = D3D11_USAGE_DYNAMIC;
				D3D11_SUBRESOURCE_DATA initData = { vertices, 0, 0 };
				s.d3d11Device->CreateBuffer(&bd, &initData, &vertexBuffer);

				// Map vertex buffer
				D3D11_MAPPED_SUBRESOURCE mappedResource;
				s.d3d11Context->Map(vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
				memcpy(mappedResource.pData, vertices, sizeof(vertices));
				s.d3d11Context->Unmap(vertexBuffer, 0);

				// Ensure render target is bound
				ID3D11RenderTargetView* currentRTVs[1] = { rtv.Get() };
				s.d3d11Context->OMSetRenderTargets(1, currentRTVs, nullptr);

				s.d3d11Context->RSSetViewports(1, &vp);
				s.d3d11Context->VSSetShader(s.blitVS.Get(), nullptr, 0);
				s.d3d11Context->PSSetShader(s.solidColorPS.Get(), nullptr, 0);

				if (!s.colorConstantBuffer) {
					D3D11_BUFFER_DESC cbDesc = {};
					cbDesc.ByteWidth = sizeof(float) * 4;
					cbDesc.Usage = D3D11_USAGE_DEFAULT;
					cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
					HRESULT hr = s.d3d11Device->CreateBuffer(&cbDesc, nullptr, s.colorConstantBuffer.GetAddressOf());
					if (FAILED(hr)) {
						Logf("[WinXrApi] Failed to create color constant buffer: 0x%08X", hr);
						// Release resources
						vertexBuffer->Release();
						return;
					}
				}

				// Update color constant buffer
				struct ColorConstantBuffer {
					float color[4];
				} colorBuffer;
				colorBuffer.color[0] = redVal;
				colorBuffer.color[1] = 0;
				colorBuffer.color[2] = 0;
				colorBuffer.color[3] = 255;
				s.d3d11Context->UpdateSubresource(s.colorConstantBuffer.Get(), 0, nullptr, &colorBuffer, 0, 0);
				s.d3d11Context->PSSetConstantBuffers(0, 1, s.colorConstantBuffer.GetAddressOf());

				// Set vertex buffer
				UINT stride = sizeof(Vertex);
				UINT offset = 0;
				s.d3d11Context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
				s.d3d11Context->IASetInputLayout(s.simpleVertexLayout.Get());
				s.d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				s.d3d11Context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
				s.d3d11Context->OMSetDepthStencilState(nullptr, 0);
				s.d3d11Context->RSSetState(s.noCullRS.Get());

				// Draw the quad
				s.d3d11Context->Draw(4, 0);

				// Clean up
				vertexBuffer->Release();
				};

			// Render the eyes
			if (singleEye) {
				if (showLeft && leftSRV) {
					blitTexture(leftSRV.Get(), fullVp);
				}
				else if (showRight && rightSRV) {
					blitTexture(rightSRV.Get(), fullVp);
				}
			}
			else {
				// Side by side (or over/under)
				if (showLeft && leftSRV) {
					blitTexture(leftSRV.Get(), leftVp);
				}
				if (showRight && rightSRV) {
					blitTexture(rightSRV.Get(), rightVp);
				}
				else if (showRight && leftSRV) {
					// Mirror left eye if no right eye available
					blitTexture(leftSRV.Get(), rightVp);
				}
			}

			//----------------
			//OXRWXR CHANGE:
			//---------------- 
			// Render the OpenXR Frame Sync pixels (OPENGL)
			Logf("[WinXrApi] Drawing Red Sync Pixel (OPENGL)");
			D3D11_VIEWPORT vpOXR = {};
			vpOXR.TopLeftX = 0;
			vpOXR.TopLeftY = 0;
			vpOXR.Width = 100;
			vpOXR.Height = 100;
			vpOXR.MinDepth = 0;
			vpOXR.MaxDepth = 1;

			//XRTODO: Pass OpenXR Frame ID sync
			blitRedQuad(vpOXR, OpenXRFrameID);

			// Update window title with FPS
			static int glTitleFrameCount = 0;
			static auto glLastTitleUpdate = std::chrono::high_resolution_clock::now();
			static int glLastFPS = 0;
			glTitleFrameCount++;
			auto now = std::chrono::high_resolution_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - glLastTitleUpdate).count();
			if (elapsed >= 500) {
				glLastFPS = (int)(glTitleFrameCount * 1000 / elapsed);
				glTitleFrameCount = 0;
				glLastTitleUpdate = now;
				ui::UpdateWindowTitle(s.hwnd, glLastFPS, 0);
			}

			// Present (may be deferred if overlays are pending)
			if (!skipPresent) {
				MSG msg;
				while (PeekMessageW(&msg, s.hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

				if (glFrameCount % 60 == 1) {
					Logf("[OXRWXR] GL PREVIEW: About to Present - hwnd=%p, swapchain=%p", s.hwnd, s.previewSwapchain.Get());
				}

				HRESULT presentHr = s.previewSwapchain->Present(1, 0);
				if (FAILED(presentHr) && glFrameCount % 60 == 1) {
					Logf("[OXRWXR] GL PREVIEW: Present FAILED with hr=0x%08X", presentHr);
				}
			}
			else {
				g_presentPending = true;
			}

			return;
		}

		// Use UNORM format for swapchain (SRGB not valid for FLIP_DISCARD)
		// We create SRGB RTVs for proper gamma when rendering
		DXGI_FORMAT displayFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		const auto viewMode = ui::g_uiState.viewMode;
		const auto layout = ui::g_uiState.displayLayout;
		int targetWidth = (int)width;
		int targetHeight = (int)height;
		ui::CalculateWindowSize((int)width, (int)height, targetWidth, targetHeight);
		ensurePreviewSized(s, (UINT)targetWidth, (UINT)targetHeight, displayFormat);
		const bool singleEye = (viewMode != ui::ViewMode::BothEyes);
		const bool showLeft = (viewMode != ui::ViewMode::RightEyeOnly);
		const bool showRight = (viewMode != ui::ViewMode::LeftEyeOnly);

		// Get left image index
		uint32_t leftIdx = 0;
		if (chL.lastReleased != UINT32_MAX && chL.lastReleased < chL.imageCount) {
			leftIdx = chL.lastReleased;
		}
		else if (chL.lastAcquired != UINT32_MAX && chL.lastAcquired < chL.imageCount) {
			leftIdx = chL.lastAcquired;
		}

		static int blitCount = 0;
		if (++blitCount % 60 == 1) {  // Log every 60 frames
			Logf("[OXRWXR] Blitting left eye: idx=%u (lastReleased=%u, lastAcquired=%u, imageCount=%u)",
				leftIdx, chL.lastReleased, chL.lastAcquired, chL.imageCount);
		}

		if (!s.usesD3D12) {
			// ===== D3D11 PATH =====
			if (!s.previewSwapchain) return;

			// Save D3D11 context state - will auto-restore when stateBackup goes out of scope
			D3D11StateBackup stateBackup(s.d3d11Context.Get());

			// Get the backbuffer and create RTV
			ComPtr<ID3D11Texture2D> bb;
			if (FAILED(s.previewSwapchain->GetBuffer(0, IID_PPV_ARGS(bb.GetAddressOf())))) {
				Log("[OXRWXR] Failed to get preview swapchain buffer.");
				return;
			}

			// Create explicit sRGB RTV for proper gamma encoding
			DXGI_FORMAT bbFmt = s.previewFormat;
			DXGI_FORMAT rtvFmt = bbFmt;
			if (bbFmt == DXGI_FORMAT_R8G8B8A8_UNORM)       rtvFmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			else if (bbFmt == DXGI_FORMAT_B8G8R8A8_UNORM)  rtvFmt = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = rtvFmt;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rtvDesc.Texture2D.MipSlice = 0;

			ComPtr<ID3D11RenderTargetView> rtv;
			HRESULT hr = s.d3d11Device->CreateRenderTargetView(bb.Get(), &rtvDesc, rtv.GetAddressOf());
			if (FAILED(hr)) {
				Logf("[OXRWXR] Explicit sRGB RTV failed (0x%08X), falling back to auto format", hr);
				if (FAILED(s.d3d11Device->CreateRenderTargetView(bb.Get(), nullptr, rtv.GetAddressOf()))) {
					Log("[OXRWXR] Failed to create RTV for preview.");
					return;
				}
			}

			// Bind RTV and clear
			ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
			s.d3d11Context->OMSetRenderTargets(1, rtvs, nullptr);
			const float clearColorDefault[4] = { 0.1f, 0.1f, 0.2f, 1.0f };
			const float clearColorAnaglyph[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			const float* clearColor = (layout == ui::DisplayLayout::Anaglyph) ? clearColorAnaglyph : clearColorDefault;
			s.d3d11Context->ClearRenderTargetView(rtv.Get(), clearColor);

			D3D11_VIEWPORT fullVp = {};
			fullVp.TopLeftX = 0.0f;
			fullVp.TopLeftY = 0.0f;
			fullVp.Width = (float)s.previewWidth;
			fullVp.Height = (float)s.previewHeight;
			fullVp.MinDepth = 0.0f;
			fullVp.MaxDepth = 1.0f;

			D3D11_VIEWPORT leftVp = fullVp;
			D3D11_VIEWPORT rightVp = fullVp;
			if (!singleEye) {
				if (layout == ui::DisplayLayout::SideBySide) {
					leftVp.Width = (float)s.previewWidth / 2.0f;
					rightVp.Width = (float)s.previewWidth / 2.0f;
					rightVp.TopLeftX = (float)s.previewWidth / 2.0f;
				}
				else if (layout == ui::DisplayLayout::OverUnder) {
					leftVp.Height = (float)s.previewHeight / 2.0f;
					rightVp.Height = (float)s.previewHeight / 2.0f;
					rightVp.TopLeftY = (float)s.previewHeight / 2.0f;
				}
			}

			ID3D11BlendState* leftBlend = nullptr;
			ID3D11BlendState* rightBlend = nullptr;
			if (!singleEye && layout == ui::DisplayLayout::Anaglyph) {
				leftBlend = s.anaglyphRedBS.Get();
				rightBlend = s.anaglyphCyanBS.Get();
			}

			if (showLeft) {
				blitViewToHalf(s, chL, leftIdx, vL.subImage.imageArrayIndex, vL.subImage.imageRect,
					rtv.Get(), leftVp, leftBlend, true);
			}

			// Blit right eye
			if (showRight && proj.viewCount > 1) {
				const auto& vR = proj.views[1];
				auto& chR = const_cast<rt::Swapchain&>(*chRPtr);
				uint32_t rightIdx = 0;
				if (chR.lastReleased != UINT32_MAX && chR.lastReleased < chR.imageCount) {
					rightIdx = chR.lastReleased;
				}
				else if (chR.lastAcquired != UINT32_MAX && chR.lastAcquired < chR.imageCount) {
					rightIdx = chR.lastAcquired;
				}
				blitViewToHalf(s, chR, rightIdx, vR.subImage.imageArrayIndex, vR.subImage.imageRect,
					rtv.Get(), rightVp, rightBlend, !showLeft);
			}
			else if (showRight && !showLeft) {
				// Mirror left eye if right-only mode but only one view
				blitViewToHalf(s, chL, leftIdx, vL.subImage.imageArrayIndex, vL.subImage.imageRect,
					rtv.Get(), rightVp, rightBlend, true);
			}

			// Present D3D11 (may be deferred if overlays are pending)
			if (!skipPresent) {
				MSG msg;
				while (PeekMessageW(&msg, s.hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
				s.previewSwapchain->Present(1, 0);
			}
			else {
				g_presentPending = true;
			}

			// Update window title with stats
			static int titleFrameCount = 0;
			static auto lastTitleUpdate = std::chrono::high_resolution_clock::now();
			static int lastFPS = 0;
			titleFrameCount++;
			auto now = std::chrono::high_resolution_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTitleUpdate).count();
			if (elapsed >= 500) {
				lastFPS = (int)(titleFrameCount * 1000 / elapsed);
				titleFrameCount = 0;
				lastTitleUpdate = now;
				ui::UpdateWindowTitle(s.hwnd, lastFPS, 0);
			}

			// MCP Integration - check for screenshot requests and capture
			/*mcp::CheckScreenshotRequest();
			if (mcp::g_screenshotRequested) {
				mcp::CaptureScreenshot(s.d3d11Device.Get(), s.d3d11Context.Get(), s.previewSwapchain.Get());
			}*/

		}
		else {
			// ===== D3D12 PATH =====
			if (!s.previewSwapchain12) return;

			// Blit using D3D12 copy commands
			if (proj.viewCount > 1) {
				const auto& vR = proj.views[1];
				auto& chR = const_cast<rt::Swapchain&>(*chRPtr);
				uint32_t rightIdx = 0;
				if (chR.lastReleased != UINT32_MAX && chR.lastReleased < chR.imageCount) {
					rightIdx = chR.lastReleased;
				}
				else if (chR.lastAcquired != UINT32_MAX && chR.lastAcquired < chR.imageCount) {
					rightIdx = chR.lastAcquired;
				}
				blitD3D12ToPreview(s, chL, leftIdx, vL.subImage.imageArrayIndex,
					&chR, rightIdx, vR.subImage.imageArrayIndex,
					layout, viewMode);
			}
			else {
				blitD3D12ToPreview(s, chL, leftIdx, vL.subImage.imageArrayIndex,
					nullptr, 0, 0, layout, viewMode);
			}

			// Present D3D12 (may be deferred if overlays are pending)
			if (!skipPresent) {
				MSG msg;
				while (PeekMessageW(&msg, s.hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
				s.previewSwapchain12->Present(1, 0);
			}
			else {
				g_presentPending = true;
			}
		}
	}
}

// Render a quad layer as 2D overlay (supports both D3D11 and OpenGL)
static void renderQuadLayer(rt::Session& s, const XrCompositionLayerQuad* quad) {
	if (!quad || !s.previewSwapchain) return;

	if (s.usesD3D12) {
		static bool warnedD3D12 = false;
		if (!warnedD3D12) {
			Log("[OXRWXR] WARNING: Quad layer rendering not implemented for D3D12 sessions");
			warnedD3D12 = true;
		}
		return;
	}

	auto it = rt::g_swapchains.find(quad->subImage.swapchain);
	if (it == rt::g_swapchains.end()) return;

	auto& chain = it->second;

	// Get texture dimensions from the quad subImage
	uint32_t texWidth = quad->subImage.imageRect.extent.width;
	uint32_t texHeight = quad->subImage.imageRect.extent.height;
	if (texWidth == 0) texWidth = chain.width;
	if (texHeight == 0) texHeight = chain.height;

	// Get texture index
	uint32_t texIdx = (chain.lastReleased != UINT32_MAX) ? chain.lastReleased :
		(chain.lastAcquired != UINT32_MAX) ? chain.lastAcquired : 0;

	static int quadLogCount = 0;
	bool shouldLog = (++quadLogCount % 60 == 1);

	if (shouldLog) {
		Logf("[OXRWXR] Quad swapchain: handle=%llu, lastReleased=%u, lastAcquired=%u, texIdx=%u, imageCount=%u",
			(unsigned long long)quad->subImage.swapchain, chain.lastReleased, chain.lastAcquired,
			texIdx, chain.imageCount);
	}

	ComPtr<ID3D11Texture2D> quadTex;

	// Check if using OpenGL
	if (chain.backend == rt::Swapchain::Backend::OpenGL && !chain.imagesGL.empty()) {
		if (texIdx >= chain.imagesGL.size()) return;
		GLuint glTex = chain.imagesGL[texIdx];
		if (glTex == 0) return;

		// Save and switch to app's GL context
		HGLRC savedRC = wglGetCurrentContext();
		HDC savedDC = wglGetCurrentDC();

		// DEBUG: Log current context BEFORE switch
		if (shouldLog) {
			Logf("[OXRWXR] Quad GL context: current={RC=%p,DC=%p}, stored={RC=%p,DC=%p}",
				savedRC, savedDC, s.glRC, s.glDC);
		}

		if (s.glRC && s.glDC) {
			BOOL switchResult = wglMakeCurrent(s.glDC, s.glRC);
			if (shouldLog) {
				HGLRC afterRC = wglGetCurrentContext();
				Logf("[OXRWXR] Quad GL context switch: result=%d, afterRC=%p (expected %p)",
					switchResult, afterRC, s.glRC);
			}
		}

		// Ensure all GL commands are finished before reading
		glFinish();

		// DEBUG: Verify texture exists and is valid
		if (shouldLog) {
			GLboolean isValid = glIsTexture(glTex);
			Logf("[OXRWXR] Quad texture check: glTex=%u, glIsTexture=%d", glTex, isValid);
		}

		// Read pixels from GL texture using FBO (more reliable than glGetTexImage)
		std::vector<uint8_t> pixels(texWidth * texHeight * 4);

		// Try FBO method first if available
		bool usedFBO = false;
		if (EnsureGLFramebufferFuncs()) {
			// Create a temporary FBO to read the texture
			GLuint readFBO = 0;
			g_glGenFramebuffers(1, &readFBO);
			g_glBindFramebuffer(GL_FRAMEBUFFER, readFBO);
			g_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);

			GLenum fboStatus = g_glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (fboStatus == GL_FRAMEBUFFER_COMPLETE) {
				glReadPixels(0, 0, texWidth, texHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
				usedFBO = true;
			}
			else {
				if (shouldLog) {
					Logf("[OXRWXR] Quad FBO not complete: status=0x%X, falling back to glGetTexImage", fboStatus);
				}
			}

			// Cleanup FBO
			g_glBindFramebuffer(GL_FRAMEBUFFER, 0);
			g_glDeleteFramebuffers(1, &readFBO);
		}

		// Fallback to glGetTexImage
		if (!usedFBO) {
			glBindTexture(GL_TEXTURE_2D, glTex);
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		}

		// Debug: check pixel values before flip
		if (shouldLog) {
			uint32_t pixelSum = 0;
			for (size_t i = 0; i < std::min((size_t)4000, pixels.size()); i++) pixelSum += pixels[i];
			Logf("[OXRWXR] Quad GL pixels: sum=%u, first 4=[%d,%d,%d,%d][%d,%d,%d,%d][%d,%d,%d,%d][%d,%d,%d,%d]",
				pixelSum, pixels[0], pixels[1], pixels[2], pixels[3],
				pixels[4], pixels[5], pixels[6], pixels[7],
				pixels[8], pixels[9], pixels[10], pixels[11],
				pixels[12], pixels[13], pixels[14], pixels[15]);
			// Check middle of image
			size_t midIdx = (texHeight / 2 * texWidth + texWidth / 2) * 4;
			if (midIdx + 3 < pixels.size()) {
				Logf("[OXRWXR] Quad GL middle pixel: [%d,%d,%d,%d]",
					pixels[midIdx], pixels[midIdx + 1], pixels[midIdx + 2], pixels[midIdx + 3]);
			}
		}

		// Flip vertically (OpenGL has Y=0 at bottom)
		const uint32_t rowSize = texWidth * 4;
		std::vector<uint8_t> tempRow(rowSize);
		for (uint32_t y = 0; y < texHeight / 2; y++) {
			uint8_t* topRow = pixels.data() + y * rowSize;
			uint8_t* bottomRow = pixels.data() + (texHeight - 1 - y) * rowSize;
			memcpy(tempRow.data(), topRow, rowSize);
			memcpy(topRow, bottomRow, rowSize);
			memcpy(bottomRow, tempRow.data(), rowSize);
		}

		// Store quad layer pixels for MCP screenshot capture
		//mcp::StoreQuadLayerPixels(pixels.data(), texWidth, texHeight);

		// Restore GL context
		if (savedRC) wglMakeCurrent(savedDC, savedRC);

		// Create D3D11 texture from pixel data
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = texWidth;
		texDesc.Height = texHeight;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA initData = {};
		initData.pSysMem = pixels.data();
		initData.SysMemPitch = texWidth * 4;

		if (FAILED(s.d3d11Device->CreateTexture2D(&texDesc, &initData, quadTex.GetAddressOf()))) {
			if (shouldLog) Log("[OXRWXR] renderQuadLayer: Failed to create D3D11 texture from GL pixels");
			return;
		}

		if (shouldLog) {
			Logf("[OXRWXR] Rendering quad layer (OpenGL): size=%.2fx%.2f, texSize=%ux%u, glTex=%u",
				quad->size.width, quad->size.height, texWidth, texHeight, glTex);
		}
	}
	else if (!chain.images.empty()) {
		// D3D11 path
		if (texIdx >= chain.images.size() || !chain.images[texIdx]) return;

		// Skip depth formats
		D3D11_TEXTURE2D_DESC srcDesc;
		chain.images[texIdx]->GetDesc(&srcDesc);
		if (srcDesc.Format == DXGI_FORMAT_D32_FLOAT || srcDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
			srcDesc.Format == DXGI_FORMAT_D16_UNORM || srcDesc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
			return;
		}

		// Convert typeless formats to typed formats for SRV creation
		DXGI_FORMAT typedFormat = srcDesc.Format;
		switch (srcDesc.Format) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			typedFormat = (chain.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
			break;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			typedFormat = (chain.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
			break;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			typedFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
			break;
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
			typedFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
			break;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			typedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
			break;
		default:
			break; // Already typed or unknown
		}

		// Create temp texture for the quad content with typed format
		D3D11_TEXTURE2D_DESC tempDesc = srcDesc;
		tempDesc.Format = typedFormat;  // Use typed format for the temp texture
		tempDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		tempDesc.MiscFlags = 0;
		tempDesc.SampleDesc.Count = 1;

		if (FAILED(s.d3d11Device->CreateTexture2D(&tempDesc, nullptr, quadTex.GetAddressOf()))) return;

		// Copy the quad texture (handle array index if needed)
		const auto& rect = quad->subImage.imageRect;
		uint32_t arraySlice = quad->subImage.imageArrayIndex;
		D3D11_BOX box = { (UINT)rect.offset.x, (UINT)rect.offset.y, 0,
						  (UINT)(rect.offset.x + rect.extent.width), (UINT)(rect.offset.y + rect.extent.height), 1 };
		uint32_t srcSubresource = D3D11CalcSubresource(0, arraySlice, 1);
		s.d3d11Context->CopySubresourceRegion(quadTex.Get(), 0, 0, 0, 0, chain.images[texIdx].Get(), srcSubresource, &box);

		if (shouldLog) {
			Logf("[OXRWXR] Rendering quad layer (D3D11): size=%.2fx%.2f, texSize=%ux%u, typedFmt=%d, srcFmt=%d, arraySlice=%u",
				quad->size.width, quad->size.height, srcDesc.Width, srcDesc.Height, typedFormat, srcDesc.Format, arraySlice);
		}
	}
	else {
		if (shouldLog) Log("[OXRWXR] renderQuadLayer: No valid images in swapchain");
		return;
	}

	// Get backbuffer
	ComPtr<ID3D11Texture2D> bb;
	if (FAILED(s.previewSwapchain->GetBuffer(0, IID_PPV_ARGS(bb.GetAddressOf())))) return;

	// Create RTV
	ComPtr<ID3D11RenderTargetView> rtv;
	if (FAILED(s.d3d11Device->CreateRenderTargetView(bb.Get(), nullptr, rtv.GetAddressOf()))) return;

	// Create SRV
	ComPtr<ID3D11ShaderResourceView> srv;
	if (FAILED(s.d3d11Device->CreateShaderResourceView(quadTex.Get(), nullptr, srv.GetAddressOf()))) return;

	// Calculate viewport for quad
	// For menus, show fullscreen (or nearly fullscreen)
	float quadAspect = quad->size.width / quad->size.height;
	float screenW = (float)s.previewWidth;
	float screenH = (float)s.previewHeight;

	// Use most of the screen for menu display
	float displayW = screenW * 0.9f;
	float displayH = displayW / quadAspect;
	if (displayH > screenH * 0.9f) {
		displayH = screenH * 0.9f;
		displayW = displayH * quadAspect;
	}

	// Center on screen
	float displayX = (screenW - displayW) / 2.0f;
	float displayY = (screenH - displayH) / 2.0f;

	D3D11_VIEWPORT vp = { displayX, displayY, displayW, displayH, 0.0f, 1.0f };

	// Render the quad
	s.d3d11Context->RSSetViewports(1, &vp);
	s.d3d11Context->VSSetShader(s.blitVS.Get(), nullptr, 0);
	s.d3d11Context->PSSetShader(s.blitPS.Get(), nullptr, 0);

	ID3D11ShaderResourceView* srvs[] = { srv.Get() };
	s.d3d11Context->PSSetShaderResources(0, 1, srvs);
	ID3D11SamplerState* samplers[] = { s.samplerState.Get() };
	s.d3d11Context->PSSetSamplers(0, 1, samplers);

	s.d3d11Context->IASetInputLayout(nullptr);
	s.d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// Use alpha blending for overlay
	s.d3d11Context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	s.d3d11Context->OMSetDepthStencilState(nullptr, 0);
	s.d3d11Context->RSSetState(s.noCullRS.Get());

	ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
	s.d3d11Context->OMSetRenderTargets(1, rtvs, nullptr);

	s.d3d11Context->Draw(4, 0);

	// Cleanup
	ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
	s.d3d11Context->PSSetShaderResources(0, 1, nullSRV);
}

static XrResult XRAPI_PTR xrEndFrame_runtime(XrSession, const XrFrameEndInfo* info) {
	static int frameCount = 0;
	frameCount++;

	// Log every frame for first 10 frames, then every 60 frames
	bool shouldLog = (frameCount <= 10) || (frameCount % 60 == 1);

	if (shouldLog) {
		Logf("[OXRWXR] xrEndFrame called (frame #%d)", frameCount);
	}

	if (!info) {
		Log("[OXRWXR] xrEndFrame: ERROR - info is null");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	if (shouldLog) {
		Logf("[OXRWXR] xrEndFrame: layers=%u", info->layerCount);
	}

	// First pass: count layer types to know if we need to defer Present
	int projectionCount = 0, quadCount = 0, cylinderCount = 0, otherCount = 0;
	for (uint32_t i = 0; i < info->layerCount; ++i) {
		const XrCompositionLayerBaseHeader* base = info->layers[i];
		if (!base) continue;
		switch (base->type) {
		case XR_TYPE_COMPOSITION_LAYER_PROJECTION: projectionCount++; break;
		case XR_TYPE_COMPOSITION_LAYER_QUAD: quadCount++; break;
		case (XrStructureType)37: cylinderCount++; break; // XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR
		default: otherCount++; break;
		}
	}

	// Determine if we need to defer Present for overlay layers
	bool hasOverlays = (quadCount > 0 || cylinderCount > 0);
	g_presentPending = false;

	// Second pass: render projection layers (background)
	// If there are overlays, skip Present until after they're rendered
	for (uint32_t i = 0; i < info->layerCount; ++i) {
		const XrCompositionLayerBaseHeader* base = info->layers[i];
		if (!base) continue;

		if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
			const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(base);
			presentProjection(rt::g_session, *proj, hasOverlays);  // skipPresent if overlays pending
		}
	}

	// Third pass: render overlay layers (quad, cylinder) on top of the projection
	for (uint32_t i = 0; i < info->layerCount; ++i) {
		const XrCompositionLayerBaseHeader* base = info->layers[i];
		if (!base) continue;

		switch (base->type) {
		case XR_TYPE_COMPOSITION_LAYER_QUAD: {
			const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(base);
			renderQuadLayer(rt::g_session, quad);
			break;
		}
		case (XrStructureType)37: {
			// TODO: Implement cylinder layer rendering
			break;
		}
		default:
			break;
		}
	}

	// Now Present after all layers are rendered
	if (g_presentPending) {
		auto& s = rt::g_session;
		MSG msg;
		while (PeekMessageW(&msg, s.hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

		if (s.usesD3D12 && s.previewSwapchain12) {
			s.previewSwapchain12->Present(1, 0);
		}
		else if (s.previewSwapchain) {
			s.previewSwapchain->Present(1, 0);
		}
		g_presentPending = false;
	}

	if (shouldLog && (quadCount > 0 || cylinderCount > 0)) {
		Logf("[OXRWXR] xrEndFrame: proj=%d quad=%d cyl=%d other=%d",
			projectionCount, quadCount, cylinderCount, otherCount);
	}

	if (projectionCount == 0 && shouldLog) {
		Log("[OXRWXR] xrEndFrame: WARNING - No projection layers found!");
	}

	//// MCP Integration - write frame status for diagnostics
	//mcp::WriteFrameStatus(frameCount, rt::g_session.previewWidth, rt::g_session.previewHeight,
	//	"RGBA8", mcp::GetSessionStateName((int)rt::g_session.state),
	//	rt::g_headYaw, rt::g_headPitch,
	//	rt::g_headPos.x, rt::g_headPos.y, rt::g_headPos.z);

	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrLocateViews_runtime(XrSession, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* outCount, XrView* views) {
	if (outCount) *outCount = 2;
	if (vs) {
		vs->type = XR_TYPE_VIEW_STATE;
		// Set both VALID and TRACKED bits so Unity knows this is a real tracked HMD
		vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
			XR_VIEW_STATE_POSITION_VALID_BIT |
			XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
			XR_VIEW_STATE_POSITION_TRACKED_BIT;
	}
	if (cap < 2 || !views) return XR_SUCCESS;
	const float ipd = 0.064f;

	//----------------
	//OXRWXR CHANGE:
	//---------------- 
	//Now we have the real quat
	XrQuaternionf orientation = { HMDQuat.x, HMDQuat.y, HMDQuat.z, HMDQuat.w }; //rt::QuatFromYawPitchRoll(rt::g_headYaw, rt::g_headPitch, rt::g_headRoll);

	// Helper function to rotate a vector by a quaternion
	auto rotateVector = [](XrQuaternionf q, XrVector3f v) -> XrVector3f {
		// quaternion-vector rotation: q * v * q^-1
		XrQuaternionf qv{ v.x, v.y, v.z, 0 };
		XrQuaternionf qinv{ -q.x, -q.y, -q.z, q.w };

		// Quaternion multiplication: q * qv
		XrQuaternionf temp{
			q.w * qv.x + q.x * qv.w + q.y * qv.z - q.z * qv.y,
			q.w * qv.y - q.x * qv.z + q.y * qv.w + q.z * qv.x,
			q.w * qv.z + q.x * qv.y - q.y * qv.x + q.z * qv.w,
			q.w * qv.w - q.x * qv.x - q.y * qv.y - q.z * qv.z
		};

		// temp * qinv
		XrQuaternionf result{
			temp.w * qinv.x + temp.x * qinv.w + temp.y * qinv.z - temp.z * qinv.y,
			temp.w * qinv.y - temp.x * qinv.z + temp.y * qinv.w + temp.z * qinv.x,
			temp.w * qinv.z + temp.x * qinv.y - temp.y * qinv.x + temp.z * qinv.w,
			temp.w * qinv.w - temp.x * qinv.x - temp.y * qinv.y - temp.z * qinv.z
		};

		return XrVector3f{ result.x, result.y, result.z };
		};

	for (uint32_t i = 0; i < 2; ++i) {
		views[i].type = XR_TYPE_VIEW;
		views[i].pose.orientation = orientation;

		//XRTODO: IPD here
		// Apply IPD offset in full head orientation space (yaw+pitch)
		// This fixes stereo geometry and eliminates warping when pitching
		float eyeOffset = (i == 0 ? -ipd * 0.5f : ipd * 0.5f);
		XrVector3f localEyeOffset{ eyeOffset, 0.0f, 0.0f };
		XrVector3f rotatedOffset = rotateVector(orientation, localEyeOffset);

		views[i].pose.position = {
			rt::g_headPos.x + rotatedOffset.x,
			rt::g_headPos.y + rotatedOffset.y,
			rt::g_headPos.z + rotatedOffset.z
		};

		// Configurable FOV from UI settings
		// Convert degrees to tangent: tan(fovDegrees/2 * PI/180)
		// Safeguard: ensure fovDegrees is valid (default to 90 if not)
		int fovDeg = ui::g_uiState.fovDegrees;
		if (fovDeg <= 0 || fovDeg > 180) fovDeg = 90;
		float fovRadians = fovDeg * 0.5f * 3.14159265f / 180.0f;
		float fovTan = tanf(fovRadians);
		views[i].fov = { -fovTan, fovTan, fovTan, -fovTan };
	}
	static int locateCount = 0;
	if (++locateCount % 90 == 1) {  // Log every 90 frames (~1 second)
		Logf("[OXRWXR] xrLocateViews: pos=(%.2f,%.2f,%.2f) yaw=%.2f pitch=%.2f",
			rt::g_headPos.x, rt::g_headPos.y, rt::g_headPos.z,
			rt::g_headYaw, rt::g_headPitch);
	}
	return XR_SUCCESS;
}

// Add missing space/action functions for compatibility
static XrResult XRAPI_PTR xrCreateReferenceSpace_runtime(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
	if (!info || !space) return XR_ERROR_VALIDATION_FAILURE;
	static uintptr_t nextSpace = 100;
	*space = (XrSpace)(nextSpace++);
	Logf("[OXRWXR] xrCreateReferenceSpace: type=%d space=%p", info->referenceSpaceType, *space);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroySpace_runtime(XrSpace space) {
	Logf("[OXRWXR] xrDestroySpace: space=%p", space);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrLocateSpace_runtime(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
	if (!location) return XR_ERROR_VALIDATION_FAILURE;
	location->type = XR_TYPE_SPACE_LOCATION;

	// Check if this is a controller space
	auto it = rt::g_controllerSpaces.find(space);
	if (it != rt::g_controllerSpaces.end()) {
		int ctrlType = it->second;
		const rt::ControllerState& ctrl = (ctrlType == 1) ? rt::g_leftController : rt::g_rightController;

		if (ctrl.isTracking) {
			location->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
				XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
				XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
				XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
			rt::GetControllerPose(ctrl, &location->pose, (ctrlType != 1));

			// Handle velocity if chained (XrSpaceVelocity)
			XrSpaceVelocity* velocity = (XrSpaceVelocity*)location->next;
			if (velocity && velocity->type == XR_TYPE_SPACE_VELOCITY) {
				velocity->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
				// Return the calculated velocities from the controller state
				velocity->linearVelocity = ctrl.linearVelocity;
				velocity->angularVelocity = ctrl.angularVelocity;
			}

			static int logCount = 0;
			if (++logCount % 500 == 1) {
				float speed = sqrtf(ctrl.linearVelocity.x * ctrl.linearVelocity.x +
					ctrl.linearVelocity.y * ctrl.linearVelocity.y +
					ctrl.linearVelocity.z * ctrl.linearVelocity.z);
				Logf("[OXRWXR] xrLocateSpace: controller %d at (%.2f, %.2f, %.2f) vel=(%.2f, %.2f, %.2f) speed=%.2f m/s",
					ctrlType, location->pose.position.x, location->pose.position.y, location->pose.position.z,
					ctrl.linearVelocity.x, ctrl.linearVelocity.y, ctrl.linearVelocity.z, speed);
			}
		}
		else {
			location->locationFlags = 0;
			location->pose.orientation = { 0, 0, 0, 1 };
			location->pose.position = { 0, 0, 0 };
		}
	}
	else {
		// Default for non-controller spaces (identity pose)
		location->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		location->pose.orientation = { 0, 0, 0, 1 };
		location->pose.position = { 0, 0, 0 };
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateReferenceSpaces_runtime(XrSession, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* spaces) {
	if (count) *count = 3;
	if (capacity >= 3 && spaces) {
		spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
		spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
		spaces[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateActionSpace_runtime(XrSession, const XrActionSpaceCreateInfo* info, XrSpace* space) {
	if (!info || !space) return XR_ERROR_VALIDATION_FAILURE;
	static uintptr_t nextSpace = 200;
	*space = (XrSpace)(nextSpace++);

	// Detect controller subaction paths and register the space
	int controllerType = 0;  // 0=none, 1=left, 2=right
	Logf("[OXRWXR] xrCreateActionSpace: subactionPath=%llu, g_pathStrings.size()=%zu",
		(unsigned long long)info->subactionPath, rt::g_pathStrings.size());

	if (info->subactionPath != XR_NULL_PATH) {
		auto it = rt::g_pathStrings.find(info->subactionPath);
		if (it != rt::g_pathStrings.end()) {
			const std::string& pathStr = it->second;
			Logf("[OXRWXR] xrCreateActionSpace: found path='%s'", pathStr.c_str());
			if (pathStr.find("/user/hand/left") != std::string::npos) {
				controllerType = 1;  // Left controller
				Logf("[OXRWXR] xrCreateActionSpace: LEFT controller space %llu", (unsigned long long) * space);
			}
			else if (pathStr.find("/user/hand/right") != std::string::npos) {
				controllerType = 2;  // Right controller
				Logf("[OXRWXR] xrCreateActionSpace: RIGHT controller space %llu", (unsigned long long) * space);
			}
		}
		else {
			Logf("[OXRWXR] xrCreateActionSpace: path %llu NOT FOUND in g_pathStrings", (unsigned long long)info->subactionPath);
		}
	}
	else {
		Log("[OXRWXR] xrCreateActionSpace: subactionPath is XR_NULL_PATH");
	}

	if (controllerType > 0) {
		rt::g_controllerSpaces[*space] = controllerType;
	}

	Log("[OXRWXR] xrCreateActionSpace");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateActionSet_runtime(XrInstance, const XrActionSetCreateInfo* info, XrActionSet* set) {
	if (!info || !set) return XR_ERROR_VALIDATION_FAILURE;
	static uintptr_t nextSet = 300;
	*set = (XrActionSet)(nextSet++);
	// actionSetName may not be null-terminated
	char setName[XR_MAX_ACTION_SET_NAME_SIZE + 1] = { 0 };
	memcpy(setName, info->actionSetName, XR_MAX_ACTION_SET_NAME_SIZE);
	Logf("[OXRWXR] xrCreateActionSet: name=%s", setName);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroyActionSet_runtime(XrActionSet set) {
	Log("[OXRWXR] xrDestroyActionSet");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateAction_runtime(XrActionSet, const XrActionCreateInfo* info, XrAction* action) {
	if (!info || !action) return XR_ERROR_VALIDATION_FAILURE;
	static uintptr_t nextAction = 400;
	*action = (XrAction)(nextAction++);
	// actionName may not be null-terminated
	char actName[XR_MAX_ACTION_NAME_SIZE + 1] = { 0 };
	memcpy(actName, info->actionName, XR_MAX_ACTION_NAME_SIZE);
	Logf("[OXRWXR] xrCreateAction: name=%s, type=%d", actName, info->actionType);

	// Store action name for input mapping
	rt::g_actionNames[*action] = actName;

	// Detect which hand this action is bound to based on subactionPaths
	int handBinding = 0;  // 0=both/any
	if (info->countSubactionPaths > 0 && info->subactionPaths) {
		for (uint32_t i = 0; i < info->countSubactionPaths; i++) {
			auto it = rt::g_pathStrings.find(info->subactionPaths[i]);
			if (it != rt::g_pathStrings.end()) {
				if (it->second.find("left") != std::string::npos) handBinding |= 1;
				if (it->second.find("right") != std::string::npos) handBinding |= 2;
			}
		}
	}
	rt::g_actionHand[*action] = handBinding;

	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroyAction_runtime(XrAction action) {
	Log("[OXRWXR] xrDestroyAction");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrSuggestInteractionProfileBindings_runtime(XrInstance, const XrInteractionProfileSuggestedBinding* bindings) {
	if (!bindings) return XR_ERROR_VALIDATION_FAILURE;
	// interactionProfile is an XrPath (integer), not a C-string
	Logf("[OXRWXR] xrSuggestInteractionProfileBindings: profile=0x%llx",
		(unsigned long long)bindings->interactionProfile);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrAttachSessionActionSets_runtime(XrSession, const XrSessionActionSetsAttachInfo* info) {
	if (!info) return XR_ERROR_VALIDATION_FAILURE;
	Logf("[OXRWXR] xrAttachSessionActionSets: count=%u", info->countActionSets);
	return XR_SUCCESS;
}

// Helper to get controller state based on action and subactionPath
static rt::ControllerState* GetControllerForAction(XrAction action, XrPath subactionPath) {
	// First check subactionPath
	if (subactionPath != XR_NULL_PATH) {
		auto pathIt = rt::g_pathStrings.find(subactionPath);
		if (pathIt != rt::g_pathStrings.end()) {
			if (pathIt->second.find("left") != std::string::npos) return &rt::g_leftController;
			if (pathIt->second.find("right") != std::string::npos) return &rt::g_rightController;
		}
	}
	// Fall back to action's hand binding
	auto handIt = rt::g_actionHand.find(action);
	if (handIt != rt::g_actionHand.end()) {
		if (handIt->second == 1) return &rt::g_leftController;
		if (handIt->second == 2) return &rt::g_rightController;
	}
	// Default to right hand
	return &rt::g_rightController;
}

// Helper to check if action name matches input type (case-insensitive substring match)
static bool ActionNameMatches(const std::string& name, const char* pattern) {
	std::string lower = name;
	for (auto& c : lower) c = (char)tolower(c);
	std::string patLower = pattern;
	for (auto& c : patLower) c = (char)tolower(c);
	return lower.find(patLower) != std::string::npos;
}

static XrResult XRAPI_PTR xrGetActionStateBoolean_runtime(XrSession, const XrActionStateGetInfo* info, XrActionStateBoolean* state) {
	if (!info || !state) return XR_ERROR_VALIDATION_FAILURE;
	state->type = XR_TYPE_ACTION_STATE_BOOLEAN;
	state->changedSinceLastSync = XR_FALSE;
	state->lastChangeTime = 0;

	// Get controller for this action
	rt::ControllerState* ctrl = GetControllerForAction(info->action, info->subactionPath);

	// Get action name to determine input type
	bool buttonState = false;
	auto nameIt = rt::g_actionNames.find(info->action);
	if (nameIt != rt::g_actionNames.end()) {
		const std::string& name = nameIt->second;
		if (ActionNameMatches(name, "trigger") || ActionNameMatches(name, "select") || ActionNameMatches(name, "fire")) {
			buttonState = ctrl->triggerPressed;
		}
		else if (ActionNameMatches(name, "grip") || ActionNameMatches(name, "squeeze") || ActionNameMatches(name, "grab")) {
			buttonState = ctrl->gripPressed;
		}
		else if (ActionNameMatches(name, "menu")) {
			buttonState = ctrl->menuPressed;
		}
		else if (ActionNameMatches(name, "primary") || ActionNameMatches(name, "a_button") || ActionNameMatches(name, "x_button")) {
			buttonState = ctrl->primaryPressed;
		}
		else if (ActionNameMatches(name, "secondary") || ActionNameMatches(name, "b_button") || ActionNameMatches(name, "y_button")) {
			buttonState = ctrl->secondaryPressed;
		}
		else if (ActionNameMatches(name, "thumbstick") || ActionNameMatches(name, "joystick")) {
			buttonState = ctrl->thumbstickPressed;
		}
	}

	state->currentState = buttonState ? XR_TRUE : XR_FALSE;
	state->isActive = XR_TRUE;  // Controllers are always active in simulator
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetActionStateFloat_runtime(XrSession, const XrActionStateGetInfo* info, XrActionStateFloat* state) {
	if (!info || !state) return XR_ERROR_VALIDATION_FAILURE;
	state->type = XR_TYPE_ACTION_STATE_FLOAT;
	state->changedSinceLastSync = XR_FALSE;
	state->lastChangeTime = 0;

	// Get controller for this action
	rt::ControllerState* ctrl = GetControllerForAction(info->action, info->subactionPath);

	// Get action name to determine input type
	float floatState = 0.0f;
	auto nameIt = rt::g_actionNames.find(info->action);
	if (nameIt != rt::g_actionNames.end()) {
		const std::string& name = nameIt->second;
		if (ActionNameMatches(name, "trigger") || ActionNameMatches(name, "select") || ActionNameMatches(name, "fire")) {
			floatState = ctrl->triggerValue;
		}
		else if (ActionNameMatches(name, "grip") || ActionNameMatches(name, "squeeze") || ActionNameMatches(name, "grab")) {
			floatState = ctrl->gripValue;
		}
	}

	state->currentState = floatState;
	state->isActive = XR_TRUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetActionStatePose_runtime(XrSession, const XrActionStateGetInfo* info, XrActionStatePose* state) {
	if (!info || !state) return XR_ERROR_VALIDATION_FAILURE;
	state->type = XR_TYPE_ACTION_STATE_POSE;
	state->isActive = XR_TRUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetActionStateVector2f_runtime(XrSession, const XrActionStateGetInfo* info, XrActionStateVector2f* state) {
	if (!info || !state) return XR_ERROR_VALIDATION_FAILURE;
	state->type = XR_TYPE_ACTION_STATE_VECTOR2F;
	state->changedSinceLastSync = XR_FALSE;
	state->lastChangeTime = 0;

	// Get controller for this action
	rt::ControllerState* ctrl = GetControllerForAction(info->action, info->subactionPath);

	// Get action name to determine input type
	auto nameIt = rt::g_actionNames.find(info->action);
	if (nameIt != rt::g_actionNames.end()) {
		const std::string& name = nameIt->second;
		if (ActionNameMatches(name, "thumbstick") || ActionNameMatches(name, "joystick") ||
			ActionNameMatches(name, "move") || ActionNameMatches(name, "turn")) {
			state->currentState = ctrl->thumbstick;
		}
		else {
			state->currentState = { 0.0f, 0.0f };
		}
	}
	else {
		state->currentState = { 0.0f, 0.0f };
	}

	state->isActive = XR_TRUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrSyncActions_runtime(XrSession, const XrActionsSyncInfo* info) {
	if (!info) return XR_ERROR_VALIDATION_FAILURE;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrStringToPath_runtime(XrInstance, const char* pathString, XrPath* path) {
	if (!pathString || !path) return XR_ERROR_VALIDATION_FAILURE;
	// Simple hash as path ID
	size_t hash = 5381;
	for (const char* c = pathString; *c; ++c) {
		hash = ((hash << 5) + hash) + *c;
	}
	*path = (XrPath)hash;
	// Store path string for controller detection
	rt::g_pathStrings[*path] = pathString;
	Logf("[OXRWXR] xrStringToPath: %s -> %llu", pathString, (unsigned long long) * path);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrPathToString_runtime(XrInstance, XrPath path, uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer) {
	const char* str = "/unknown/path";
	size_t len = strlen(str) + 1;
	if (bufferCountOutput) *bufferCountOutput = (uint32_t)len;
	if (buffer && bufferCapacityInput > 0) {
		strncpy(buffer, str, bufferCapacityInput - 1);
		buffer[bufferCapacityInput - 1] = '\0';
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetCurrentInteractionProfile_runtime(XrSession, XrPath topLevelUserPath, XrInteractionProfileState* interactionProfile) {
	if (!interactionProfile) return XR_ERROR_VALIDATION_FAILURE;
	interactionProfile->type = XR_TYPE_INTERACTION_PROFILE_STATE;
	interactionProfile->interactionProfile = XR_NULL_PATH;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateBoundSourcesForAction_runtime(XrSession, const XrBoundSourcesForActionEnumerateInfo* info, uint32_t sourceCapacityInput, uint32_t* sourceCountOutput, XrPath* sources) {
	if (sourceCountOutput) *sourceCountOutput = 0;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetInputSourceLocalizedName_runtime(XrSession, const XrInputSourceLocalizedNameGetInfo* info, uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer) {
	const char* name = "Unknown";
	size_t len = strlen(name) + 1;
	if (bufferCountOutput) *bufferCountOutput = (uint32_t)len;
	if (buffer && bufferCapacityInput > 0) {
		strncpy(buffer, name, bufferCapacityInput - 1);
		buffer[bufferCapacityInput - 1] = '\0';
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroySwapchain_runtime(XrSwapchain sc) {
	auto it = rt::g_swapchains.find(sc);
	if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;

	// For OpenGL swapchains, delete the textures
	if (it->second.backend == rt::Swapchain::Backend::OpenGL && !it->second.imagesGL.empty()) {
		// Make app's GL context current if available
		HGLRC prevRC = wglGetCurrentContext();
		HDC prevDC = wglGetCurrentDC();
		if (rt::g_session.glDC && rt::g_session.glRC) {
			wglMakeCurrent(rt::g_session.glDC, rt::g_session.glRC);
		}
		for (GLuint tex : it->second.imagesGL) {
			glDeleteTextures(1, &tex);
		}
		if (prevRC) wglMakeCurrent(prevDC, prevRC);
	}

	rt::g_swapchains.erase(it);
	Logf("[OXRWXR] xrDestroySwapchain: sc=%p", sc);
	return XR_SUCCESS;
}

// Missing functions Unity needs
static XrResult XRAPI_PTR xrResultToString_runtime(XrInstance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
	const char* str = "XR_SUCCESS";
	if (value != XR_SUCCESS) str = "XR_ERROR";
	strncpy(buffer, str, XR_MAX_RESULT_STRING_SIZE - 1);
	buffer[XR_MAX_RESULT_STRING_SIZE - 1] = '\0';
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrStructureTypeToString_runtime(XrInstance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
	snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XrStructureType_%d", (int)value);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetReferenceSpaceBoundsRect_runtime(XrSession, XrReferenceSpaceType, XrExtent2Df* bounds) {
	if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
	bounds->width = 3.0f;
	bounds->height = 3.0f;
	Log("[OXRWXR] xrGetReferenceSpaceBoundsRect: 3x3 meters");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetViewConfigurationProperties_runtime(XrInstance, XrSystemId, XrViewConfigurationType type,
	XrViewConfigurationProperties* props) {
	if (!props) return XR_ERROR_VALIDATION_FAILURE;
	props->type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
	props->viewConfigurationType = type;
	props->fovMutable = XR_FALSE;
	Log("[OXRWXR] xrGetViewConfigurationProperties");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrApplyHapticFeedback_runtime(XrSession, const XrHapticActionInfo* info, const XrHapticBaseHeader* haptic) {
	// Just stub for now
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrStopHapticFeedback_runtime(XrSession, const XrHapticActionInfo* info) {
	// Just stub for now
	return XR_SUCCESS;
}

// Time conversion functions for XR_KHR_win32_convert_performance_counter_time
static XrResult XRAPI_PTR xrConvertWin32PerformanceCounterToTimeKHR_runtime(XrInstance instance,
	const LARGE_INTEGER* performanceCounter,
	XrTime* time) {
	if (!performanceCounter || !time) return XR_ERROR_VALIDATION_FAILURE;

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);

	// Convert to nanoseconds
	*time = (performanceCounter->QuadPart * 1000000000) / freq.QuadPart;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrConvertTimeToWin32PerformanceCounterKHR_runtime(XrInstance instance,
	XrTime time,
	LARGE_INTEGER* performanceCounter) {
	if (!performanceCounter) return XR_ERROR_VALIDATION_FAILURE;

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);

	// Convert from nanoseconds  
	performanceCounter->QuadPart = (time * freq.QuadPart) / 1000000000;
	return XR_SUCCESS;
}

// ----------------------------------------------

struct NameFn { const char* name; PFN_xrVoidFunction fn; };

static const NameFn kFnTable[] = {
	{"xrGetInstanceProcAddr", (PFN_xrVoidFunction)xrGetInstanceProcAddr_runtime},
	{"xrEnumerateApiLayerProperties", (PFN_xrVoidFunction)xrEnumerateApiLayerProperties_runtime},
	{"xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction)xrEnumerateInstanceExtensionProperties_runtime},
	{"xrCreateInstance", (PFN_xrVoidFunction)xrCreateInstance_runtime},
	{"xrDestroyInstance", (PFN_xrVoidFunction)xrDestroyInstance_runtime},
	{"xrGetInstanceProperties", (PFN_xrVoidFunction)xrGetInstanceProperties_runtime},
	{"xrGetSystem", (PFN_xrVoidFunction)xrGetSystem_runtime},
	{"xrGetSystemProperties", (PFN_xrVoidFunction)xrGetSystemProperties_runtime},
	{"xrEnumerateViewConfigurations", (PFN_xrVoidFunction)xrEnumerateViewConfigurations_runtime},
	{"xrEnumerateViewConfigurationViews", (PFN_xrVoidFunction)xrEnumerateViewConfigurationViews_runtime},
	{"xrEnumerateEnvironmentBlendModes", (PFN_xrVoidFunction)xrEnumerateEnvironmentBlendModes_runtime},
	{"xrCreateSession", (PFN_xrVoidFunction)xrCreateSession_runtime},
	{"xrDestroySession", (PFN_xrVoidFunction)xrDestroySession_runtime},
	{"xrEnumerateSwapchainFormats", (PFN_xrVoidFunction)xrEnumerateSwapchainFormats_runtime},
	{"xrCreateSwapchain", (PFN_xrVoidFunction)xrCreateSwapchain_runtime},
	{"xrDestroySwapchain", (PFN_xrVoidFunction)xrDestroySwapchain_runtime},
	{"xrEnumerateSwapchainImages", (PFN_xrVoidFunction)xrEnumerateSwapchainImages_runtime},
	{"xrAcquireSwapchainImage", (PFN_xrVoidFunction)xrAcquireSwapchainImage_runtime},
	{"xrWaitSwapchainImage", (PFN_xrVoidFunction)xrWaitSwapchainImage_runtime},
	{"xrReleaseSwapchainImage", (PFN_xrVoidFunction)xrReleaseSwapchainImage_runtime},
	{"xrBeginSession", (PFN_xrVoidFunction)xrBeginSession_runtime},
	{"xrEndSession", (PFN_xrVoidFunction)xrEndSession_runtime},
	{"xrWaitFrame", (PFN_xrVoidFunction)xrWaitFrame_runtime},
	{"xrBeginFrame", (PFN_xrVoidFunction)xrBeginFrame_runtime},
	{"xrEndFrame", (PFN_xrVoidFunction)xrEndFrame_runtime},
	{"xrPollEvent", (PFN_xrVoidFunction)xrPollEvent_runtime},
	{"xrLocateViews", (PFN_xrVoidFunction)xrLocateViews_runtime},
	{"xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction)xrGetD3D11GraphicsRequirementsKHR_runtime},
	{"xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction)xrGetD3D12GraphicsRequirementsKHR_runtime},
	{"xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction)xrGetOpenGLGraphicsRequirementsKHR_runtime},
	{"xrRequestExitSession", (PFN_xrVoidFunction)xrRequestExitSession_runtime},
	// Space functions
	{"xrCreateReferenceSpace", (PFN_xrVoidFunction)xrCreateReferenceSpace_runtime},
	{"xrDestroySpace", (PFN_xrVoidFunction)xrDestroySpace_runtime},
	{"xrLocateSpace", (PFN_xrVoidFunction)xrLocateSpace_runtime},
	{"xrEnumerateReferenceSpaces", (PFN_xrVoidFunction)xrEnumerateReferenceSpaces_runtime},
	{"xrCreateActionSpace", (PFN_xrVoidFunction)xrCreateActionSpace_runtime},
	// Action functions
	{"xrCreateActionSet", (PFN_xrVoidFunction)xrCreateActionSet_runtime},
	{"xrDestroyActionSet", (PFN_xrVoidFunction)xrDestroyActionSet_runtime},
	{"xrCreateAction", (PFN_xrVoidFunction)xrCreateAction_runtime},
	{"xrDestroyAction", (PFN_xrVoidFunction)xrDestroyAction_runtime},
	{"xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)xrSuggestInteractionProfileBindings_runtime},
	{"xrAttachSessionActionSets", (PFN_xrVoidFunction)xrAttachSessionActionSets_runtime},
	{"xrGetActionStateBoolean", (PFN_xrVoidFunction)xrGetActionStateBoolean_runtime},
	{"xrGetActionStateFloat", (PFN_xrVoidFunction)xrGetActionStateFloat_runtime},
	{"xrGetActionStatePose", (PFN_xrVoidFunction)xrGetActionStatePose_runtime},
	{"xrGetActionStateVector2f", (PFN_xrVoidFunction)xrGetActionStateVector2f_runtime},
	{"xrSyncActions", (PFN_xrVoidFunction)xrSyncActions_runtime},
	// Path functions
	{"xrStringToPath", (PFN_xrVoidFunction)xrStringToPath_runtime},
	{"xrPathToString", (PFN_xrVoidFunction)xrPathToString_runtime},
	// Interaction functions
	{"xrGetCurrentInteractionProfile", (PFN_xrVoidFunction)xrGetCurrentInteractionProfile_runtime},
	{"xrEnumerateBoundSourcesForAction", (PFN_xrVoidFunction)xrEnumerateBoundSourcesForAction_runtime},
	{"xrGetInputSourceLocalizedName", (PFN_xrVoidFunction)xrGetInputSourceLocalizedName_runtime},
	// Utility functions
	{"xrResultToString", (PFN_xrVoidFunction)xrResultToString_runtime},
	{"xrStructureTypeToString", (PFN_xrVoidFunction)xrStructureTypeToString_runtime},
	{"xrGetReferenceSpaceBoundsRect", (PFN_xrVoidFunction)xrGetReferenceSpaceBoundsRect_runtime},
	{"xrGetViewConfigurationProperties", (PFN_xrVoidFunction)xrGetViewConfigurationProperties_runtime},
	// Haptic functions
	{"xrApplyHapticFeedback", (PFN_xrVoidFunction)xrApplyHapticFeedback_runtime},
	{"xrStopHapticFeedback", (PFN_xrVoidFunction)xrStopHapticFeedback_runtime},
	// Time conversion functions
	{"xrConvertWin32PerformanceCounterToTimeKHR", (PFN_xrVoidFunction)xrConvertWin32PerformanceCounterToTimeKHR_runtime},
	{"xrConvertTimeToWin32PerformanceCounterKHR", (PFN_xrVoidFunction)xrConvertTimeToWin32PerformanceCounterKHR_runtime},
};

static XrResult XRAPI_PTR xrGetInstanceProcAddr_runtime(XrInstance instance, const char* name, PFN_xrVoidFunction* fn) {
	if (!name || !fn) {
		Logf("[OXRWXR] xrGetInstanceProcAddr: ERROR - name=%p, fn=%p", name, fn);
		return XR_ERROR_VALIDATION_FAILURE;
	}

	// Reduce logging verbosity for xrGetInstanceProcAddr
	static bool reduceLogging = false;
	static int callCount = 0;
	callCount++;

	for (auto& e : kFnTable) {
		if (strcmp(name, e.name) == 0) {
			*fn = e.fn;
			if (callCount < 100 || strstr(name, "D3D11") || strstr(name, "Create") || strstr(name, "Destroy")) {
				Logf("[OXRWXR] xrGetInstanceProcAddr: %s -> FOUND", name);
			}
			return XR_SUCCESS;
		}
	}

	if (callCount < 100 || strstr(name, "D3D11")) {
		Logf("[OXRWXR] xrGetInstanceProcAddr: %s -> NOT FOUND", name);
	}
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}
