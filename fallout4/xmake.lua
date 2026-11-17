-- Snapshot for Fallout 4 (F4SE plugin), built with CommonLibF4 (libxse) and xmake
set_xmakever("3.0.0")
set_project("fallout4-snapshot")
set_version("1.0.0")
set_languages("c++23")
set_encodings("utf-8")

add_rules("mode.releasedbg")
set_defaultmode("releasedbg")

includes("lib/commonlibf4")

target("Snapshot", function()
    add_rules("commonlibf4.plugin", {
        name = "Snapshot",
        author = "tonycubed2",
        description = "Troubleshooting snapshot for bug reports"
    })
    -- the built DLL stays in this project's build folder: nothing is copied into the game
    set_values("commonlib.plugin.install", false)
    set_installdir("$(projectdir)/build/install")
    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    add_includedirs("src")
end)
