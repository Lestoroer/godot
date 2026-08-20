/**************************************************************************/
/*  display_server_offscreen.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "display_server_offscreen.h"

#include "core/os/os.h"

#if defined(RD_ENABLED) && defined(VULKAN_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/rendering_device.h"
#endif

DisplayServer *DisplayServerOffscreen::create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error) {
	DisplayServerOffscreen *display_server = memnew(DisplayServerOffscreen(p_rendering_driver, p_resolution, p_context, r_error));
	if (r_error != OK) {
		memdelete(display_server);
		return nullptr;
	}
	return display_server;
}

Vector<String> DisplayServerOffscreen::get_rendering_drivers_func() {
	Vector<String> drivers;
#if defined(RD_ENABLED) && defined(VULKAN_ENABLED)
	drivers.push_back("vulkan");
#endif
	return drivers;
}

void DisplayServerOffscreen::register_offscreen_driver() {
	// Fork(Lestoroer): offscreen is opt-in and must stay between the native
	// Windows server and the always-last headless server.
	register_create_function(DRIVER_NAME, create_func, get_rendering_drivers_func);
}

Vector<DisplayServerEnums::WindowID> DisplayServerOffscreen::get_window_list() const {
	Vector<DisplayServerEnums::WindowID> windows;
	windows.push_back(DisplayServerEnums::MAIN_WINDOW_ID);
	return windows;
}

DisplayServerEnums::WindowID DisplayServerOffscreen::create_sub_window(DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Rect2i &p_rect, bool p_exclusive, DisplayServerEnums::WindowID p_transient_parent) {
	ERR_FAIL_V_MSG(DisplayServerEnums::INVALID_WINDOW_ID, "Subwindows are not supported by the offscreen display server.");
}

void DisplayServerOffscreen::delete_sub_window(DisplayServerEnums::WindowID p_id) {
	ERR_FAIL_MSG("Subwindows are not supported by the offscreen display server.");
}

void DisplayServerOffscreen::window_set_size(const Size2i p_size, DisplayServerEnums::WindowID p_window) {
	ERR_FAIL_COND_MSG(p_window != DisplayServerEnums::MAIN_WINDOW_ID, "Only the main virtual window exists in offscreen mode.");
	window_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
}

DisplayServerOffscreen::DisplayServerOffscreen(const String &p_rendering_driver, const Vector2i &p_resolution, DisplayServerEnums::Context p_context, Error &r_error) {
	r_error = ERR_UNAVAILABLE;
	window_size = Size2i(MAX(1, p_resolution.x), MAX(1, p_resolution.y));

	ERR_FAIL_COND_MSG(p_context != DisplayServerEnums::CONTEXT_ENGINE, "The offscreen display server only supports running projects, not the editor or project manager.");
	ERR_FAIL_COND_MSG(p_rendering_driver != "vulkan", "The offscreen display server only supports the Vulkan rendering driver.");

#if defined(RD_ENABLED) && defined(VULKAN_ENABLED)
	// Fork(Lestoroer): RenderingContextDriverVulkan is intentionally initialized
	// without a platform surface, then RenderingDevice without a main window.
	rendering_context = memnew(RenderingContextDriverVulkan);
	ERR_FAIL_COND_MSG(rendering_context->initialize() != OK, "Failed to initialize the surfaceless Vulkan context for offscreen rendering.");

	rendering_device = memnew(RenderingDevice);
	ERR_FAIL_COND_MSG(rendering_device->initialize(rendering_context, DisplayServerEnums::INVALID_WINDOW_ID, true) != OK, "Failed to initialize the Vulkan rendering device for offscreen rendering.");

	RendererCompositorRD::make_current();
	r_error = OK;
#else
	ERR_FAIL_MSG("The offscreen display server requires a build with RenderingDevice and Vulkan support.");
#endif
}

DisplayServerOffscreen::~DisplayServerOffscreen() {
#if defined(RD_ENABLED) && defined(VULKAN_ENABLED)
	if (rendering_device) {
		memdelete(rendering_device);
		rendering_device = nullptr;
	}
	if (rendering_context) {
		memdelete(rendering_context);
		rendering_context = nullptr;
	}
#endif
}
