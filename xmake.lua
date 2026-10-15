add_rules("mode.debug", "mode.release")
set_policy("package.requires_lock", true)

package("preloader")
    set_homepage("https://github.com/LiteLDev/preloader-android")
    set_description("Preloader Android")
    add_urls("https://github.com/LiteLDev/preloader-android.git")
    add_versions("main", "main")
    add_deps("cmake")
    on_install("android", function (package)
        import("package.tools.cmake").install(package)
    end)
package_end()

add_requires("preloader")
add_requires("nlohmann_json v3.11.3")
add_requires("glm")
add_requires("entt")
add_requires("fmt")

target("RenderScale")
    set_kind("shared")
    set_languages("c++20")
    set_strip("all")
    add_files("src/main.cpp")
    add_includedirs("src")
    add_packages("preloader", "nlohmann_json", "glm", "entt", "fmt")

    if is_plat("android") then
        add_cxflags("-fPIC", "-Oz", "-ffunction-sections", "-fdata-sections", "-flto", "-fno-stack-protector", "-fexceptions", "-w", "-fvisibility=hidden")
        add_cxxflags("-fno-rtti", "-fvisibility-inlines-hidden")
        add_shflags("-Wl,--gc-sections", "-Wl,--hash-style=gnu", "-Wl,-z,max-page-size=16384", "-flto")
        add_links("android", "log")
    end

    after_build(function (target)
        if not target:is_plat("android") then return end
        import("lib.detect.find_tool")
        local python = find_tool("python3") or find_tool("python")
        assert(python, "Python 3 is required to package RenderScale.levipack")
        os.vrunv(python.program, {
            path.join(os.projectdir(), "scripts", "package_levipack.py"),
            "--library", target:targetfile(),
            "--output", path.join(target:targetdir(), "RenderScale.levipack")
        })
    end)
