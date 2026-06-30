# ApplyUESDKPatch.cmake
#
# UESDK (praydog's Unreal SDK) is a PRIVATE submodule, so it cannot be forked
# publicly to carry our one-line embedded-RenderDoc fix. Instead we apply the
# fix to the builder's OWN UESDK checkout at configure time. Anyone building
# UEVR already has legitimate UESDK access (it is required to build at all), so
# this republishes nothing -- it is just a local working-tree patch.
#
# The fix teaches FRHITexture::get_native_resource()'s vtable probe to accept
# RenderDoc-wrapped D3D12 resources (whose vtable lives in renderdoc.dll).
# Without it the probe rejects the correct wrapped resource and scans on into
# bad vtable slots, crashing during stereo setup under embedded capture.
#
# Must run BEFORE add_subdirectory(dependencies/submodules/UESDK) so the patched
# file is on disk before it is compiled. Idempotent: skips if already applied,
# warns (never fails) on upstream drift.

set(_uesdk_dir   "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/submodules/UESDK")
set(_uesdk_patch "${CMAKE_CURRENT_SOURCE_DIR}/patches/UESDK-StereoStuff-renderdoc.patch")
set(_uesdk_file  "${_uesdk_dir}/src/sdk/StereoStuff.cpp")

if(NOT EXISTS "${_uesdk_file}")
    message(STATUS "UESDK patch: ${_uesdk_file} not found (submodule not checked out?) -- skipping")
elseif(NOT EXISTS "${_uesdk_patch}")
    message(WARNING "UESDK patch: ${_uesdk_patch} missing -- embedded RenderDoc may crash on inject")
else()
    find_package(Git QUIET)
    if(NOT GIT_EXECUTABLE)
        set(GIT_EXECUTABLE git)
    endif()

    # Already applied? A clean reverse-apply check means the fix is present.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_uesdk_dir}" apply --reverse --check "${_uesdk_patch}"
        RESULT_VARIABLE _uesdk_rev_rc OUTPUT_QUIET ERROR_QUIET)

    if(_uesdk_rev_rc EQUAL 0)
        message(STATUS "UESDK embedded-RenderDoc patch: already applied")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_uesdk_dir}" apply --check "${_uesdk_patch}"
            RESULT_VARIABLE _uesdk_fwd_rc OUTPUT_QUIET ERROR_QUIET)
        if(_uesdk_fwd_rc EQUAL 0)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${_uesdk_dir}" apply "${_uesdk_patch}"
                RESULT_VARIABLE _uesdk_ap_rc)
            if(_uesdk_ap_rc EQUAL 0)
                message(STATUS "UESDK embedded-RenderDoc patch: APPLIED to StereoStuff.cpp")
            else()
                message(WARNING "UESDK embedded-RenderDoc patch: failed to apply -- embedded RenderDoc may crash on inject")
            endif()
        else()
            message(WARNING "UESDK embedded-RenderDoc patch: does not apply cleanly to this UESDK "
                            "revision (upstream drift?). Embedded RenderDoc may crash on inject. "
                            "See patches/UESDK-StereoStuff-renderdoc.patch")
        endif()
    endif()
endif()
