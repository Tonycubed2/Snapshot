-- Snapshot for Starfield (SFSE plugin), built with CommonLibSF (libxse) and xmake
set_xmakever("3.0.0")
set_project("starfield-snapshot")
set_version("1.0.0")
set_languages("c++23")
set_encodings("utf-8")

add_rules("mode.releasedbg")
set_defaultmode("releasedbg")

includes("lib/commonlibsf")

target("Snapshot", function()
    add_rules("commonlibsf.plugin", {
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
