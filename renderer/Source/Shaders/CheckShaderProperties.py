"""
Pins the shader interface and the memory layout of the HLSL shaders. Every pair is compiled to
SPIR-V with dxc, disassembled and reflected into a property set, and that property set is
stored in a recording under Reflection/<pair path>.txt and checked in. The default mode
recompiles every pair and verifies it against its recording, so an interface or layout change
shows up as a diff against the recording.

Compared properties: entry point and execution model, workgroup size, specialization
constants, descriptor set/binding assignment, resource kind and storage class (the kind of
descriptor the host binds: a uniform buffer, a storage buffer, or a sampler/image/TLAS), the
byte layout of every block (recursively, including nested structs and array strides),
input/output locations, the builtins the entry point uses, and the capabilities and extensions
the module declares. The capability comparison exists because the host loads the module: a
capability the device is not required to enable is rejected by the validation layer with
VUID-VkShaderModuleCreateInfo-pCode-08740/08742. dxc has flags that change the capabilities it
generates, which is why the comparison cannot be left to a source review.

A header has no stage of its own and is therefore never compiled on its own, so a header is
pinned by a probe: a shader that instantiates what the header declares and touches every
member of it. The probe's HLSL lives in Probes/, out of the shader build, and is recorded and
verified like any other pair.

Usage:
    python CheckShaderProperties.py [--record [--force]] [shader ...]

With no arguments, every pair is verified against its recording: each <name>.<stage>.hlsl
next to this script and every Probes/<name>.<stage>.hlsl is compiled, disassembled and
reflected, and the result is compared with its recording under Reflection/ (HLSL/
CmPrepareFinal.comp records to Reflection/HLSL/CmPrepareFinal.comp.txt). A missing recording,
a malformed recording, a property mismatch and a recording that does not correspond to any
current pair all fail the run. Exits with a non-zero status on any failure.

--record writes the recordings of the named pairs, or of every pair when none are named. It
writes nothing if any pair fails to compile or disassemble, and it does not overwrite a
recording that changed unless --force is given: without it the diff is printed and the run
fails so the maintainer decides.
"""

import difflib
import os
import re
import subprocess
import sys


HLSL_FOLDER_PATH = "HLSL/"
PROBES_FOLDER_PATH = "Probes/"
STAGE_EXTENSIONS = [".comp", ".vert", ".frag", ".rgen", ".rahit", ".rchit", ".rmiss"]
HLSL_SUFFIX = ".hlsl"
TEMP_FOLDER_PATH = "Build/"
REFLECTION_FOLDER_PATH = "Reflection/"
REFLECTION_SUFFIX = ".txt"
REFLECTION_FORMAT = 1

HLSL_PROFILES = {
    ".comp":    "cs_6_2",
    ".vert":    "vs_6_2",
    ".frag":    "ps_6_2",
    ".rgen":    "lib_6_3",
    ".rahit":   "lib_6_3",
    ".rchit":   "lib_6_3",
    ".rmiss":   "lib_6_3",
}

# The same extension allow-list GenerateShaders.py passes to dxc, so that the HLSL half is built
# with the flags the host build uses. Without it dxc declares SPV_KHR_ray_query/RayQueryKHR for
# every module that has an OpTypeAccelerationStructureKHR, even one that only calls OpTraceRayKHR,
# which is what the capability comparison below is there to catch.
SPIRV_EXTENSIONS = [
    "SPV_KHR_ray_tracing",
    "SPV_EXT_descriptor_indexing",
    "SPV_KHR_compute_shader_derivatives",
]

# Extensions the target environment (vulkan1.2) has promoted to core. The compiler is free to
# declare or omit them, and the capabilities they gate are compared separately.
SPIRV_CORE_EXTENSIONS = [
    "SPV_EXT_descriptor_indexing",
]

# SPV_KHR_ray_query can not join SPIRV_EXTENSIONS, the list above. It is an allow-list entry of
# the compiler, and with the ray query extension permitted dxc declares RayQueryKHR and
# SPV_KHR_ray_query for every module that carries OpTypeAccelerationStructureKHR, TraceRay-only
# ones included (measured: RtRaygenDirect.rgen gets the extension beside SPV_KHR_ray_tracing once
# it is allowed), which is exactly the kind of unused capability the comparison is there to
# reject. The extension is therefore allowed per translation unit: a source whose
# comment-stripped text, or the text of anything it includes, uses the RayQuery type gets it
# (RsSmoke.vert.hlsl through SmokeLight.hlsli), everything else keeps the shared list. The scan
# has to answer the same question as the one in GenerateShaders.py, or the checker and the host
# build would compile the HLSL half with different flags.
SPIRV_RAY_QUERY_EXTENSION = "SPV_KHR_ray_query"

# The folders both compiler command lines pass with -I, in the order they pass them.
INCLUDE_FOLDERS = [".", "LPM", "CAS", "../Generated/"]

TAB = "  "


def findTool(name, arguments):
    try:
        r = subprocess.run([name] + arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    except FileNotFoundError:
        return None, name + " is not in PATH"
    if r.returncode != 0:
        return None, r.stdout
    return r.stdout, None


def addVulkanSdkToPath():
    sdkBin = os.path.join(os.environ.get("VULKAN_SDK", ""), "Bin")
    if os.path.isdir(sdkBin):
        os.environ["PATH"] = sdkBin + os.pathsep + os.environ["PATH"]


def getHLSLProfile(filename):
    for ext in STAGE_EXTENSIONS:
        if filename.endswith(ext + HLSL_SUFFIX):
            return ext
    return None


class Instruction:
    def __init__(self, resultId, opcode, operands):
        self.resultId = resultId
        self.opcode = opcode
        self.operands = operands


class Module:
    def __init__(self, text):
        self.instructions = []
        self.resultInstruction = {}
        # decorations[id][name] = [args]
        self.decorations = {}
        self.memberDecorations = {}
        self.names = {}
        self.entryPoints = []
        self.capabilities = []
        self.extensions = []
        self.executionModes = {}

        for line in text.splitlines():
            line = line.strip()
            if not line.startswith("%") and not line.startswith("Op"):
                continue

            tokens = line.split()
            resultId = None

            if tokens[0].startswith("%"):
                if len(tokens) < 3 or tokens[1] != "=":
                    continue
                resultId = tokens[0]
                tokens = tokens[2:]

            instruction = Instruction(resultId, tokens[0], tokens[1:])
            self.instructions.append(instruction)

            if resultId is not None:
                self.resultInstruction[resultId] = instruction

            self.parseSpecial(instruction)

        self.buildUsedIds()

    def parseSpecial(self, instruction):
        op = instruction.opcode
        o = instruction.operands

        if op == "OpDecorate":
            self.decorations.setdefault(o[0], {})[o[1]] = o[2:]
        elif op == "OpMemberDecorate":
            self.memberDecorations.setdefault((o[0], int(o[1])), {})[o[2]] = o[3:]
        elif op == "OpName":
            self.names[o[0]] = o[1].strip('"')
        elif op == "OpEntryPoint":
            self.entryPoints.append((o[0], o[1], o[2].strip('"'), o[3:]))
        elif op == "OpCapability":
            self.capabilities.append(o[0])
        elif op == "OpExtension":
            self.extensions.append(o[0].strip('"'))
        elif op == "OpExecutionMode":
            self.executionModes.setdefault(o[0], []).append(o[1:])

    def buildUsedIds(self):
        # A resource is used if it is referenced from a function body. Both compilers drop
        # unused resources, but they do not have to agree on which ones those are, and the
        # difference is worth reporting.
        self.usedIds = set()
        isInFunction = False

        for instruction in self.instructions:
            if instruction.opcode == "OpFunction":
                isInFunction = True
            elif instruction.opcode == "OpFunctionEnd":
                isInFunction = False
            elif isInFunction:
                for operand in instruction.operands:
                    if operand.startswith("%"):
                        self.usedIds.add(operand)

    # -- type helpers ------------------------------------------------------------------

    def typeOf(self, id):
        instruction = self.resultInstruction.get(id)
        if instruction is None or not instruction.opcode.startswith("OpType"):
            return None, []
        return instruction.opcode, instruction.operands

    def constantValue(self, id):
        instruction = self.resultInstruction.get(id)
        if instruction is None or not instruction.opcode.startswith("OpConstant"):
            return id
        return instruction.operands[1]

    def describeType(self, id, depth=0):
        op, o = self.typeOf(id)

        if op is None:
            return id
        if op == "OpTypeFloat" or op == "OpTypeInt":
            return ("float" if op == "OpTypeFloat" else ("int" if o[1] == "1" else "uint")) + o[0]
        if op == "OpTypeBool":
            return "bool"
        if op == "OpTypeVector":
            return "v" + o[1] + "(" + self.describeType(o[0], depth) + ")"
        if op == "OpTypeMatrix":
            return "m" + o[1] + "(" + self.describeType(o[0], depth) + ")"
        if op == "OpTypeArray":
            return "array[" + self.constantValue(o[1]) + "](" + self.describeType(o[0], depth) + ")"
        if op == "OpTypeRuntimeArray":
            return "array[](" + self.describeType(o[0], depth) + ")"
        if op == "OpTypeStruct":
            return "struct"
        if op == "OpTypePointer":
            return "ptr(" + self.describeType(o[1], depth) + ")"
        if op == "OpTypeImage":
            # An unknown depth is reported as 2 and a real depth image as 1; an unknown depth
            # is treated as not-depth instead of as a difference.
            dimension, depth, arrayed, ms, sampled, imageFormat = o[1:7]
            return "image:" + ":".join([dimension, "0" if depth == "2" else depth,
                                        arrayed, ms, sampled, imageFormat])
        if op == "OpTypeSampler":
            return "sampler"
        if op == "OpTypeSampledImage":
            return "sampledimage(" + self.describeType(o[0], depth) + ")"
        if op == "OpTypeAccelerationStructureKHR":
            return "accelerationstructure"
        if op == "OpTypeVoid":
            return "void"

        return op

    def arrayStride(self, id):
        return self.decorations.get(id, {}).get("ArrayStride", [None])[0]

    def isNonWritable(self, variableId, blockTypeId):
        if "NonWritable" in self.decorations.get(variableId, {}):
            return True
        return "NonWritable" in self.memberDecorations.get((blockTypeId, 0), {})

    # -- property extraction -----------------------------------------------------------

    def getDescribedBlocks(self):
        """Returns {property-path: [layout lines]} for blocks, keyed by where they are bound."""
        blocks = {}

        for instruction in self.instructions:
            if instruction.opcode != "OpVariable":
                continue

            storageClass = instruction.operands[1]
            if storageClass not in ["Uniform", "StorageBuffer", "PushConstant"]:
                continue
            if instruction.resultId not in self.usedIds:
                continue

            blockTypeId = instruction.operands[0]
            blockTypeId = self.pointee(blockTypeId)
            if self.typeOf(blockTypeId)[0] != "OpTypeStruct":
                continue

            path = self.bindingPath(instruction, storageClass)
            blocks["block " + path] = self.describeBlock(self.unwrapBlock(blockTypeId))

        return blocks

    def unwrapBlock(self, typeId):
        """Steps over the wrappers a block is wrapped in.

        A single-instance storage block (``buffer B { T x; }``) has no HLSL spelling: a struct
        member of a block cannot be addressed in HLSL, so the shader uses
        ``StructuredBuffer<T>`` (read) or ``RWStructuredBuffer<T>`` (written), and dxc describes
        that as an array of the struct. Stepping over that array compares the layout of the
        element, which is what the host binds for the spelling; the offsets of every member and
        the stride between elements stay under check.
        """
        while True:
            op, members = self.typeOf(typeId)
            if op != "OpTypeStruct" or len(members) != 1:
                return typeId
            if self.memberDecorations.get((typeId, 0), {}).get("Offset", ["?"])[0] != "0":
                return typeId

            memberOp, memberOperands = self.typeOf(members[0])
            if memberOp == "OpTypeStruct":
                typeId = members[0]
                continue
            if memberOp in ["OpTypeRuntimeArray", "OpTypeArray"]:
                elementId = memberOperands[0]
                if self.typeOf(elementId)[0] == "OpTypeStruct":
                    typeId = elementId
                    continue
            return typeId

    def pointee(self, id):
        op, o = self.typeOf(id)
        while op in ["OpTypePointer", "OpTypeArray", "OpTypeRuntimeArray"]:
            # OpTypePointer is <storage class> <pointee type>, the arrays are <element type> <length>.
            id = o[1] if op == "OpTypePointer" else o[0]
            op, o = self.typeOf(id)
        return id

    def describeBlock(self, typeId, depth=0, visited=None):
        visited = visited or set()
        if typeId in visited or depth > 4:
            return [TAB * depth + "... (recursion)"]

        visited = visited | {typeId}
        op, members = self.typeOf(typeId)
        lines = []

        for index, memberId in enumerate(members):
            decor = self.memberDecorations.get((typeId, index), {})
            offset = decor.get("Offset", ["?"])[0]

            description = TAB * depth + "[%d] offset %s %s" % (index, offset, self.describeType(memberId))

            # ArrayStride is decorated on the array type itself, not on its element type: a plain
            # array member therefore carries its stride here, where a lookup on the element would
            # find nothing and silently compare no stride at all.
            stride = self.arrayStride(memberId)
            if stride is not None:
                description += " stride " + stride
            if "MatrixStride" in decor:
                description += " matrixstride " + decor["MatrixStride"][0]

            lines.append(description)

            nested = self.nestedStruct(memberId)
            if nested is not None:
                lines += self.describeBlock(self.unwrapBlock(nested), depth + 1, visited)

        return lines

    def elementType(self, id):
        op, o = self.typeOf(id)
        if op in ["OpTypeArray", "OpTypeRuntimeArray"]:
            return o[0]
        return id

    def nestedStruct(self, id):
        op, o = self.typeOf(id)
        if op in ["OpTypeArray", "OpTypeRuntimeArray"]:
            return self.nestedStruct(o[0])
        if op == "OpTypeStruct":
            return id
        return None

    def bindingPath(self, instruction, storageClass):
        decor = self.decorations.get(instruction.resultId, {})
        if storageClass == "PushConstant":
            return "pushconstant"
        return "set " + decor.get("DescriptorSet", ["?"])[0] + " binding " + decor.get("Binding", ["?"])[0]

    def getDescribedDescriptors(self):
        """Returns {property-path: description} for the descriptors, keyed by their binding.

        Two properties are reported per descriptor: the pointee kind under
        "descriptor <set/binding>", and the storage class of the variable under
        "descriptor <set/binding>: storage class".
        """
        descriptors = {}

        for instruction in self.instructions:
            if instruction.opcode != "OpVariable":
                continue

            storageClass = instruction.operands[1]
            if storageClass not in ["UniformConstant", "Uniform", "StorageBuffer"]:
                continue
            if instruction.resultId not in self.usedIds:
                continue

            path = self.bindingPath(instruction, storageClass)
            pointeeId = self.pointee(instruction.operands[0])
            description = self.describeType(pointeeId)
            decor = self.decorations.get(instruction.resultId, {})

            # A read-only resource is NonWritable; for a uniform block the flag has no effect
            # on the descriptor, so it is not part of its description.
            if storageClass != "Uniform" and self.isNonWritable(instruction.resultId, pointeeId):
                description += " nonwritable"

            descriptors[path] = description

            # The storage class is the kind of descriptor the host binds: a Uniform block is
            # a VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, a StorageBuffer is a ..._STORAGE_BUFFER,
            # and a UniformConstant is a sampler, an image or a TLAS. It needs a property of
            # its own, because the pointee kind above cannot separate a uniform buffer from a
            # StructuredBuffer: dxc spells the latter as a struct holding a runtime array, so
            # both describe as "struct". The storage class separates them, and reporting it
            # separately names the resource in the recorded property list.
            descriptors[path + ": storage class"] = storageClass

        return descriptors

    def getEntryPointProperty(self):
        return ", ".join([model + " " + name for model, funcId, name, iface in self.entryPoints])

    def getWorkgroupSizeProperty(self):
        result = []
        for funcId, modes in self.executionModes.items():
            for mode in modes:
                if mode[0] == "LocalSize":
                    result.append(" ".join(mode[1:]))
        return ", ".join(sorted(result))

    def getSpecializationConstants(self):
        constants = {}

        for id, decor in self.decorations.items():
            if "SpecId" not in decor:
                continue
            instruction = self.resultInstruction.get(id)
            typeName = self.describeType(instruction.operands[0]) if instruction else "?"
            constants[decor["SpecId"][0]] = typeName + " = " + (instruction.operands[1] if instruction else "?")

        return constants

    def getInterfaceProperties(self):
        properties = {}

        for model, funcId, name, interface in self.entryPoints:
            for id in interface:
                decor = self.decorations.get(id, {})
                instruction = self.resultInstruction.get(id)
                pointeeId = self.pointee(instruction.operands[0]) if instruction else None

                # Builtins are the only interface of a compute or an RT shader, and their
                # names are language independent in SPIR-V (SV_DispatchThreadID becomes
                # GlobalInvocationId for both compilers).
                if "BuiltIn" in decor:
                    properties["builtin " + decor["BuiltIn"][0]] = self.builtinType(pointeeId)
                    continue

                # A block whose members carry BuiltIn decorations is compared through those
                # members, so the property list sees the same builtin whichever way the
                # compiler spells it. Only the members the module accesses are expanded --
                # the block type always carries PointSize, ClipDistance and CullDistance,
                # which a shader writing only the position neither reads nor writes.
                if self.typeOf(pointeeId)[0] == "OpTypeStruct":
                    expanded = self.expandBlockBuiltins(id, pointeeId)
                    if expanded:
                        properties.update(expanded)
                        continue

                if model not in ["Vertex", "Fragment"] or "Location" not in decor:
                    continue

                storageClass = instruction.operands[1] if instruction else "?"
                if storageClass not in ["Input", "Output"]:
                    continue

                key = storageClass.lower() + " location " + decor["Location"][0]
                properties[key] = self.describeType(pointeeId)

        return properties

    def builtinType(self, pointeeId):
        """The builtin's type with the integer sign normalized away.

        The vertex/instance index builtins are uint in HLSL; SPIR-V fixes the width but not the
        sign, and the host cannot see either. The comparison therefore keeps the width and drops
        the sign, so a genuinely different width still shows up.
        """
        description = self.describeType(pointeeId)
        if description.startswith("uint"):
            return "int" + description[len("uint"):]
        if "(uint" in description:
            return description.replace("(uint", "(int")
        return description

    def expandBlockBuiltins(self, blockVariableId, blockTypeId):
        """{property name: type} for the builtin members of a block the module accesses.

        The per-vertex block of a vertex shader declares Position, PointSize, ClipDistance and
        CullDistance; only the members the shader touches are compared, so the property list
        does not ask for the rest.
        """
        accessed = set()
        for instruction in self.instructions:
            if instruction.opcode != "OpAccessChain":
                continue
            if len(instruction.operands) < 3 or instruction.operands[1] != blockVariableId:
                continue
            accessed.add(self.constantValue(instruction.operands[2]))

        expanded = {}
        op, members = self.typeOf(blockTypeId)
        if op != "OpTypeStruct":
            return expanded
        for index, memberId in enumerate(members):
            decor = self.memberDecorations.get((blockTypeId, index), {})
            if "BuiltIn" not in decor or str(index) not in accessed:
                continue
            expanded["builtin " + decor["BuiltIn"][0]] = self.describeType(memberId)
        return expanded

    def getPropertySet(self):
        properties = {
            "entry point": self.getEntryPointProperty(),
            "workgroup size": self.getWorkgroupSizeProperty(),
        }

        for specId, value in sorted(self.getSpecializationConstants().items(), key=lambda i: int(i[0])):
            properties["constant_id " + specId] = value

        for path, description in self.getDescribedDescriptors().items():
            properties["descriptor " + path] = description

        for path, lines in self.getDescribedBlocks().items():
            properties[path] = "\n" + "\n".join(lines)

        for key, value in self.getInterfaceProperties().items():
            properties[key] = value

        # The capabilities and extensions the module declares. They are compared like any other
        # property, so the [MISMATCH] report names them. A capability appears in the property
        # path ("capability RayQueryKHR"), which is what a validator error refers to.
        for capability in self.capabilities:
            properties["capability " + capability] = "declared"
        for extension in self.extensions:
            if extension in SPIRV_CORE_EXTENSIONS:
                continue
            properties["extension " + extension] = "declared"

        return properties


def compileShader(sourcePath, outputPath):
    profile = getHLSLProfile(sourcePath)

    if profile is None:
        return False, sourcePath + " does not name a known shader stage"

    extensions = list(SPIRV_EXTENSIONS)
    if sourceUsesRayQuery(sourcePath):
        extensions.append(SPIRV_RAY_QUERY_EXTENSION)

    command = ["dxc", "-spirv", "-T", HLSL_PROFILES[profile], "-fspv-target-env=vulkan1.2"] + \
        ["-fspv-extension=" + ext for ext in extensions] + \
        getIncludeFoldersProcArg() + [sourcePath, "-Fo", outputPath]

    try:
        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    except FileNotFoundError:
        return False, command[0] + " is not in PATH"

    if r.returncode != 0:
        return False, r.stdout

    return True, None


def getIncludeFoldersProcArg():
    return [a for p in INCLUDE_FOLDERS for a in ("-I", p)]


# True when the translation unit of filename uses the HLSL RayQuery type, which is what decides
# whether SPV_KHR_ray_query is allowed for its compile (see above). The search runs over the
# comment-stripped text and follows the #include lines through INCLUDE_FOLDERS, so a header such
# as SmokeLight.hlsli is enough to make the stage that includes it a ray query user. The same
# question is answered the same way in GenerateShaders.py, because the host build and the checker
# have to compile the HLSL half identically.
def sourceUsesRayQuery(filename, visited=None):
    visited = visited if visited is not None else set()

    filename = os.path.abspath(filename).replace('\\', '/')
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
            for folder in INCLUDE_FOLDERS:
                included = os.path.abspath(folder + "/" + includeFile).replace('\\', '/')
                if os.path.exists(included) and sourceUsesRayQuery(included, visited):
                    return True

    return False


def disassemble(spvPath, txtPath):
    try:
        r = subprocess.run(["spirv-dis", "--no-color", spvPath, "-o", txtPath],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    except FileNotFoundError:
        return False, "spirv-dis is not in PATH"

    if r.returncode != 0:
        return False, r.stdout

    return True, None


def compareProperties(name, expectedProperties, actualProperties):
    """Returns (messages, mismatchCount) for a recording against a fresh reflection."""
    messages = []
    mismatches = 0
    keys = []
    expectedLabel = "recorded"
    actualLabel = "current"

    for key in expectedProperties:
        if key not in keys:
            keys.append(key)
    for key in actualProperties:
        if key not in keys:
            keys.append(key)

    for key in keys:
        expectedValue = expectedProperties.get(key, None)
        actualValue = actualProperties.get(key, None)

        if expectedValue == actualValue:
            continue

        mismatches += 1
        messages.append(TAB + "[MISMATCH] " + key)

        expectedText = "" if expectedValue is None else str(expectedValue)
        actualText = "" if actualValue is None else str(actualValue)

        if "\n" in expectedText or "\n" in actualText:
            # A block mismatch is easier to read as a diff than as two full dumps.
            diff = difflib.unified_diff(expectedText.split("\n"), actualText.split("\n"),
                expectedLabel, actualLabel, n=1, lineterm="")
            for line in diff:
                messages.append(TAB + TAB + line)
        else:
            messages.append(TAB + TAB + expectedLabel + ": " + expectedText)
            messages.append(TAB + TAB + actualLabel + ": " + actualText)

    return messages, mismatches


def getToolVersion(tool):
    output, error = findTool(tool, ["--version"])
    if output is None:
        return "unknown"
    lines = output.splitlines()
    return lines[0].strip() if lines else "unknown"


def getToolchainVersion():
    return "dxc " + getToolVersion("dxc") + "; spirv-dis " + getToolVersion("spirv-dis")


def getReflectionPath(pair):
    pair = pair.replace("\\", "/")
    if pair.startswith("./"):
        pair = pair[2:]
    return REFLECTION_FOLDER_PATH + pair + REFLECTION_SUFFIX


def serializeReflection(properties, toolchain):
    lines = ["# Format: " + str(REFLECTION_FORMAT), "# Toolchain: " + toolchain]

    for key in sorted(properties):
        lines.append("=== " + key)
        value = str(properties[key])
        if len(value) > 0:
            lines += value.split("\n")

    return "\n".join(lines) + "\n"


def parseReflection(text):
    properties = {}
    currentKey = None
    currentLines = []
    formatVersion = None

    for line in text.splitlines():
        if line.startswith("# "):
            if currentKey is not None:
                properties[currentKey] = "\n".join(currentLines)
                currentKey = None
                currentLines = []
            match = re.match(r"# Format: (\d+)$", line)
            if match:
                formatVersion = int(match.group(1))
            continue

        if line.startswith("=== "):
            if currentKey is not None:
                properties[currentKey] = "\n".join(currentLines)
            currentKey = line[4:]
            currentLines = []
            if len(currentKey) == 0 or currentKey in properties:
                return None, "empty or duplicate key"
            continue

        if currentKey is None:
            return None, "value line outside a key"

        currentLines.append(line)

    if currentKey is not None:
        properties[currentKey] = "\n".join(currentLines)

    if formatVersion is None:
        return None, "the # Format header is missing"
    if formatVersion != REFLECTION_FORMAT:
        return None, "format " + str(formatVersion) + " is not supported"

    return properties, None


def reflectHLSL(pair):
    baseName = os.path.basename(pair)
    hlslPath = pair + HLSL_SUFFIX
    hlslSpvPath = TEMP_FOLDER_PATH + baseName + ".hlsl.spv"

    success, compilerOutput = compileShader(hlslPath, hlslSpvPath)
    if not success:
        return None, [TAB + "dxc failed", compilerOutput]

    hlslTxtPath = TEMP_FOLDER_PATH + baseName + ".hlsl.spv.txt"

    if not disassemble(hlslSpvPath, hlslTxtPath)[0]:
        return None, [TAB + "spirv-dis failed"]

    with open(hlslTxtPath, "r", encoding="utf-8") as f:
        hlslModule = Module(f.read())

    return hlslModule.getPropertySet(), None


def checkRecording(pair):
    reflectionPath = getReflectionPath(pair)

    if not os.path.isfile(reflectionPath):
        return 1, [TAB + "no recording: " + reflectionPath,
                   TAB + "run CheckShaderProperties.py --record to create it"]

    with open(reflectionPath, "r", encoding="utf-8-sig") as f:
        recordedProperties, parseError = parseReflection(f.read())
    if parseError is not None:
        return 1, [TAB + "malformed recording " + reflectionPath + ": " + parseError]

    hlslProperties, messages = reflectHLSL(pair)
    if hlslProperties is None:
        return 1, messages

    messages, mismatches = compareProperties(pair, recordedProperties, hlslProperties)
    return mismatches, messages


def recordPairs(pairs, force):
    propertiesByPair = {}
    failed = 0

    for pair in pairs:
        print("=== " + pair)
        hlslProperties, messages = reflectHLSL(pair)
        if hlslProperties is None:
            for message in messages:
                print(message)
            failed += 1
            continue
        propertiesByPair[pair] = hlslProperties

    if failed > 0:
        print("")
        print("> " + str(failed) + " pair(s) failed to compile or disassemble; no recording")
        print("> was written. Fix the HLSL first.")
        return 1

    toolchain = getToolchainVersion()
    written = 0
    unchanged = 0
    skipped = 0

    for pair in pairs:
        hlslProperties = propertiesByPair[pair]
        reflectionPath = getReflectionPath(pair)

        if os.path.isfile(reflectionPath):
            with open(reflectionPath, "r", encoding="utf-8-sig") as f:
                recordedProperties, parseError = parseReflection(f.read())

            if parseError is None and recordedProperties == hlslProperties:
                unchanged += 1
                continue

            if not force:
                skipped += 1
                if parseError is not None:
                    print(TAB + "malformed recording " + reflectionPath + ": " + parseError)
                    print(TAB + "rerun with --force to replace it")
                else:
                    print(TAB + "recording differs: " + reflectionPath)
                    messages, mismatches = compareProperties(pair, recordedProperties,
                        hlslProperties)
                    for message in messages:
                        print(message)
                    print(TAB + str(mismatches) +
                        " recorded propertie(s) differ; rerun with --force to replace it")
                continue

        folder = os.path.dirname(reflectionPath)
        if folder and not os.path.isdir(folder):
            os.makedirs(folder)
        with open(reflectionPath, "w", encoding="utf-8", newline="\n") as f:
            f.write(serializeReflection(hlslProperties, toolchain))
        written += 1
        print(TAB + "recorded " + reflectionPath)

    print("")
    print("> " + str(written) + " recording(s) written, " + str(unchanged) + " unchanged, " +
          str(skipped) + " skipped.")
    return 1 if skipped > 0 else 0


def findOrphanRecordings(pairs):
    recordings = []
    for folder, _, entries in os.walk(REFLECTION_FOLDER_PATH):
        for entry in entries:
            if entry.endswith(REFLECTION_SUFFIX):
                recordings.append(os.path.join(folder, entry).replace("\\", "/"))

    expected = set(getReflectionPath(pair) for pair in pairs)
    return sorted(recording for recording in recordings if recording not in expected)


def resolveProbeName(name):
    if os.path.isfile(name + HLSL_SUFFIX):
        return name
    if os.path.isfile(HLSL_FOLDER_PATH + name + HLSL_SUFFIX):
        return HLSL_FOLDER_PATH + name
    if os.path.isfile(PROBES_FOLDER_PATH + name + HLSL_SUFFIX):
        return PROBES_FOLDER_PATH + name
    return name


def getPairs(arguments):
    if len(arguments) > 0:
        # Arguments may name the HLSL file or the shader itself, e.g. EfWaves.comp.hlsl or
        # EfWaves.comp. A probe can be named without its folder.
        names = [a[:-len(HLSL_SUFFIX)] if a.endswith(HLSL_SUFFIX) else a for a in arguments]
        return [resolveProbeName(name) for name in names]

    pairs = []
    for folder in ["./", HLSL_FOLDER_PATH, PROBES_FOLDER_PATH]:
        if not os.path.isdir(folder):
            continue
        for entry in sorted(os.listdir(folder)):
            if entry.endswith(HLSL_SUFFIX) and getHLSLProfile(entry) is not None:
                pairs.append(folder + entry[:-len(HLSL_SUFFIX)])

    return pairs


def main():
    addVulkanSdkToPath()

    if not os.path.isdir(TEMP_FOLDER_PATH):
        os.makedirs(TEMP_FOLDER_PATH)

    flags = [a for a in sys.argv[1:] if a.startswith("-")]
    unknown = [a for a in flags if a not in ["--record", "--force"]]
    if len(unknown) > 0:
        print("> Unknown option(s): " + " ".join(unknown))
        print("> Usage: python CheckShaderProperties.py [--record [--force]] [shader ...]")
        return 1

    record = "--record" in flags
    force = "--force" in flags

    if force and not record:
        print("> --force is only valid together with --record.")
        return 1

    arguments = [a for a in sys.argv[1:] if not a.startswith("-")]
    pairs = getPairs(arguments)

    if record:
        if len(pairs) == 0:
            print("> No HLSL shaders to record. Nothing to do.")
            return 0
        return recordPairs(pairs, force)

    if len(pairs) == 0:
        print("> No HLSL shaders to check. Nothing to do.")
        return 0

    orphans = findOrphanRecordings(getPairs([]))
    if len(orphans) > 0:
        print("> " + str(len(orphans)) + " recording(s) do not correspond to a current shader pair:")
        for orphan in orphans:
            print(TAB + orphan)
        print("> Delete them, or add the pair they record to the tree.")
        return 1

    failures = 0

    for pair in pairs:
        print("=== " + pair)
        pairFailures, messages = checkRecording(pair)
        failures += pairFailures

        for message in messages:
            print(message)

        if pairFailures == 0:
            print(TAB + "all properties match")

    if failures > 0:
        print("")
        print("> " + str(failures) + " failure(s) against the recorded reflections.")
        print("> Fix them in HLSL, or run --record --force if the change is deliberate.")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
