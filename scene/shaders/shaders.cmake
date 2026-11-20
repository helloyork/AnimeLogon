# animelogon_scene_shaders(<target>)
#
# The scene renderer's own shaders, compiled by fxc when the program is built so that a scene's
# first frame never waits for the shader compiler (compiling them all at start took most of a
# second on a slow processor). <target> gets one generated header per entry point, each holding a
# byte array. Authors' shaders are compiled when a scene is imported, as before.
include(${PROJECT_SOURCE_DIR}/cmake/CompileShader.cmake)
function(animelogon_scene_shaders target)
    set(d ${PROJECT_SOURCE_DIR}/scene/shaders)
    animelogon_compile_shader(${target} ${d}/fullscreen.hlsl main vs_5_0 g_scene_fullscreen_vs scene_fullscreen_vs.h)
    animelogon_compile_shader(${target} ${d}/draw_vs.hlsl main vs_5_0 g_scene_draw_vs scene_draw_vs.h ${d}/draw.hlsli)
    animelogon_compile_shader(${target} ${d}/draw_ps.hlsl main ps_5_0 g_scene_draw_ps scene_draw_ps.h ${d}/draw.hlsli)
    animelogon_compile_shader(${target} ${d}/effects_vs.hlsl main vs_5_0 g_effects_vs scene_effects_vs.h)
    foreach(entry Effect Prefilter Down Blur Rays MaskCopy)
        string(TOLOWER ${entry} lower)
        animelogon_compile_shader(${target} ${d}/effects.hlsl ${entry} ps_5_0 g_effects_${lower} scene_effects_${lower}.h)
    endforeach()
    animelogon_compile_shader(${target} ${d}/particles.hlsl Vertex vs_5_0 g_particles_vs scene_particles_vs.h)
    animelogon_compile_shader(${target} ${d}/particles.hlsl Pixel ps_5_0 g_particles_ps scene_particles_ps.h)
endfunction()
