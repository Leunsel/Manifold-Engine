#!/usr/bin/env python3
import os
import sys
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECTS = os.path.join(ROOT, "Projects")

GUIDS = {
    "Manifold-Engine": "{6C1E2A4B-7D93-4F0E-9A51-2B8C3D4E5F60}",
    "Dependencies": "{9A2B3C4D-5E6F-4071-8293-A4B5C6D7E8F9}",
    "Manifold-Engine.Tests": "{3F4E5D6C-7B8A-4990-A1B2-C3D4E5F60718}",
    "TestTarget": "{7E8F9A0B-1C2D-4E3F-8A4B-5C6D7E8F9012}",
    "ManifoldAgent": "{2D3C4B5A-6978-4A1B-9C8D-0E1F2A3B4C5D}",
    "SyscallTest": "{4A5B6C7D-8E9F-40A1-B2C3-D4E5F6071829}",
    "ManagedDumper": "{5B6C7D8E-9FA0-41B2-83C4-E5F607182930}",
}

TEST_EXCLUDED_DIRS = ("UI",)
TEST_EXCLUDED_FILES = ("Main.cpp", "Application.cpp", "WindowHost.cpp", "CrashHandler.cpp", "Fonts.cpp")


def collect(folder, extensions):
    found = []
    for base, dirs, files in os.walk(folder):
        dirs.sort()
        for name in sorted(files):
            if os.path.splitext(name)[1].lower() in extensions:
                found.append(os.path.relpath(os.path.join(base, name), ROOT))
    return found


def rel(path):
    return "..\\" + path.replace("/", "\\")


PLATFORMS = ("x64", "Win32")


def configurations():
    text = '  <ItemGroup Label="ProjectConfigurations">\n'
    for config in ("Debug", "Release"):
        for platform in PLATFORMS:
            text += f"""    <ProjectConfiguration Include="{config}|{platform}">
      <Configuration>{config}</Configuration>
      <Platform>{platform}</Platform>
    </ProjectConfiguration>
"""
    text += "  </ItemGroup>\n"
    return text


def common_head(name, kind, target):
    return f"""<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
{configurations()}  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <Keyword>Win32Proj</Keyword>
    <ProjectGuid>{GUIDS[name]}</ProjectGuid>
    <RootNamespace>{name.replace('-', '').replace('.', '')}</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
    <ProjectName>{name}</ProjectName>
    <PreferredToolArchitecture>x64</PreferredToolArchitecture>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>{kind}</ConfigurationType>
    <PlatformToolset>{TOOLSET}</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)'=='Debug'" Label="Configuration">
    <UseDebugLibraries>true</UseDebugLibraries>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)'=='Release'" Label="Configuration">
    <UseDebugLibraries>false</UseDebugLibraries>
    <WholeProgramOptimization>true</WholeProgramOptimization>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <ImportGroup Label="PropertySheets">
    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
  <PropertyGroup>
    <OutDir>$(ProjectDir)..\\Build\\$(Platform)\\$(Configuration)\\</OutDir>
    <IntDir>$(ProjectDir)..\\Build\\obj\\$(Platform)\\$(Configuration)\\{target}\\</IntDir>
    <TargetName>{name}</TargetName>
  </PropertyGroup>
"""


TOOLSET = "ClangCL"
DEPENDENCY_INCLUDES = "$(ProjectDir)..\\Dependencies\\ImGui;$(ProjectDir)..\\Dependencies\\ImGui\\backends;$(ProjectDir)..\\Dependencies\\Zydis;$(ProjectDir)..\\Dependencies\\Lua;$(ProjectDir)..\\Dependencies\\stb"
INCLUDES = "$(ProjectDir)..\\Source;$(IntDir)Info;%(AdditionalIncludeDirectories)"
CLANG_OPTIONS = """  <ItemDefinitionGroup Condition="'$(PlatformToolset)'=='ClangCL'">
    <ClCompile>
      <AdditionalOptions>-Wno-missing-field-initializers %(AdditionalOptions)</AdditionalOptions>
    </ClCompile>
  </ItemDefinitionGroup>
"""
DEFINES = "_WINDOWS;WIN32_LEAN_AND_MEAN;NOMINMAX;ZYDIS_STATIC_BUILD;ZYCORE_STATIC_BUILD;IMGUI_IMPL_WIN32_DISABLE_GAMEPAD;%(PreprocessorDefinitions)"
LIBS = "d3d11.lib;dxgi.lib;dwmapi.lib;comdlg32.lib;shell32.lib;ole32.lib;advapi32.lib;user32.lib;gdi32.lib;dbghelp.lib;bcrypt.lib;version.lib;wininet.lib;%(AdditionalDependencies)"


def config_groups(strict, subsystem, link):
    warning = "<WarningLevel>Level4</WarningLevel>\n      <TreatWarningAsError>true</TreatWarningAsError>\n      <SDLCheck>true</SDLCheck>\n      <ExternalWarningLevel>TurnOffAllWarnings</ExternalWarningLevel>" if strict else "<WarningLevel>TurnOffAllWarnings</WarningLevel>\n      <TreatWarningAsError>false</TreatWarningAsError>"
    external = f"""  <PropertyGroup>
    <ExternalIncludePath>{DEPENDENCY_INCLUDES};$(ExternalIncludePath)</ExternalIncludePath>
  </PropertyGroup>
""" if strict else ""
    includes = INCLUDES if strict else DEPENDENCY_INCLUDES + ";" + INCLUDES
    link_block = ""
    if link:
        link_block = f"""    <Link>
      <SubSystem>{subsystem}</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <AdditionalOptions>/PDBALTPATH:%_PDB% %(AdditionalOptions)</AdditionalOptions>
      <AdditionalDependencies>{LIBS}</AdditionalDependencies>
    </Link>
"""
    return external + f"""  <ItemDefinitionGroup>
    <ClCompile>
      {warning}
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <AdditionalOptions>/utf-8 /Zc:__cplusplus %(AdditionalOptions)</AdditionalOptions>
      <AdditionalIncludeDirectories>{includes}</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>{DEFINES}</PreprocessorDefinitions>
      <DebugInformationFormat>ProgramDatabase</DebugInformationFormat>
    </ClCompile>
{link_block}  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)'=='Debug'">
    <ClCompile>
      <RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary>
      <PreprocessorDefinitions>_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)'=='Release'">
    <ClCompile>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <FunctionLevelLinking>true</FunctionLevelLinking>
      <IntrinsicFunctions>true</IntrinsicFunctions>
      <Optimization>MaxSpeed</Optimization>
      <PreprocessorDefinitions>NDEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
""" + ("""    <Link>
      <EnableCOMDATFolding>true</EnableCOMDATFolding>
      <OptimizeReferences>true</OptimizeReferences>
    </Link>
""" if link else "") + """  </ItemDefinitionGroup>
""" + (CLANG_OPTIONS if strict else "")


def items(tag, paths):
    if not paths:
        return ""
    lines = "".join(f'    <{tag} Include="{rel(p)}" />\n' for p in paths)
    return f"  <ItemGroup>\n{lines}  </ItemGroup>\n"


def disable_jmc_for(project, path):
    # The direct-syscall stubs are naked functions; Just My Code would inject a
    # call ahead of the first instruction and corrupt the argument registers and
    # the return address, so it must be off for this translation unit.
    needle = f'    <ClCompile Include="{rel(path)}" />\n'
    replacement = (f'    <ClCompile Include="{rel(path)}">\n'
                   f'      <SupportJustMyCode>false</SupportJustMyCode>\n'
                   f'    </ClCompile>\n')
    if needle not in project:
        raise SystemExit(f"disable_jmc_for: {path} not found in project")
    return project.replace(needle, replacement, 1)


def reference(name):
    return f"""  <ItemGroup>
    <ProjectReference Include="{name}.vcxproj">
      <Project>{GUIDS[name]}</Project>
    </ProjectReference>
  </ItemGroup>
"""


def build_only_reference(name):
    return f"""  <ItemGroup>
    <ProjectReference Include="{name}.vcxproj">
      <Project>{GUIDS[name]}</Project>
      <ReferenceOutputAssembly>false</ReferenceOutputAssembly>
      <LinkLibraryDependencies>false</LinkLibraryDependencies>
    </ProjectReference>
  </ItemGroup>
"""


def tail():
    return """  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
</Project>
"""


def filters(sources, headers, extra):
    folders = set()
    entries = []
    for tag, paths in (("ClCompile", sources), ("ClInclude", headers)) + tuple(extra):
        for path in paths:
            folder = os.path.dirname(path).replace("/", "\\")
            parts = folder.split("\\")
            for i in range(1, len(parts) + 1):
                folders.add("\\".join(parts[:i]))
            entries.append((tag, path, folder))
    text = '<?xml version="1.0" encoding="utf-8"?>\n<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n  <ItemGroup>\n'
    for folder in sorted(folders):
        guid = "{" + str(uuid.uuid5(uuid.NAMESPACE_URL, folder)).upper() + "}"
        text += f'    <Filter Include="{folder}">\n      <UniqueIdentifier>{guid}</UniqueIdentifier>\n    </Filter>\n'
    text += "  </ItemGroup>\n  <ItemGroup>\n"
    for tag, path, folder in entries:
        text += f'    <{tag} Include="{rel(path)}">\n      <Filter>{folder}</Filter>\n    </{tag}>\n'
    text += "  </ItemGroup>\n</Project>\n"
    return text


def write(name, text):
    path = os.path.join(PROJECTS, name)
    with open(path, "w", encoding="utf-8", newline="\r\n") as file:
        file.write(text)
    print("wrote", os.path.relpath(path, ROOT))


PREBUILD = """    <PreBuildEvent>
      <Command>where python &gt;nul 2&gt;nul || (echo Manifold : warning ME0001 : Python was not found, so the generated sources in Source\\Generated were not refreshed &amp; exit /b 0)
python "$(ProjectDir)..\\Tools\\embed_lua.py" || exit /b 1
python "$(ProjectDir)..\\Tools\\embed_templates.py" || exit /b 1
python "$(ProjectDir)..\\Tools\\embed_modules.py" || exit /b 1
python "$(ProjectDir)..\\Tools\\gen_buildinfo.py" "$(IntDir)Info\\ManifoldBuild.h"</Command>
      <Message>Embedding the Lua compatibility scripts, the built-in templates and the hashes of the shipped autorun modules, writing the build info</Message>
    </PreBuildEvent>
"""


POSTBUILD = """    <PostBuildEvent>
      <Command>if not exist "$(ProjectDir)..\\Autorun\\*" exit /b 0
xcopy "$(ProjectDir)..\\Autorun" "$(OutDir)autorun" /D /E /I /Y /Q &gt;nul
if errorlevel 2 exit /b 1
exit /b 0</Command>
      <Message>Copying the Autorun modules that are newer than the ones next to the program</Message>
    </PostBuildEvent>
"""


MANAGED_BUILD = """where dotnet &gt;nul 2&gt;nul || (echo ManagedDumper : warning ME0002 : The .NET SDK was not found, so this build has no managed dumper for Mono games &amp; goto embed)
set MSBuildSDKsPath=
set MSBuildExtensionsPath=
set MSBUILD_EXE_PATH=
dotnet build "$(ProjectDir)..\\Managed\\ManifoldDumper\\ManifoldDumper.csproj" -c Release -nologo -v:q --disable-build-servers || exit /b 1
dotnet build "$(ProjectDir)..\\Managed\\FakeUnity\\FakeUnity.csproj" -c Release -nologo -v:q --disable-build-servers || exit /b 1
:embed
where python &gt;nul 2&gt;nul &amp;&amp; goto generate
if exist "$(ProjectDir)..\\Source\\Generated\\ManagedDumperImage.cpp" exit /b 0
echo ManagedDumper : warning ME0003 : Python was not found, so an empty managed dumper is embedded
copy /Y "$(ProjectDir)..\\Tools\\ManagedDumperImage.empty.cpp" "$(ProjectDir)..\\Source\\Generated\\ManagedDumperImage.cpp" &gt;nul
exit /b 0
:generate
python "$(ProjectDir)..\\Tools\\embed_managed.py" --touch"""


def managed_project():
    import embed_managed
    sources = [p for p in collect(os.path.join(ROOT, "Managed"), {".cs", ".csproj"})
               if "\\obj\\" not in p.replace("/", "\\") and "\\bin\\" not in p.replace("/", "\\")]
    inputs = ";".join("$(ProjectDir)" + rel(p) for p in sources + ["Tools/embed_managed.py"])
    project = common_head("ManagedDumper", "Utility", "ManagedDumper")
    project += f"""  <ItemDefinitionGroup>
    <CustomBuildStep>
      <Command>{MANAGED_BUILD}</Command>
      <Message>Building the managed dumper and embedding it into the program</Message>
      <Inputs>{inputs}</Inputs>
      <Outputs>$(ProjectDir)..\\Build\\Managed\\{embed_managed.assembly_name()}.dll;$(ProjectDir)..\\Build\\Managed\\UnityEngine.CoreModule.dll;$(ProjectDir)..\\Source\\Generated\\ManagedDumperImage.cpp</Outputs>
    </CustomBuildStep>
  </ItemDefinitionGroup>
"""
    project += items("None", sources)
    project += tail()
    return project


def with_x86_twin(project, name):
    # A 32-bit game needs the 32-bit agent, and the 32-bit tests need a 32-bit
    # target. Each x64 build of these projects also builds the project as Win32
    # under the name <name>32, next to the x64 program, so one Debug|x64 or
    # Release|x64 build has everything the program and the tests load.
    head_end = f"    <TargetName>{name}</TargetName>\n"
    at = project.index(head_end)
    group_end = project.index("  </PropertyGroup>\n", at) + len("  </PropertyGroup>\n")
    twin = f"""  <PropertyGroup Condition="'$(Platform)'=='Win32'">
    <OutDir>$(ProjectDir)..\\Build\\x64\\$(Configuration)\\</OutDir>
    <TargetName>{name}32</TargetName>
  </PropertyGroup>
"""
    project = project[:group_end] + twin + project[group_end:]
    target = """  <Target Name="BuildX86Twin" AfterTargets="Build" Condition="'$(Platform)'=='x64'">
    <MSBuild Projects="$(MSBuildProjectFullPath)" Properties="Configuration=$(Configuration);Platform=Win32" />
  </Target>
"""
    return project.replace("</Project>\n", target + "</Project>\n")


def with_prebuild(project):
    return project.replace("  </ItemDefinitionGroup>\n", PREBUILD + "  </ItemDefinitionGroup>\n", 1)


def with_postbuild(project):
    return project.replace(PREBUILD, PREBUILD + POSTBUILD, 1)


def main():
    global TOOLSET
    if "--msvc" in sys.argv[1:]:
        TOOLSET = "v143"
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import embed_lua
    embed_lua.main()
    import embed_templates
    embed_templates.main()
    import embed_modules
    embed_modules.main()
    import embed_managed
    embed_managed.main()
    import gen_icon_names
    gen_icon_names.main()
    import gen_lua_docs
    gen_lua_docs.main()
    os.makedirs(PROJECTS, exist_ok=True)
    source = os.path.join(ROOT, "Source")
    app_sources = collect(source, {".cpp"})
    app_headers = collect(source, {".h"})

    deps_sources = [p for p in collect(os.path.join(ROOT, "Dependencies", "ImGui"), {".cpp"})
                    if "backends" not in p or os.path.basename(p) in ("imgui_impl_dx11.cpp", "imgui_impl_win32.cpp")]
    deps_sources += collect(os.path.join(ROOT, "Dependencies", "Zydis"), {".c"})
    deps_sources += [p for p in collect(os.path.join(ROOT, "Dependencies", "Lua"), {".c"}) if os.path.basename(p) not in ("lua.c", "luac.c")]
    deps_headers = [p for p in collect(os.path.join(ROOT, "Dependencies"), {".h", ".hpp"}) if "backends" not in p or "dx11" in p or "win32" in p]

    app = common_head("Manifold-Engine", "Application", "App")
    app += config_groups(True, "Windows", True)
    app = app.replace("      <GenerateDebugInformation>true</GenerateDebugInformation>\n",
                      "      <GenerateDebugInformation>true</GenerateDebugInformation>\n      <GenerateManifest>false</GenerateManifest>\n", 1)
    app = app.replace("    <TargetName>Manifold-Engine</TargetName>\n",
                      "    <TargetName>Manifold-Engine</TargetName>\n    <GenerateManifest>false</GenerateManifest>\n    <EmbedManifest>false</EmbedManifest>\n", 1)
    app += items("ClCompile", app_sources)
    app += items("ClInclude", app_headers)
    app += items("ResourceCompile", ["Source/Resources/App.rc"])
    app += items("None", ["Source/Resources/App.manifest"])
    app += items("Image", ["Source/Resources/icon.ico"])
    app += reference("Dependencies")
    app += build_only_reference("ManifoldAgent")
    app += build_only_reference("ManagedDumper")
    app += tail()
    app = disable_jmc_for(app, "Source/Syscalls/Syscall.cpp")
    app = with_prebuild(app)
    app = with_postbuild(app)
    write("Manifold-Engine.vcxproj", app)
    write("Manifold-Engine.vcxproj.filters", filters(app_sources, app_headers, (("ResourceCompile", ["Source/Resources/App.rc"]), ("Image", ["Source/Resources/icon.ico"]))))

    deps = common_head("Dependencies", "StaticLibrary", "Dependencies")
    deps += config_groups(False, "Windows", False)
    deps += items("ClCompile", deps_sources)
    deps += items("ClInclude", deps_headers)
    deps += tail()
    write("Dependencies.vcxproj", deps)
    write("Dependencies.vcxproj.filters", filters(deps_sources, deps_headers, ()))

    test_sources = [p for p in app_sources
                    if not any(("Source\\" + d + "\\") in p.replace("/", "\\") for d in TEST_EXCLUDED_DIRS)
                    and os.path.basename(p) not in TEST_EXCLUDED_FILES]
    test_sources += [p for p in collect(os.path.join(ROOT, "Tests"), {".cpp"}) if "Target" not in p and "Syscalls" not in p]
    test_headers = collect(os.path.join(ROOT, "Tests"), {".h"})
    tests = common_head("Manifold-Engine.Tests", "Application", "Tests")
    tests += config_groups(True, "Console", True)
    tests = tests.replace("%(AdditionalIncludeDirectories)", "$(ProjectDir)..\\Tests;%(AdditionalIncludeDirectories)")
    tests = tests.replace("_WINDOWS;", "_CONSOLE;MANIFOLD_TESTS;")
    tests += items("ClCompile", test_sources)
    tests += items("ClInclude", test_headers)
    tests += reference("Dependencies")
    tests += build_only_reference("ManifoldAgent")
    tests += build_only_reference("ManagedDumper")
    tests += tail()
    tests = disable_jmc_for(tests, "Source/Syscalls/Syscall.cpp")
    tests = with_prebuild(tests)
    write("Manifold-Engine.Tests.vcxproj", tests)
    write("Manifold-Engine.Tests.vcxproj.filters", filters(test_sources, test_headers, ()))

    target_sources = ["Tests/Target/TestTarget.cpp"]
    target = common_head("TestTarget", "Application", "TestTarget")
    target = target.replace("    <TargetName>TestTarget</TargetName>\n", "    <TargetName>TestTarget</TargetName>\n    <LinkIncremental>false</LinkIncremental>\n")
    target += config_groups(True, "Console", True)
    target = target.replace("_WINDOWS;", "_CONSOLE;")
    target = target.replace("<UseDebugLibraries>true</UseDebugLibraries>", "<UseDebugLibraries>false</UseDebugLibraries>")
    target = target.replace("<RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary>", "<RuntimeLibrary>MultiThreaded</RuntimeLibrary>\n      <Optimization>MaxSpeed</Optimization>")
    target = target.replace("<PreprocessorDefinitions>_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>", "<PreprocessorDefinitions>NDEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>")
    target += items("ClCompile", target_sources)
    target += tail()
    target = with_x86_twin(target, "TestTarget")
    write("TestTarget.vcxproj", target)

    # Stand-alone console test for the direct-syscall stub. It compiles only the
    # syscall unit plus a driver, so it builds for Win32 too (unlike the app,
    # which is x64-only).
    syscall_sources = ["Source/Syscalls/Syscall.cpp", "Tests/Syscalls/SyscallSelfTest.cpp"]
    syscall_test = common_head("SyscallTest", "Application", "SyscallTest")
    syscall_test += config_groups(False, "Console", True)
    syscall_test = syscall_test.replace("_WINDOWS;", "_CONSOLE;")
    # The naked syscall stubs must not be instrumented: Just My Code injects a
    # call ahead of the first instruction, which clobbers the argument registers
    # and the return address in a frameless function.
    syscall_test = syscall_test.replace("<ConformanceMode>true</ConformanceMode>",
                                        "<ConformanceMode>true</ConformanceMode>\n      <SupportJustMyCode>false</SupportJustMyCode>", 1)
    syscall_test += items("ClCompile", syscall_sources)
    syscall_test += tail()
    write("SyscallTest.vcxproj", syscall_test)

    agent_sources = ["Agent/Agent.cpp", "Agent/Speed.cpp"]
    agent = common_head("ManifoldAgent", "DynamicLibrary", "Agent")
    agent += """  <ItemDefinitionGroup>
    <ClCompile>
      <WarningLevel>Level4</WarningLevel>
      <TreatWarningAsError>true</TreatWarningAsError>
      <SDLCheck>true</SDLCheck>
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <AdditionalOptions>/utf-8 /Zc:__cplusplus %(AdditionalOptions)</AdditionalOptions>
      <AdditionalIncludeDirectories>$(ProjectDir)..\\Source;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>WIN32_LEAN_AND_MEAN;NOMINMAX;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <DebugInformationFormat>ProgramDatabase</DebugInformationFormat>
    </ClCompile>
    <Link>
      <SubSystem>Windows</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <AdditionalDependencies>kernel32.lib;user32.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)'=='Debug'">
    <ClCompile>
      <RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary>
      <PreprocessorDefinitions>_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)'=='Release'">
    <ClCompile>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <Optimization>MaxSpeed</Optimization>
      <PreprocessorDefinitions>NDEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
""" + CLANG_OPTIONS
    agent += items("ClCompile", agent_sources)
    agent += items("ClInclude", ["Source/Debug/VehProtocol.h", "Source/Debug/BreakProgram.h", "Source/Target/SpeedhackProtocol.h"])
    agent += tail()
    agent = with_x86_twin(agent, "ManifoldAgent")
    write("ManifoldAgent.vcxproj", agent)

    write("ManagedDumper.vcxproj", managed_project())

    sln = "\r\nMicrosoft Visual Studio Solution File, Format Version 12.00\r\n# Visual Studio Version 17\r\nVisualStudioVersion = 17.0.31903.59\r\nMinimumVisualStudioVersion = 10.0.40219.1\r\n"
    names = ("Manifold-Engine", "Dependencies", "Manifold-Engine.Tests", "TestTarget", "ManifoldAgent", "SyscallTest", "ManagedDumper")
    for name in names:
        sln += f'Project("{{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}}") = "{name}", "Projects\\{name}.vcxproj", "{GUIDS[name]}"\r\nEndProject\r\n'
    sln += "Global\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n"
    for config in ("Debug", "Release"):
        for platform in PLATFORMS:
            sln += f"\t\t{config}|{platform} = {config}|{platform}\r\n"
    sln += "\tEndGlobalSection\r\n"
    sln += "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n"
    for name in names:
        for config in ("Debug", "Release"):
            for platform in PLATFORMS:
                sln += f"\t\t{GUIDS[name]}.{config}|{platform}.ActiveCfg = {config}|{platform}\r\n\t\t{GUIDS[name]}.{config}|{platform}.Build.0 = {config}|{platform}\r\n"
    sln += "\tEndGlobalSection\r\n\tGlobalSection(SolutionProperties) = preSolution\r\n\t\tHideSolutionNode = FALSE\r\n\tEndGlobalSection\r\nEndGlobal\r\n"
    with open(os.path.join(ROOT, "Manifold-Engine.sln"), "w", encoding="utf-8-sig", newline="") as file:
        file.write(sln)
    print("wrote Manifold-Engine.sln")


if __name__ == "__main__":
    main()
