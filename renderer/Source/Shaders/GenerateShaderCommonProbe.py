"""Generates the ShaderCommon probe from the generated header.

The generated header has no shader stage of its own, so it can not be compiled on its own and
CheckShaderProperties.py can not compare it. This script reads the header back and writes a
compute shader whose only purpose is to instantiate everything it declares:

    Probes/ShaderCommon.probe.comp.hlsl  -- includes Shaders/ShaderCommonHLSL.hlsli

The probe is derived from ShaderCommonHLSL.hlsli itself: the structs, the framebuffer
declarations and their bindings are parsed out of the header and turned into statements that
touch every member.

Every framebuffer has to be touched, not sampled: dxc drops the declarations that are not
referenced, and the checker only compares the variables that appear inside a function. A
declaration that is never used therefore can not be checked.

Run it from this folder, the same way as GenerateShaders.py:

    python GenerateShaderCommonProbe.py
"""

import os
import re
import sys

HLSL_HEADER = "../Generated/ShaderCommonHLSL.hlsli"
HLSL_OUTPUT = "Probes/ShaderCommon.probe.comp.hlsl"

# The probe binds its own buffers into a set that nothing else uses, so that the bindings of the
# framebuffers and of the global uniform stay free.
PROBE_DESC_SET = 2

# One set and one binding per probe buffer.
PROBE_BUFFER_BINDINGS = [
    ("ShVertex", "vertices"),
    ("ShGeometryInstance", "geometryInstances"),
    ("ShTonemapping", "tonemapping"),
    ("ShLightEncoded", "lights"),
    ("ShIndirectDrawCommand", "drawCmds"),
    ("ShDecalInstance", "decals"),
]
PROBE_PORTALS_BINDING = 7
PROBE_OUTPUT_BINDING = 8

STRUCT_RE = re.compile(r"^struct (\w+)\s*\{(.*?)^\};", re.S | re.M)
MEMBER_RE = re.compile(r"^(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?;$")

HLSL_STORAGE_RE = re.compile(
    r'\[\[vk::binding\((\d+), DESC_SET_FRAMEBUFFERS\), vk::image_format\("(\w+)"\)\]\] '
    r'RWTexture2D<(\w+)> (\w+);')
HLSL_SAMPLED_RE = re.compile(
    r"\[\[vk::binding\((\d+), DESC_SET_FRAMEBUFFERS\)\]\] Texture2D<(\w+)> (\w+);")
HLSL_SAMPLER_RE = re.compile(
    r"\[\[vk::binding\((\d+), DESC_SET_FRAMEBUFFERS\)\]\] SamplerState (\w+);")


def fail(message):
    sys.exit("GenerateShaderCommonProbe.py: " + message)


def read_structs():
    """Returns {name: [(type, memberName, arraySize or None)]} in declaration order."""
    structs = {}
    for match in STRUCT_RE.finditer(open(HLSL_HEADER, encoding="utf-8").read()):
        members = []
        for line in match.group(2).splitlines():
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            member = MEMBER_RE.match(line)
            if member is None:
                fail("can not parse the member %r of the struct %s" % (line, match.group(1)))
            members.append((member.group(1), member.group(2),
                            int(member.group(3)) if member.group(3) else None))
        structs[match.group(1)] = members
    for required in ["ShVertex", "ShGeometryInstance", "ShTonemapping", "ShLightEncoded",
                      "ShIndirectDrawCommand", "ShDecalInstance",
                     "ShPortalInstance", "ShVertPreprocessing", "ShGlobalUniform"]:
        if required not in structs:
            fail("the header does not define the struct " + required)
    return structs


def member_accessor(hlsl_type, reference):
    """Returns an expression that reads a single component of the member.

    Matrices are declared as the transpose of the column-major original, so the [0][0] element
    of a matrix and the x component of a vector are the same in every spelling.
    """
    if hlsl_type == "float":
        return reference
    if hlsl_type in ("int", "uint"):
        return "float(%s)" % reference
    if hlsl_type in ("float2", "float3", "float4"):
        return reference + ".x"
    if hlsl_type in ("int2", "int3", "int4", "uint2", "uint3", "uint4"):
        return "float(%s.x)" % reference
    if re.match(r"^float[234]x[234]$", hlsl_type):
        return reference + "[0][0]"
    fail("no accessor for the type " + hlsl_type)


def wrap(items, statement, prefix="    ", width=100):
    """Joins the expressions into one statement, wrapped to a readable width.

    The continuation lines are indented under the first item, as the statement itself is only
    written once.
    """
    lines, current = [], ""
    for index, item in enumerate(items):
        piece = item if index == len(items) - 1 else item + " +"
        if current and len(current) + 1 + len(piece) > width:
            lines.append(current)
            current = piece
        else:
            current = (current + " " + piece).strip()
    lines.append(current)
    indent = prefix + " " * len(statement)
    return [prefix + statement + lines[0]] + [indent + line for line in lines[1:]]


def struct_board(structs, struct_name, reference, indent="    "):
    """Returns the statements that read every member of the struct."""
    items = []
    for member_type, member_name, array_size in structs[struct_name]:
        member = reference + "." + member_name
        if array_size is not None:
            member += "[0]"
        items.append(member_accessor(member_type, member))
    lines = wrap(items, "v += ", prefix=indent)
    lines[-1] += ";"
    return lines


def read_framebuffers():
    """Returns the framebuffer slots, ordered by binding, as a list of dicts.

    A slot is one storage image together with its sampled view and its sampler. The three halves
    are declared separately, with the same base name, and the script fails if that is not so:
    the probe relies on the naming to touch all three of them.
    """
    slots = {}
    for line in open(HLSL_HEADER, encoding="utf-8"):
        line = line.strip()
        match = HLSL_STORAGE_RE.match(line)
        if match:
            slots[match.group(4)] = {"binding": int(match.group(1)), "format": match.group(2),
                                     "component": match.group(3)}
            continue
        match = HLSL_SAMPLED_RE.match(line)
        if match:
            slots[strip_suffix(match.group(3), "_Sampled")]["sampledComponent"] = match.group(2)
            continue
        match = HLSL_SAMPLER_RE.match(line)
        if match:
            slots[strip_suffix(match.group(2), "_Sampler")]["sampler"] = True

    if not slots:
        fail("no framebuffer is declared by " + HLSL_HEADER)

    for name, slot in slots.items():
        if "sampledComponent" not in slot or "sampler" not in slot:
            fail("the framebuffer %s has no matching %s" % (name, "_Sampled/_Sampler"))
        if slot["sampledComponent"] != slot["component"]:
            fail("the framebuffer %s is stored as %s and sampled as %s"
                 % (name, slot["component"], slot["sampledComponent"]))
        slot["unsigned"] = slot["component"] == "uint4"

    return [dict(slot, name=name) for name, slot in
            sorted(slots.items(), key=lambda item: item[1]["binding"])]


def strip_suffix(name, suffix):
    if not name.endswith(suffix):
        fail("the expected the name suffix %s in %s" % (suffix, name))
    return name[: -len(suffix)]


def hlsl_framebuffer_board(framebuffers, reference_texture):
    """Returns the statements that touch every framebuffer of the HLSL header."""
    lines = [
        "    // One slot per framebuffer: its storage image, its sampled view and its sampler.",
        "    // The sampled view is fetched and sampled too. A uint texture can not be sampled,",
        "    // so one float texture samples every sampler of the table in turn.",
    ]
    for slot in framebuffers:
        name, unsigned = slot["name"], slot["unsigned"]
        value = "uint4(1u, 1u, 1u, 1u)" if unsigned else "float4(1.0, 1.0, 1.0, 1.0)"
        lines.append("")
        lines.append("    // %s  %s" % (name, slot["format"]))
        lines.append("    %s[pix] = %s;" % (name, value))
        lines.append("    v += float(%s[pix].x);" % name)
        lines.append("    v += float(%s_Sampled.Load(int3(pix, 0)).x);" % name)
        lines.append("    v += %s.SampleLevel(%s_Sampler, uv, 0.0).x;"
                     % (reference_texture, name))
    return lines


HLSL_TEMPLATE = """// Generated by GenerateShaderCommonProbe.py, do not edit by hand. The probe has no shader stage
// of its own: it exists so that CheckShaderProperties.py can compile the generated header and
// pin the layouts, the bindings and the framebuffer declarations it derives. Every framebuffer
// is touched, because dxc drops the unused declarations and the checker can only compare the
// ones inside a function.

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1

#include "ShaderCommonHLSL.hlsli"

#define PROBE_DESC_SET %(probe_set)d

struct Push_BT
{
    ShVertPreprocessing push;
};

// A fixed size array needs an explicit wrapper, because a block that contains an array is not
// unwrapped by the checker, while a block that contains a single struct is.
struct ProbeShPortalInstance_BT
{
    ShPortalInstance portals[PORTAL_MAX_COUNT];
};

[[vk::push_constant]] ConstantBuffer<Push_BT> pushConstant;

%(probe_buffers)s

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    float v = 0.0;

    uint2 pix = uint2(0, 0);
    float2 uv = float2(0.5, 0.5);

%(uniform_board)s

%(push_board)s

%(struct_boards)s

%(framebuffer_board)s

    probeOutput[0] = v;
}
"""


def probe_buffers_lines():
    """Returns the declarations of the probe buffers."""
    lines = []
    for binding, (struct_name, name) in enumerate(PROBE_BUFFER_BINDINGS):
        lines.append("[[vk::binding(%d, PROBE_DESC_SET)]] %-52s %s;"
                     % (binding, "StructuredBuffer<%s>" % struct_name, name))
    lines.append("[[vk::binding(%d, PROBE_DESC_SET)]] ConstantBuffer<ProbeShPortalInstance_BT> "
                 "portalBuffer;" % PROBE_PORTALS_BINDING)
    lines.append("[[vk::binding(%d, PROBE_DESC_SET)]] RWStructuredBuffer<float> "
                 "probeOutput;" % PROBE_OUTPUT_BINDING)
    return lines


def struct_boards_text(structs, portals_reference):
    """Returns the statements that read every member of every probe buffer struct.

    The portal instance is reached through the constant buffer member: a constant buffer is
    subscripted through its member, unlike a block that exposes its members directly.
    """
    boards = []
    for struct_name, name in PROBE_BUFFER_BINDINGS:
        boards.append("    // " + struct_name)
        boards.extend(struct_board(structs, struct_name, name + "[0]"))
        boards.append("")
    boards.append("    // ShPortalInstance")
    boards.extend(struct_board(structs, "ShPortalInstance", portals_reference))
    return "\n".join(boards)


def main():
    structs = read_structs()
    framebuffers = read_framebuffers()

    hlsl = HLSL_TEMPLATE % {
        "probe_set": PROBE_DESC_SET,
        "probe_buffers": "\n".join(probe_buffers_lines()),
        "uniform_board": "\n".join(["    // ShGlobalUniform"] +
                                   struct_board(structs, "ShGlobalUniform", "globalUniform")),
        "push_board": "\n".join(["    // ShVertPreprocessing"] +
                                struct_board(structs, "ShVertPreprocessing", "pushConstant.push")),
        "struct_boards": struct_boards_text(structs, "portalBuffer.portals[0]"),
        "framebuffer_board": "\n".join(
            hlsl_framebuffer_board(framebuffers, "framebufAlbedo_Sampled")),
    }

    for path, text in [(HLSL_OUTPUT, hlsl)]:
        folder = os.path.dirname(path)
        if folder and not os.path.isdir(folder):
            os.makedirs(folder)
        with open(path, "w", encoding="utf-8", newline="\n") as file:
            file.write(text)
        print("wrote %s, %d lines" % (path, text.count("\n")))


if __name__ == "__main__":
    main()
