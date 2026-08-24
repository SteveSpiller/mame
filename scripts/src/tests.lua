-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   tests.lua
--
--   Rules for building tests
--
---------------------------------------------------------------------------

project("mametests")
	uuid ("66d4c639-196b-4065-a411-7ee9266564f5")
	kind "ConsoleApp"

	flags {
		"Symbols", -- always include minimum symbols for executables
	}

	if _OPTIONS["SEPARATE_BIN"]~="1" then
		targetdir(MAME_DIR)
	end

	configuration { "Release" }
		targetsuffix ""
		if _OPTIONS["PROFILE"] then
			targetsuffix "p"
		end

	configuration { "Debug" }
		targetsuffix "d"
		if _OPTIONS["PROFILE"] then
			targetsuffix "dp"
		end

	configuration { "mingw*" or "vs*" }
		targetextension ".exe"

	configuration { }

	links {
		"utils",
		ext_lib("expat"),
		ext_lib("zlib"),
		"ocore_" .. _OPTIONS["osd"],
	}

	includedirs {
		MAME_DIR .. "3rdparty/catch/single_include",
		MAME_DIR .. "src/devices",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/lib/util",
		ext_includedir("expat"),
		ext_includedir("zlib"),
	}

	-- production code exercised by the tests
	files {
		MAME_DIR .. "src/devices/video/cocovga_artifact.cpp",
		MAME_DIR .. "src/devices/video/cocovga_artifact.h",
		MAME_DIR .. "src/devices/video/cocovga_capture.cpp",
		MAME_DIR .. "src/devices/video/cocovga_capture.h",
		MAME_DIR .. "src/devices/video/cocovga_charrom.h",
		MAME_DIR .. "src/devices/video/cocovga_core.cpp",
		MAME_DIR .. "src/devices/video/cocovga_core.h",
		MAME_DIR .. "src/devices/video/cocovga_extended.cpp",
		MAME_DIR .. "src/devices/video/cocovga_extended.h",
		MAME_DIR .. "src/devices/video/cocovga_renderer.cpp",
		MAME_DIR .. "src/devices/video/cocovga_renderer.h",
		MAME_DIR .. "src/devices/video/mc6847_charset.h",
	}

	files {
		MAME_DIR .. "tests/main.cpp",
		MAME_DIR .. "tests/lib/util/corestr.cpp",
		MAME_DIR .. "tests/lib/util/options.cpp",
		MAME_DIR .. "tests/emu/attotime.cpp",
		MAME_DIR .. "tests/emu/video/rgbutil.cpp",
		MAME_DIR .. "tests/emu/video/cocovga_artifact_test.cpp",
		MAME_DIR .. "tests/emu/video/cocovga_artifact_vectors.h",
		MAME_DIR .. "tests/emu/video/cocovga_capture_test.cpp",
		MAME_DIR .. "tests/emu/video/cocovga_charrom_oracle.h",
		MAME_DIR .. "tests/emu/video/cocovga_core_test.cpp",
		MAME_DIR .. "tests/emu/video/cocovga_extended_test.cpp",
		MAME_DIR .. "tests/emu/video/cocovga_integration_test.cpp",
		MAME_DIR .. "tests/emu/video/cocovga_lowercase_oracle.h",
		MAME_DIR .. "tests/emu/video/cocovga_renderer_test.cpp",
		MAME_DIR .. "tests/emu/video/cocovga_test_helpers.h",
		MAME_DIR .. "tests/emu/video/mc6847.cpp",
		MAME_DIR .. "tests/emu/video/mc6847_charset_fixture.h",
	}

