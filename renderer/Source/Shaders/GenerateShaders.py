# Copyright (c) 2026 QuakeRay contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

import sys
import os
import re
import subprocess
import pathlib


CACHE_FOLDER_PATH = "Build/"
OUTPUT_FOLDER_PATH = "../../Build/"
CACHE_FILE_NAME = "GenerateShadersCache.txt"
EXTENSIONS = [".comp", ".vert", "frag", ".rgen", ".rahit", ".rchit", ".rmiss"]
DEPENDENCY_EXTENSIONS = [".h", ".inl", ".glsl", ".hlsl", ".hlsli"]
DEPENDENCY_FOLDERS = {"", "../Generated/"}
HLSL_FOLDER_PATH = "HLSL/"
SOURCE_FOLDERS = ["", HLSL_FOLDER_PATH]
DEPENDENCY_FOLDERS_IGNORE = [CACHE_FOLDER_PATH, ".vscode/", "GLSL/", HLSL_FOLDER_PATH]
DEPENDENCY_IGNORE = ["BlueNoiseFileNames.h", "ShaderCommonC.h", "ShaderCommonCFramebuf.h"]

HLSL_SUFFIX = ".hlsl"
HLSL_PROFILES = {
    ".comp": "cs_6_2",
    ".vert": "vs_6_2",
    "frag": "ps_6_2",
    ".rgen": "lib_6_3",
    ".rahit": "lib_6_3",
    ".rchit": "lib_6_3",
    ".rmiss": "lib_6_3",
}

SPIRV_EXTENSIONS = [
    "SPV_KHR_ray_tracing",
    "SPV_EXT_descriptor_indexing",
    "SPV_KHR_compute_shader_derivatives",
]

SPIRV_RAY_QUERY_EXTENSION = "SPV_KHR_ray_query"


CACHE_FILE_DEPENDENCY_MAP_SEPARATOR_LINE = "DEPENDENCY\n"


MARKED_FILES = []


def wereDependentModified(dependencyMap, modifiedDependent, cache, baseFile, firstTime=True):
    global MARKED_FILES
    if firstTime:
        MARKED_FILES = []

    for dpd in dependencyMap[baseFile]:
        if dpd in modifiedDependent or dpd not in cache:
            return True
        elif dpd not in MARKED_FILES:
            MARKED_FILES.append(dpd)
            if wereDependentModified(dependencyMap, modifiedDependent, cache, dpd, firstTime=False):
                return True

    return False


def printInPowerShell(msg, color):
    p = subprocess.run([
        "PowerShell",
        "Write-Host",
        "\"" + msg + "\"",
        "-ForegroundColor", color
    ])


def getDependentFoldersProcArg():
    return [a for p in DEPENDENCY_FOLDERS for a in ("-I", p if p != "" else ".")]


def getHLSLStage(filename):
    if not filename.endswith(HLSL_SUFFIX):
        return None

    for ext in EXTENSIONS:
        if filename.endswith(ext + HLSL_SUFFIX):
            return ext

    return None


def isShaderSource(filename):
    return any([filename.endswith(ext) for ext in EXTENSIONS]) or getHLSLStage(filename) is not None


def getOutputFilename(filename):
    base = os.path.basename(filename)

    if base.endswith(HLSL_SUFFIX):
        base = base[:-len(HLSL_SUFFIX)]

    return OUTPUT_FOLDER_PATH + base + ".spv"


def getCompileCommand(filename, outputFilename):
    stage = getHLSLStage(filename)

    if stage is None:
        return [
            "glslc", "--target-env=vulkan1.2"
        ] + getDependentFoldersProcArg() + [
            filename,
            "-o", outputFilename]

    extensions = list(SPIRV_EXTENSIONS)
    if sourceUsesRayQuery(filename):
        extensions.append(SPIRV_RAY_QUERY_EXTENSION)

    return [
        "dxc",
        "-spirv",
        "-T", HLSL_PROFILES[stage],
        "-fspv-target-env=vulkan1.2"
    ] + ["-fspv-extension=" + ext for ext in extensions] + getDependentFoldersProcArg() + [
        filename,
        "-Fo", outputFilename]


def sourceUsesRayQuery(filename, visited=None):
    visited = visited if visited is not None else set()

    filename = abspath(filename)
    if filename in visited:
        return False
    visited.add(filename)

    try:
        with open(filename, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return False

    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)

    if re.search(r"\bRayQuery\b", text):
        return True

    for line in text.splitlines():
        if "#include" in line and '"' in line:
            includeFile = line.split('"')[1]
            for folder in DEPENDENCY_FOLDERS:
                included = abspath(folder + includeFile)
                if os.path.exists(included) and sourceUsesRayQuery(included, visited):
                    return True

    return False


def abspath(filename):
    return os.path.abspath(filename).replace('\\', '/')


def getAllSubfolders(folder):
    folder = folder if folder != "" else "."
    return {os.path.relpath(root).replace('\\', '/') + '/' for root, _, _ in os.walk(folder) if abspath(root) != abspath(folder)}


def fillDependencyFolders():
    global DEPENDENCY_FOLDERS

    fs = DEPENDENCY_FOLDERS
    for f in DEPENDENCY_FOLDERS:
        fs = fs.union(getAllSubfolders(f))

    for i in DEPENDENCY_FOLDERS_IGNORE:
        fs.discard(i)

    DEPENDENCY_FOLDERS = fs


def main():
    if "--help" in sys.argv or "-help" in sys.argv or "-h" in sys.argv or "--h" in sys.argv:
        print("-rebuild  : clear cache and rebuild all shaders")
        print("-gencomm  : invoke GenerateShaderCommon.py script")
        print("-psout    : use PowerShell for printing colored output")
        print("-r        : same as \"-rebuild\"")
        print("-g        : same as \"-gencomm\"")
        print("-ps       : same as \"-psout\"")
        print("")
        print("GLSL shaders are compiled with glslc, HLSL shaders (<name>.<stage>.hlsl)")
        print("are compiled with dxc into the very same <name>.<stage>.spv blobs.")
        return

    forceRebuild = False
    powerShellOutput = False
    if "-rebuild" in sys.argv or "--rebuild" in sys.argv or "-r" in sys.argv or "--r" in sys.argv:
        forceRebuild = True
    if "-gencomm" in sys.argv or "--gencomm" in sys.argv or "-g" in sys.argv or "--g" in sys.argv:
        subprocess.run(["python", "../Generated/GenerateShaderCommon.py", "--path", "../Generated/"])
    if "-psout" in sys.argv or "--psout" in sys.argv or "-ps" in sys.argv or "--ps" in sys.argv:
        powerShellOutput = True

    fillDependencyFolders()

    if not os.path.exists(CACHE_FOLDER_PATH):
        try:
            os.mkdir(CACHE_FOLDER_PATH)
        except OSError:
            print("> Coudn't create cache folder")
            return

    if not os.path.exists(OUTPUT_FOLDER_PATH):
        try:
            os.makedirs(OUTPUT_FOLDER_PATH)
        except OSError:
            print("> Coudn't create output folder")
            return

    if not os.path.exists(CACHE_FOLDER_PATH + CACHE_FILE_NAME):
        try:
            with open(CACHE_FOLDER_PATH + CACHE_FILE_NAME, "w"): pass
        except OSError:
            print("> Coudn't create cache file")
            return
    with open(CACHE_FOLDER_PATH + CACHE_FILE_NAME, "r+") as cacheFile:
        cache = {}
        dependencyMap = {}

        if not forceRebuild:
            try:
                parsingDpdncy = False
                for line in cacheFile:
                    if line == CACHE_FILE_DEPENDENCY_MAP_SEPARATOR_LINE:
                        parsingDpdncy = True
                    else:
                        words = line.split()
                        if not parsingDpdncy and len(words) >= 2:
                            cache[words[0]] = int(words[1])

                        if parsingDpdncy:
                            if len(words) >= 2:
                                checkedDpds = set()
                                dpds = set(words[1:])
                                for dpd in dpds:
                                    if os.path.exists(dpd):
                                        checkedDpds.add(dpd)
                                dependencyMap[words[0]] = checkedDpds
                            else:
                                dependencyMap[words[0]] = set()
            except:
                cache = {}
                dependencyMap = {}

    modifiedDependent = set()

    msgWasAnyShaderRebuilt = False
    msgErrorCount = 0

    for folder in DEPENDENCY_FOLDERS:
        if not forceRebuild:
            print("> Checking dependency files in " + ("current folder" if folder == "" else folder))

        fileList = os.listdir() if folder == "" else os.listdir(folder)
        for otherFolderFilename in fileList:
            if otherFolderFilename in DEPENDENCY_IGNORE:
                continue

            filename = abspath(folder + otherFolderFilename)
            isDependentOn = any([filename.endswith(ext) for ext in DEPENDENCY_EXTENSIONS])

            if not isDependentOn:
                continue

            if ' ' in folder + filename:
                print("> File \"" + folder + filename + "\" has spaces in its path. Skipping.")
                continue

            lastModifTime = int(pathlib.Path(filename).stat().st_mtime)

            isOutdated = filename in cache and lastModifTime != cache[filename]

            if filename not in cache or isOutdated:
                modifiedDependent.add(filename)

            cache[filename] = lastModifTime

            if filename not in dependencyMap or isOutdated:
                dependencyMap[filename] = set()

                with open(filename, "r") as dpd:
                    for line in dpd:
                        if "#include" in line and '"' in line:
                            dpdFile = line.split("\"")[1]
                            for dpdFolder in DEPENDENCY_FOLDERS:
                                dpd = abspath(dpdFolder + dpdFile)
                                if os.path.exists(dpd) and dpd != filename:
                                    dependencyMap[filename].add(dpd)

    for filenameRelative in [folder + entry for folder in SOURCE_FOLDERS for entry in os.listdir(folder or ".")]:
        filename = abspath(filenameRelative)

        if not isShaderSource(filename):
            continue

        if ' ' in filename:
            print("> File \"" + filename + "\" has spaces in its name. Skipping.")
            continue

        lastModifTime = int(pathlib.Path(filename).stat().st_mtime)
        isOutdated = filename in cache and lastModifTime != cache[filename]

        outputFilename = getOutputFilename(filename)

        if filename not in dependencyMap or isOutdated:
            dependencyMap[filename] = set()

            with open(filename, "r") as dpd:
                for line in dpd:
                    if "#include" in line and '"' in line:
                        dpdFile = line.split("\"")[1]
                        for dpdFolder in DEPENDENCY_FOLDERS:
                            dpd = abspath(dpdFolder + dpdFile)
                            if os.path.exists(dpd):
                                dependencyMap[filename].add(dpd)

        if forceRebuild or filename not in cache or isOutdated or not os.path.exists(outputFilename) or wereDependentModified(dependencyMap, modifiedDependent, cache, filename):
            print("> Building " + os.path.basename(filename))

            command = getCompileCommand(filename, outputFilename)

            try:
                r = subprocess.run(command,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                compilerOutput = r.stdout
                isFailed = r.returncode != 0
            except FileNotFoundError:
                compilerOutput = "> " + command[0] + " is not in PATH"
                isFailed = True

            if isFailed:
                if powerShellOutput:
                    printInPowerShell(compilerOutput, "Red")
                else:
                    print(compilerOutput)

                msgErrorCount += 1

                if os.path.exists(outputFilename):
                    os.remove(outputFilename)

                if filename in cache:
                    del cache[filename]
            else:
                if len(compilerOutput) > 0:
                    if powerShellOutput:
                        printInPowerShell(compilerOutput, "Yellow")
                    else:
                        print(compilerOutput)

                cache[filename] = lastModifTime

            msgWasAnyShaderRebuilt = True

    with open(CACHE_FOLDER_PATH + CACHE_FILE_NAME, "w") as cacheFile:
        for name, tm in cache.items():
            cacheFile.write(name + " " + str(tm) + "\n")
        cacheFile.write(CACHE_FILE_DEPENDENCY_MAP_SEPARATOR_LINE)
        for name, arr in dependencyMap.items():
            arrStr = " ".join(arr)
            cacheFile.write(name + " " + arrStr + "\n")

    msg = ""
    color = ""

    if msgErrorCount > 0:
        msg = "> " + str(msgErrorCount) + (" shader build failed." if msgErrorCount == 1 else " shader builds failed.")
        color = "DarkRed"
    elif not msgWasAnyShaderRebuilt:
        msg = "> Everything is up-to-date."
        color = "Green"
    else:
        msg = "> Done."
        color = "Green"

    if powerShellOutput:
        printInPowerShell(msg, color)
    else:
        print(msg)

    if msgErrorCount > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
