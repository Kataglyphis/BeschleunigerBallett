if(NOT COMMAND kataglyphis_set_imgui_filter)
  function(kataglyphis_set_imgui_filter external_lib_root)
    set(_kataglyphis_imgui_root "${external_lib_root}/IMGUI/")
    set(_kataglyphis_imgui_filter
        "${_kataglyphis_imgui_root}imconfig.h"
        "${_kataglyphis_imgui_root}imgui.cpp"
        "${_kataglyphis_imgui_root}imgui.h"
        "${_kataglyphis_imgui_root}imgui_demo.cpp"
        "${_kataglyphis_imgui_root}imgui_draw.cpp"
        "${_kataglyphis_imgui_root}imgui_internal.h"
        "${_kataglyphis_imgui_root}imgui_tables.cpp"
        "${_kataglyphis_imgui_root}imgui_widgets.cpp"
        "${_kataglyphis_imgui_root}imstb_rectpack.h"
        "${_kataglyphis_imgui_root}imstb_textedit.h"
        "${_kataglyphis_imgui_root}imstb_truetype.h"
        "${_kataglyphis_imgui_root}backends/imgui_impl_glfw.h"
        "${_kataglyphis_imgui_root}backends/imgui_impl_glfw.cpp"
        ${ARGN})

    set(IMGUI_FILTER
        "${_kataglyphis_imgui_filter}"
        PARENT_SCOPE)
  endfunction()
endif()
