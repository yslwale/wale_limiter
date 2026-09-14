// wale_limiter - a ReShade addon that hard-caps the frame rate to 60 FPS.
// Built for the Underground Racing server.
//
// Copyright (C) 2026 Nipeno
// Copyright (C) 2026 bjj_dev
// Copyright (C) 2026 wale
//
// Modified by wale in 2026 from an earlier GPLv3 frame limiter by the copyright holders above:
// new name, logo, links and config section, and an English / Hungarian language switch.
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY. See the GNU General
// Public License (LICENSE file) for details: https://www.gnu.org/licenses/
//
// Build target: single DLL named wale_limiter.addon64 (Windows x64).
// Requires the ADDON-ENABLED build of ReShade.
//
// How it works:
//  - reshade::addon_event::present fires once per frame. We measure the time since
//    the previous present and, when the limiter is enabled, block until exactly one
//    60 FPS frame interval has elapsed using a high-resolution waitable timer for the
//    bulk of the wait plus a short busy-wait for sub-millisecond accuracy.
//  - The overlay callback draws a dedicated "wale_limiter" window. ReShade
//    remembers that window's position, size and dock slot for us, in ReShade.ini.
//  - The checkbox state and the UI language are persisted via ReShade's own config
//    (no custom file). Two flag buttons switch the window between English and Hungarian.
//  - Startup, initialisation and failures are written to ReShade.log, so a user can
//    send that one file when asking for support. Routine detail is logged at DEBUG;
//    ReShade writes every level, so the level is severity labelling, not filtering.

// ImGui only made ImTextureID default to ImU64 in 1.92. We build against 1.90.4 (19040) so the
// addon targets ReShade addon API 11 and loads on the older ReShade that graphics packs ship, and
// there ImTextureID is void* - which a resource_view handle (uint64_t) cannot static_cast to.
// ReShade's overlay header asserts this exact define is present; imgui.h guards its typedef.
#define ImTextureID ImU64

#include <imgui.h>          // Must be included BEFORE reshade.hpp so the overlay wrappers compile.
#include <reshade.hpp>
#include "logo_data.h"      // Embedded wale-limiter-logo.png bytes (g_logo_png / g_logo_png_len)
#include "flags_data.h"     // Embedded flag-en.png / flag-hu.png bytes for the language buttons

#include <Windows.h>
#include <shellapi.h>       // ShellExecuteA (open Discord / source links)
#include <intrin.h>         // _mm_pause (spin-wait hint)
#include <wincodec.h>       // WIC: decode the embedded PNGs (system component, no extra dep)
#include <wrl/client.h>     // Microsoft::WRL::ComPtr
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <cstring>          // strcmp
#include <cstdarg>          // Variadic logging helper
#include <cstdio>           // vsnprintf

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Hardcoded cap. Exactly 60.000 FPS -> one frame every 1/60 second.
static constexpr double kTargetFps = 60.0;
static constexpr std::chrono::duration<double> kFrameInterval{ 1.0 / kTargetFps };

// Last slice we busy-wait instead of sleeping, for sub-millisecond frame accuracy.
// The coarse wait (high-res timer or sleep) lands within ~0.5 ms; the spin nails it.
static constexpr std::chrono::microseconds kSpinMargin{ 500 };

// Some older SDK headers lack this flag; define it so the build never depends on it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static constexpr const char *kConfigSection = "wale_limiter";
static constexpr const char *kConfigKey     = "LimitTo60";
static constexpr const char *kLanguageKey   = "Language";

// Injected by CMake / build.ps1 (-DWALE_SOURCE_URL). Must point at the repository holding the
// exact source of this build: GPLv3 requires it, and the "View Source" button opens it.
#ifndef WALE_SOURCE_URL
#define WALE_SOURCE_URL "https://github.com/yslwale/wale_limiter"
#endif

static constexpr const char *kDiscordUrl = "https://discord.gg/undergroundracing";
static constexpr const char *kSourceUrl  = WALE_SOURCE_URL;

// Injected by CMake (-DWALE_VERSION). Fallback keeps non-CMake/standalone builds compiling.
#ifndef WALE_VERSION_STR
#define WALE_VERSION_STR "0.0.0"
#endif

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

// Every string the window shows, per language. The "###id" suffixes keep ImGui's widget IDs
// stable when the visible label changes language. The source is compiled as UTF-8 (/utf-8).
// Hungarian text sticks to Latin-1 letters (á é í ó ö ú ü): ő and ű are outside it, and
// ReShade's overlay font atlas is not guaranteed to contain them.
enum language : int { lang_en, lang_hu, lang_count };

struct ui_text
{
	const char *code;       // Saved as [wale_limiter] Language=<code> in ReShade.ini
	const char *name;       // Flag tooltip, in its own language
	const char *fallback;   // Text button used if the flag image fails to load
	const char *limit;
	const char *built_for;
	const char *version;    // printf format, %s = version
	const char *discord;
	const char *source;
};

static const ui_text kText[lang_count] = {
	{ "en", "English", "EN###lang_en",
	  "Limit to 60 FPS###limit",
	  "Built for Underground Racing",
	  "Version %s",
	  "Join the Discord###discord",
	  "View Source on GitHub###source" },
	{ "hu", "Magyar", "HU###lang_hu",
	  "Korlátozás 60 FPS-re###limit",
	  "Az Underground Racing számára fejlesztve",
	  "Verzió %s",
	  "Csatlakozz a Discordhoz###discord",
	  "Forráskód a GitHubon###source" },
};

static const unsigned char *const kFlagPng[lang_count]    = { g_flag_en_png, g_flag_hu_png };
static const unsigned int         kFlagPngLen[lang_count] = { g_flag_en_png_len, g_flag_hu_png_len };

// First run (nothing saved yet): follow the Windows display language.
static int default_language()
{
	return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_HUNGARIAN ? lang_hu : lang_en;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// printf-style wrapper around reshade::log_message; the line lands in ReShade.log,
// prefixed with the add-on name. ReShade has no runtime log level - ReShadeLogMessage
// writes whatever it is given (source/addon.cpp -> reshade::log::message) - so the
// level here labels severity for whoever reads the log, it does not filter anything.
// That is exactly why nothing is logged per frame.
static void wale_log(reshade::log_level level, const char *fmt, ...)
{
	char buf[512];

	va_list args;
	va_start(args, fmt);
	const int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (written < 0)
		return; // Formatting failed; nothing useful to log.

	reshade::log_message(level, buf);
}

// Human-readable graphics API, so a user's log says which renderer we attached to.
static const char *device_api_name(reshade::api::device_api api)
{
	switch (api)
	{
	case reshade::api::device_api::d3d9:   return "D3D9";
	case reshade::api::device_api::d3d10:  return "D3D10";
	case reshade::api::device_api::d3d11:  return "D3D11";
	case reshade::api::device_api::d3d12:  return "D3D12";
	case reshade::api::device_api::opengl: return "OpenGL";
	case reshade::api::device_api::vulkan: return "Vulkan";
	default:                               return "unknown";
	}
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Read in the present callback every frame; toggled in the overlay callback.
static std::atomic<bool> g_limit_enabled{ false };

// Current UI language. Only touched on the render thread (init + overlay callbacks).
static int g_language = lang_en;

// Set on the first present we ever see, so the log can prove the pacing callback
// really runs (as opposed to "add-on registered, but events never reach us").
// Atomic because a process can drive more than one swapchain.
static std::atomic<bool> g_first_present_logged{ false };

using clock_type = std::chrono::high_resolution_clock;
static clock_type::time_point g_last_present = clock_type::now();

// High-resolution waitable timer for the coarse wait. Null -> sleep_for fallback
// (older OS or unsupported flag); timeBeginPeriod(1) keeps that path tight.
static HANDLE g_timer = nullptr;

// Images shown in the window. Created in init_effect_runtime, freed in destroy_effect_runtime.
// A zero view handle means "not loaded": the logo then falls back to a bordered placeholder,
// a flag to a text button.
struct overlay_texture
{
	reshade::api::resource      resource = { 0 };
	reshade::api::resource_view view = { 0 };
	uint32_t width = 0;
	uint32_t height = 0;
};

static reshade::api::device *g_texture_device = nullptr;
static overlay_texture g_logo;
static overlay_texture g_flags[lang_count];

// ---------------------------------------------------------------------------
// Frame pacing
// ---------------------------------------------------------------------------

// Hybrid wait + busy-wait. A plain Sleep() stutters because of Windows timer
// granularity, so we wait until ~0.5 ms before the target with a high-resolution
// waitable timer (cheap, no CPU burn) and spin only the final slice.
static void on_present(reshade::api::command_queue *, reshade::api::swapchain *,
                       const reshade::api::rect *, const reshade::api::rect *,
                       uint32_t, const reshade::api::rect *)
{
	// One line, once per process: past this point the frame pacing path is live.
	if (!g_first_present_logged.exchange(true, std::memory_order_relaxed))
		wale_log(reshade::log_level::info, "First frame presented; limiter %s.",
		         g_limit_enabled.load(std::memory_order_relaxed) ? "enabled" : "disabled");

	if (!g_limit_enabled.load(std::memory_order_relaxed))
	{
		// No cap: just keep the timestamp fresh so re-enabling does not over-sleep.
		g_last_present = clock_type::now();
		return;
	}

	const clock_type::time_point target     = g_last_present + std::chrono::duration_cast<clock_type::duration>(kFrameInterval);
	const clock_type::time_point spin_start  = target - std::chrono::duration_cast<clock_type::duration>(kSpinMargin);

	// Coarse phase: block until ~0.5 ms before the target without burning a core.
	for (;;)
	{
		const clock_type::time_point now = clock_type::now();
		if (now >= spin_start)
			break;

		const auto remaining = spin_start - now;
		if (g_timer != nullptr)
		{
			// Negative due time = relative, in 100 ns units. One wait is enough;
			// the loop just re-checks the clock in case of an early wake.
			LARGE_INTEGER due;
			due.QuadPart = -(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
			if (due.QuadPart < 0 && SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0))
				WaitForSingleObject(g_timer, INFINITE);
			else
				break; // Sub-100 ns left, or the timer failed; spin handles it.
		}
		else
		{
			std::this_thread::sleep_for(remaining); // Fallback: timeBeginPeriod(1) keeps this ~1 ms.
		}
	}

	// Fine phase: busy-wait the last slice for frame-accurate pacing.
	while (clock_type::now() < target)
		_mm_pause();

	// Drift compensation: anchor the next frame to the ideal target so per-frame
	// overshoot does not accumulate into a slow drift below 60 FPS. But if we fell
	// more than a full frame behind (alt-tab, hitch, loading stall), drop the debt
	// and re-anchor to now so we never burst-render to "catch up".
	const clock_type::time_point now = clock_type::now();
	const clock_type::time_point one_frame_late = target + std::chrono::duration_cast<clock_type::duration>(kFrameInterval);
	g_last_present = (now > one_frame_late) ? now : target;
}

// ---------------------------------------------------------------------------
// Image loading (logo + flags)
// ---------------------------------------------------------------------------

// Decode an embedded PNG to tightly-packed 32-bit RGBA using WIC (a Windows system
// component). The images are baked into the DLL, so there is no external file to ship
// or expose. Returns false if decoding fails.
static bool decode_png_rgba(const unsigned char *png, unsigned int png_len,
                            std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height)
{
	using Microsoft::WRL::ComPtr;

	// COM may already be initialised by the game; tolerate a different threading model.
	const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool co_owned = SUCCEEDED(co);

	bool ok = false;
	{
		ComPtr<IWICImagingFactory> factory;
		ComPtr<IWICStream> stream;
		ComPtr<IWICBitmapDecoder> decoder;
		ComPtr<IWICBitmapFrameDecode> frame;
		ComPtr<IWICFormatConverter> converter;

		if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
		    SUCCEEDED(factory->CreateStream(&stream)) &&
		    SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE *>(png), png_len)) &&
		    SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
		    SUCCEEDED(decoder->GetFrame(0, &frame)) &&
		    SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
		    SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		{
			UINT w = 0, h = 0;
			if (SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0)
			{
				const UINT stride = w * 4;
				pixels.resize(static_cast<size_t>(stride) * h);
				if (SUCCEEDED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
				{
					width = w;
					height = h;
					ok = true;
				}
			}
		}
	}

	if (co_owned)
		CoUninitialize();
	return ok;
}

// Decode one embedded PNG and upload it as a shader-readable texture on the runtime's device.
// `what` names the image in the log if anything fails.
static bool create_texture(reshade::api::device *device, const unsigned char *png, unsigned int png_len,
                           overlay_texture &tex, const char *what)
{
	std::vector<uint8_t> pixels;
	uint32_t w = 0, h = 0;
	if (!decode_png_rgba(png, png_len, pixels, w, h))
	{
		wale_log(reshade::log_level::warning, "%s: PNG decode failed.", what);
		return false;
	}

	const reshade::api::resource_desc desc(
		w, h, 1, 1, reshade::api::format::r8g8b8a8_unorm, 1,
		reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::shader_resource);

	const reshade::api::subresource_data initial{ pixels.data(), w * 4u, w * h * 4u };

	reshade::api::resource res = { 0 };
	if (!device->create_resource(desc, &initial, reshade::api::resource_usage::shader_resource, &res))
	{
		wale_log(reshade::log_level::warning, "%s: texture creation failed (resource).", what);
		return false;
	}

	reshade::api::resource_view view = { 0 };
	if (!device->create_resource_view(res, reshade::api::resource_usage::shader_resource,
	                                  reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm), &view))
	{
		wale_log(reshade::log_level::warning, "%s: texture creation failed (view).", what);
		device->destroy_resource(res);
		return false;
	}

	tex.resource = res;
	tex.view = view;
	tex.width = w;
	tex.height = h;
	return true;
}

static void load_textures(reshade::api::effect_runtime *runtime)
{
	g_texture_device = runtime->get_device();

	if (!create_texture(g_texture_device, g_logo_png, g_logo_png_len, g_logo, "Logo"))
		wale_log(reshade::log_level::warning, "Drawing the logo placeholder instead.");

	for (int i = 0; i < lang_count; ++i)
		if (!create_texture(g_texture_device, kFlagPng[i], kFlagPngLen[i], g_flags[i], kText[i].name))
			wale_log(reshade::log_level::warning, "Using a text button for %s instead of its flag.", kText[i].name);
}

static void free_texture(overlay_texture &tex)
{
	if (g_texture_device != nullptr)
	{
		if (tex.view.handle != 0)
			g_texture_device->destroy_resource_view(tex.view);
		if (tex.resource.handle != 0)
			g_texture_device->destroy_resource(tex.resource);
	}
	tex = overlay_texture{};
}

static void free_textures()
{
	free_texture(g_logo);
	for (overlay_texture &flag : g_flags)
		free_texture(flag);
	g_texture_device = nullptr;
}

// ---------------------------------------------------------------------------
// Config persistence (ReShade config, not a custom file)
// ---------------------------------------------------------------------------

static void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
	wale_log(reshade::log_level::info, "Effect runtime initialised (device API: %s).",
	         device_api_name(runtime->get_device()->get_api()));

	bool value = false;
	if (reshade::get_config_value(runtime, kConfigSection, kConfigKey, value))
	{
		g_limit_enabled.store(value, std::memory_order_relaxed);
		wale_log(reshade::log_level::debug, "Loaded %s=%d from config.", kConfigKey, value ? 1 : 0);
	}
	else
	{
		wale_log(reshade::log_level::debug, "No saved on/off choice; limiter starts disabled.");
	}

	g_language = default_language();
	char code[8] = "";
	size_t code_size = sizeof(code);
	if (reshade::get_config_value(runtime, kConfigSection, kLanguageKey, code, &code_size))
	{
		code[sizeof(code) - 1] = '\0';
		for (int i = 0; i < lang_count; ++i)
			if (std::strcmp(code, kText[i].code) == 0)
				g_language = i;
	}
	wale_log(reshade::log_level::debug, "UI language: %s.", kText[g_language].code);

	load_textures(runtime);
}

static void on_destroy_effect_runtime(reshade::api::effect_runtime *)
{
	wale_log(reshade::log_level::debug, "Effect runtime destroyed.");
	free_textures();
}

// ---------------------------------------------------------------------------
// Overlay drawing
// ---------------------------------------------------------------------------

// Draws the real logo texture if one was loaded; otherwise a bordered
// "[ wale_limiter logo ]" placeholder at a fixed 200x80 size.
static void draw_logo()
{
	if (g_logo.view.handle != 0)
	{
		// Fit the logo to a 200px width, preserving aspect ratio.
		const float target_w = 200.0f;
		const float scale = target_w / static_cast<float>(g_logo.width);
		ImGui::Image(static_cast<ImTextureID>(g_logo.view.handle),
		             ImVec2(target_w, static_cast<float>(g_logo.height) * scale));
		return;
	}

	const ImVec2 size(200.0f, 80.0f);
	const ImVec2 pos = ImGui::GetCursorScreenPos();

	ImDrawList *draw = ImGui::GetWindowDrawList();
	draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
	              ImGui::GetColorU32(ImGuiCol_Border), 4.0f);

	const char *label = "[ wale_limiter logo ]";
	const ImVec2 text_size = ImGui::CalcTextSize(label);
	draw->AddText(ImVec2(pos.x + (size.x - text_size.x) * 0.5f,
	                     pos.y + (size.y - text_size.y) * 0.5f),
	              ImGui::GetColorU32(ImGuiCol_Text), label);

	ImGui::Dummy(size); // Reserve the layout space the box occupies.
}

// One flag button per language, right-aligned on the logo's row. They are embedded images,
// not the flag emoji: ReShade's overlay font has no emoji glyphs, so an emoji would show as
// "??". The active language gets a frame, the other flag is dimmed; the choice is saved.
static void draw_language_selector(reshade::api::effect_runtime *runtime)
{
	const ImGuiStyle &style = ImGui::GetStyle();
	const ImVec2 flag_size(27.0f, 18.0f);
	const float button_w = flag_size.x + style.FramePadding.x * 2.0f;
	const float total_w = button_w * lang_count + style.ItemSpacing.x * (lang_count - 1);

	ImGui::SameLine();
	const float avail = ImGui::GetContentRegionAvail().x;
	if (avail > total_w)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - total_w);

	for (int i = 0; i < lang_count; ++i)
	{
		if (i > 0)
			ImGui::SameLine();

		const bool active = (i == g_language);
		bool clicked = false;
		if (g_flags[i].view.handle != 0)
			clicked = ImGui::ImageButton(kText[i].code, static_cast<ImTextureID>(g_flags[i].view.handle), flag_size,
			                             ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
			                             active ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 0.45f));
		else
			clicked = ImGui::Button(kText[i].fallback);

		if (active)
			ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
			                                    ImGui::GetColorU32(ImGuiCol_CheckMark), 3.0f, 0, 2.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", kText[i].name);

		if (clicked && !active)
		{
			g_language = i;
			reshade::set_config_value(runtime, kConfigSection, kLanguageKey, kText[i].code);
			wale_log(reshade::log_level::debug, "UI language set to %s.", kText[i].code);
		}
	}
}

static void draw_overlay(reshade::api::effect_runtime *runtime)
{
	// First run only: a deliberate size and spot instead of wherever ImGui lands.
	// ReShade persists this window's position, size, collapsed state and dock slot in
	// ReShade.ini ([OVERLAY] Window=/Docking=, keyed by the window title), and
	// ImGuiCond_FirstUseEver is ignored once that saved entry exists - so whatever the
	// user drags it to, including into ReShade's docked panel, always wins afterwards.
	// x at 45% of the screen keeps it clear of ReShade's own left-hand dock column.
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	// 0 on both axes = auto-fit to the contents. A fixed width would clip the URL
	// labels, and by how much depends on the user's ReShade font size.
	ImGui::SetWindowSize(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::SetWindowPos(ImVec2(display.x * 0.45f, display.y * 0.20f), ImGuiCond_FirstUseEver);

	draw_logo();
	draw_language_selector(runtime);
	ImGui::Spacing();

	// Looked up after the selector, so a language click takes effect in this same frame.
	const ui_text &text = kText[g_language];

	bool enabled = g_limit_enabled.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(text.limit, &enabled))
	{
		g_limit_enabled.store(enabled, std::memory_order_relaxed);
		g_last_present = clock_type::now(); // Reset pacing baseline on toggle.
		reshade::set_config_value(runtime, kConfigSection, kConfigKey, enabled);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextUnformatted(text.built_for);
	ImGui::TextDisabled(text.version, WALE_VERSION_STR);

	// Discord link: button opens the invite; full URL shown below as a selectable fallback.
	if (ImGui::Button(text.discord))
		ShellExecuteA(nullptr, "open", kDiscordUrl, nullptr, nullptr, SW_SHOWNORMAL);
	ImGui::TextUnformatted(kDiscordUrl); // Selectable/copyable if the click is blocked.

	ImGui::Spacing();

	// Source code: open the repository so users can read what they installed.
	if (ImGui::Button(text.source))
		ShellExecuteA(nullptr, "open", kSourceUrl, nullptr, nullptr, SW_SHOWNORMAL);
	ImGui::TextUnformatted(kSourceUrl); // Selectable/copyable fallback.
}

// ---------------------------------------------------------------------------
// Addon metadata (read by ReShade)
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) const char *NAME = "wale_limiter";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Hard-caps the game's frame rate to exactly 60 FPS. Built for Underground Racing.";

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		timeBeginPeriod(1); // Tighten sleep granularity for the sleep_for fallback path.
		// High-resolution waitable timer (Win10 1803+). Null on older OS -> sleep fallback.
		g_timer = CreateWaitableTimerExW(nullptr, nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (g_timer == nullptr)
			wale_log(reshade::log_level::warning,
			         "High-resolution timer unavailable (error %lu); using the sleep fallback.",
			         GetLastError());
		wale_log(reshade::log_level::info, "wale_limiter v%s loaded - cap 60 FPS, wait path: %s.",
		         WALE_VERSION_STR, g_timer != nullptr ? "high-resolution timer" : "sleep fallback");
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::present>(on_present);
		// The title is also the key ReShade stores this window's layout under in
		// ReShade.ini - renaming it throws away every user's saved position/dock slot.
		reshade::register_overlay("wale_limiter", draw_overlay);
		break;
	case DLL_PROCESS_DETACH:
		wale_log(reshade::log_level::debug, "Unloading.");
		reshade::unregister_addon(hModule);
		if (g_timer != nullptr)
			CloseHandle(g_timer);
		timeEndPeriod(1);
		break;
	}
	return TRUE;
}
