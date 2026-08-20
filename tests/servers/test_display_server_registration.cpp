/**************************************************************************/
/*  test_display_server_registration.cpp                                 */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_display_server_registration)

#include "servers/display/display_server.h"

namespace TestDisplayServerRegistration {

TEST_CASE("[DisplayServer] Headless stays last and offscreen stays opt-in") {
	// Fork(Lestoroer): Main assumes headless is last, while the native Windows
	// server must remain index zero so offscreen can never become a fallback.
	CHECK(String(DisplayServer::get_create_function_name(DisplayServer::get_create_function_count() - 1)) == "headless");

#if defined(WINDOWS_ENABLED) && defined(RD_ENABLED) && defined(VULKAN_ENABLED)
	int offscreen_index = -1;
	for (int i = 0; i < DisplayServer::get_create_function_count(); i++) {
		if (String(DisplayServer::get_create_function_name(i)) == "offscreen") {
			offscreen_index = i;
			break;
		}
	}
	CHECK(offscreen_index > 0);
#endif
}

} // namespace TestDisplayServerRegistration
