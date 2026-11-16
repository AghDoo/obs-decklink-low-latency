find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(ui_target ${CMAKE_PROJECT_NAME}-ui)
add_library(${ui_target} MODULE output-ui.cpp)
target_link_libraries(${ui_target} PRIVATE OBS::libobs OBS::frontend-api Qt6::Widgets)
target_compile_definitions(${ui_target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_features(${ui_target} PRIVATE cxx_std_17)
set_target_properties_plugin(${ui_target})
