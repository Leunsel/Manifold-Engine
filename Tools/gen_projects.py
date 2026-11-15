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


def configurations():
    return """  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
"""


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
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>{kind}</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
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
    <OutDir>$(ProjectDir)..\\Build\\$(Configuration)\\</OutDir>
    <IntDir>$(ProjectDir)..\\Build\\obj\\$(Configuration)\\{target}\\</IntDir>
    <TargetName>{name}</TargetName>
  </PropertyGroup>
"""


INCLUDES = "$(ProjectDir)..\\Source;$(ProjectDir)..\\Dependencies\\ImGui;$(ProjectDir)..\\Dependencies\\ImGui\\backends;$(ProjectDir)..\\Dependencies\\Zydis;$(ProjectDir)..\\Dependencies\\Lua;$(ProjectDir)..\\Dependencies\\stb;%(AdditionalIncludeDirectories)"
DEFINES = "_WINDOWS;WIN32_LEAN_AND_MEAN;NOMINMAX;ZYDIS_STATIC_BUILD;ZYCORE_STATIC_BUILD;IMGUI_IMPL_WIN32_DISABLE_GAMEPAD;%(PreprocessorDefinitions)"
LIBS = "d3d11.lib;dxgi.lib;dwmapi.lib;comdlg32.lib;shell32.lib;ole32.lib;advapi32.lib;user32.lib;gdi32.lib;dbghelp.lib;bcrypt.lib;version.lib;wininet.lib;%(AdditionalDependencies)"


def config_groups(strict, subsystem, link):
    warning = "<WarningLevel>Level4</WarningLevel>\n      <TreatWarningAsError>true</TreatWarningAsError>\n      <SDLCheck>true</SDLCheck>" if strict else "<WarningLevel>TurnOffAllWarnings</WarningLevel>\n      <TreatWarningAsError>false</TreatWarningAsError>"
    link_block = ""
    if link:
        link_block = f"""    <Link>
      <SubSystem>{subsystem}</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <AdditionalOptions>/PDBALTPATH:%_PDB% %(AdditionalOptions)</AdditionalOptions>
      <AdditionalDependencies>{LIBS}</AdditionalDependencies>
    </Link>
"""
    return f"""  <ItemDefinitionGroup>
    <ClCompile>
      {warning}
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <AdditionalOptions>/utf-8 /Zc:__cplusplus %(AdditionalOptions)</AdditionalOptions>
      <AdditionalIncludeDirectories>{INCLUDES}</AdditionalIncludeDirectories>
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
"""


def items(tag, paths):
    if not paths:
        return ""
    lines = "".join(f'    <{tag} Include="{rel(p)}" />\n' for p in paths)
    return f"  <ItemGroup>\n{lines}  </ItemGroup>\n"


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
      <Command>where python &gt;nul 2&gt;nul || exit /b 0
python "$(ProjectDir)..\\Tools\\embed_lua.py"</Command>
      <Message>Embedding the Lua compatibility scripts</Message>
    </PreBuildEvent>
"""


def with_prebuild(project):
    return project.replace("  </ItemDefinitionGroup>\n", PREBUILD + "  </ItemDefinitionGroup>\n", 1)


def main():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import embed_lua
    embed_lua.main()
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
    app = app.replace("  </ItemDefinitionGroup>\n  <ItemDefinitionGroup Condition=\"'$(Configuration)'=='Debug'\">",
                      "    <Manifest>\n      <EnableDpiAwareness>false</EnableDpiAwareness>\n      <AdditionalManifestFiles>$(ProjectDir)..\\Source\\Resources\\App.manifest</AdditionalManifestFiles>\n    </Manifest>\n  </ItemDefinitionGroup>\n  <ItemDefinitionGroup Condition=\"'$(Configuration)'=='Debug'\">", 1)
    app += items("ClCompile", app_sources)
    app += items("ClInclude", app_headers)
    app += items("ResourceCompile", ["Source/Resources/App.rc"])
    app += items("Manifest", ["Source/Resources/App.manifest"])
    app += items("Image", ["Source/Resources/icon.ico"])
    app += reference("Dependencies")
    app += build_only_reference("ManifoldAgent")
    app += tail()
    app = with_prebuild(app)
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
    test_sources += [p for p in collect(os.path.join(ROOT, "Tests"), {".cpp"}) if "Target" not in p]
    test_headers = collect(os.path.join(ROOT, "Tests"), {".h"})
    tests = common_head("Manifold-Engine.Tests", "Application", "Tests")
    tests += config_groups(True, "Console", True)
    tests = tests.replace("%(AdditionalIncludeDirectories)", "$(ProjectDir)..\\Tests;%(AdditionalIncludeDirectories)")
    tests = tests.replace("_WINDOWS;", "_CONSOLE;MANIFOLD_TESTS;")
    tests += items("ClCompile", test_sources)
    tests += items("ClInclude", test_headers)
    tests += reference("Dependencies")
    tests += build_only_reference("ManifoldAgent")
    tests += tail()
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
    target += items("ClCompile", target_sources)
    target += tail()
    write("TestTarget.vcxproj", target)

    agent_sources = ["Agent/Agent.cpp"]
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
"""
    agent += items("ClCompile", agent_sources)
    agent += items("ClInclude", ["Source/Debug/VehProtocol.h"])
    agent += tail()
    write("ManifoldAgent.vcxproj", agent)

    sln = "\r\nMicrosoft Visual Studio Solution File, Format Version 12.00\r\n# Visual Studio Version 17\r\nVisualStudioVersion = 17.0.31903.59\r\nMinimumVisualStudioVersion = 10.0.40219.1\r\n"
    names = ("Manifold-Engine", "Dependencies", "Manifold-Engine.Tests", "TestTarget", "ManifoldAgent")
    for name in names:
        sln += f'Project("{{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}}") = "{name}", "Projects\\{name}.vcxproj", "{GUIDS[name]}"\r\nEndProject\r\n'
    sln += "Global\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n\t\tDebug|x64 = Debug|x64\r\n\t\tRelease|x64 = Release|x64\r\n\tEndGlobalSection\r\n"
    sln += "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n"
    for name in names:
        for config in ("Debug", "Release"):
            sln += f"\t\t{GUIDS[name]}.{config}|x64.ActiveCfg = {config}|x64\r\n\t\t{GUIDS[name]}.{config}|x64.Build.0 = {config}|x64\r\n"
    sln += "\tEndGlobalSection\r\n\tGlobalSection(SolutionProperties) = preSolution\r\n\t\tHideSolutionNode = FALSE\r\n\tEndGlobalSection\r\nEndGlobal\r\n"
    with open(os.path.join(ROOT, "Manifold-Engine.sln"), "w", encoding="utf-8-sig", newline="") as file:
        file.write(sln)
    print("wrote Manifold-Engine.sln")


if __name__ == "__main__":
    main()
