add_requires("pixi::libcurl", {alias = "libcurl"})
add_requires("pixi::libiconv", {alias = "libiconv"})
add_requires("pixi::libuv", {alias = "libuv"})
add_requires("glaze")
add_requires("ngcpp-proxy")

if is_plat("mingw") then
    -- glaze installs through CMake, whose compiler check links a test program
    -- and so also needs the Pixi MinGW CRT search path (see the root xmake.lua).
    add_requireconfs("glaze", {configs = {
        ldflags = "-B" .. path.join(os.getenv("CONDA_PREFIX"), "Library", "x86_64-w64-mingw32", "sysroot", "usr", "lib") .. "/"
    }})
end

target("lighter")
    set_kind("static")
    add_files("async/**/*.cpp", "encoding/*.cpp", "http/*.cpp", "lexer/*.cpp", "lexer/**/*.cpp", "utils/*.cpp")
    -- `xmake install` (and the xmake package) lays headers out as include/lighter/...
    add_headerfiles("(**.h)|tests/**.h", "(**.hpp)", {prefixdir = "lighter"})
    add_packages("libuv")
    add_packages("libiconv")
    add_syslinks("stdc++exp", {public = true})
    add_packages("libcurl", {public = true})
    add_packages("ngcpp-proxy", {public = true})
    -- codec/json is header-only over glaze, so consumers need its headers too
    add_packages("glaze", {public = true})
    if is_os("windows") then
        add_syslinks("psapi", "user32", "advapi32", "iphlpapi", "userenv", "ws2_32", "dbghelp", "ole32", "shell32", {public = true})
    elseif is_os("linux") then
        add_syslinks("pthread", {public = true})
    end

includes("tests/xmake.lua")
