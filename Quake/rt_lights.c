// rt_lights.c -- the dynamic-light overrides of lights.yaml (see rt_lights.h).
//
// The files are read once per map (the editor reloads them through its own
// session flow): the base directory's lights.yaml first, then the running
// gamedir's, with a later file replacing the entries of an earlier one -- the
// same precedence the materials files have.

#include "quakedef.h"
#include "rt_material.h"
#include "rt_lights.h"
#include "atomics.h"

#include <yaml.h>

#define RT_LIGHT_CAP RT_LIGHT_NAMES_MAX

static rt_light_t rt_lights[RT_LIGHT_CAP];
static int        rt_light_count = 0;
static qboolean   rt_light_initialized = false;

// The lights uploaded in the current frame (see RT_TRACK_Light): the counter is
// atomic because the uploads may come from the render tasks.
static rt_tracked_light_t rt_tracked[RT_TRACKED_LIGHTS_MAX];
static atomic_uint32_t    rt_tracked_count;

void RT_TRACK_BeginFrame(void)
{
    memset(rt_tracked, 0, sizeof(rt_tracked));
    Atomic_StoreUInt32(&rt_tracked_count, 0);
}

void RT_TRACK_Light(const vec3_t position, float radius, const vec3_t color,
                    uint64_t uniqueID, int kind, const char *name)
{
    uint32_t index = Atomic_AddUInt32(&rt_tracked_count, 1);
    rt_tracked_light_t *light;

    if (index >= RT_TRACKED_LIGHTS_MAX)
    {
        /* The editor reads this list for its wireframes and picking, so a light
           the list refused is one it cannot show or select. Say it once a run. */
        static qboolean warned;

        if (!warned)
        {
            Con_DWarning("RT: more than %d lights in one frame; the rest are not tracked "
                         "(the editor cannot show or pick them)\n", RT_TRACKED_LIGHTS_MAX);
            warned = true;
        }
        return;
    }

    light = &rt_tracked[index];
    VectorCopy(position, light->position);
    light->radius = radius;
    VectorCopy(color, light->color);
    light->uniqueID = uniqueID;
    light->kind = kind;
    q_strlcpy(light->name, name ? name : "", sizeof(light->name));
    // published last: a reader that sees the count (or the ready flag) sees the
    // fields too. The editor's readers also wait for the draw task that fills the
    // list when tasks are on (gl_screen.c), so the frame's list is complete.
    light->ready = 1;
}

const rt_tracked_light_t *RT_TRACK_Lights(int *outCount)
{
    uint32_t count = Atomic_LoadUInt32(&rt_tracked_count);

    if (count > RT_TRACKED_LIGHTS_MAX)
    {
        count = RT_TRACKED_LIGHTS_MAX;
    }
    if (outCount)
    {
        *outCount = (int)count;
    }
    return rt_tracked;
}

static const char *rt_light_header =
    "# Dynamic light overrides for the vkpt ray-traced renderer.\n"
    "# A light belongs to an emitter: the texture a model or a sprite draws (the\n"
    "# same name materials.yaml uses for it), the model of the entity that asked\n"
    "# for a legacy dlight, or the classname of a map light entity -- classname\n"
    "# entries apply where the legacy light system uploads those entities\n"
    "# (rt_truelight 0); the editor itself runs on rt_truelight 1 and shows the\n"
    "# lights that system builds.\n"
    "#   light_radius    -- the size of the light (rt_dlight_radius units)\n"
    "#   light_intensity -- the brightness of the light (a multiplier of its colour)\n"
    "#   light_offset    -- \"x y z\", the offset from the emitter's pivot point\n"
    "#   light_color     -- \"rrggbb\", an explicit colour for the light\n"
    "#   force_rasterize -- draw the emitter in the rasterized path (material lights)\n"
    "#   group_edit      -- true (the default) when an edit of one light of the\n"
    "#                      group (the emitter's model) is written to all of them\n"
    "# An emitter without an entry uses the global rt_dlight_* settings.\n";

// The name the editor and the renderer agree on: the normalized texture name
// with a file extension stripped (a model skin keeps its ":frameN"). An
// instance key ("name#id") keeps its id and normalizes the name before it.
static void rt_light_norm(const char *name, char *out, size_t outsize)
{
    char  buf[MAX_QPATH];
    char *hash;
    char *dot;

    q_strlcpy(buf, name, sizeof(buf));
    hash = strchr(buf, '#');
    if (hash)
    {
        *hash = '\0';
    }

    RT_MAT_NormalizeName(buf, out, outsize);
    dot = strrchr(out, '.');
    if (dot && !strchr(dot, ':'))
    {
        *dot = '\0';
    }

    if (hash)
    {
        size_t len = strlen(out);

        if (len + 1 < outsize)
        {
            q_snprintf(out + len, outsize - len, "#%s", hash + 1);
        }
    }
}

static rt_light_t *rt_light_find_in(const char *normalized)
{
    int i;

    for (i = 0; i < rt_light_count; i++)
    {
        if (rt_lights[i].valid && !strcmp(rt_lights[i].name, normalized))
        {
            return &rt_lights[i];
        }
    }
    return NULL;
}

rt_light_t *RT_LIGHT_List(int *outCount)
{
    if (outCount)
    {
        *outCount = rt_light_count;
    }
    return rt_lights;
}

void RT_LIGHT_SetCount(int count)
{
    if (count < 0)
    {
        count = 0;
    }
    if (count > RT_LIGHT_CAP)
    {
        count = RT_LIGHT_CAP;
    }
    rt_light_count = count;
}

rt_light_t *RT_LIGHT_Find(const char *name)
{
    char normalized[MAX_QPATH];

    if (!rt_light_initialized || !name || !name[0])
    {
        return NULL;
    }
    rt_light_norm(name, normalized, sizeof(normalized));
    return rt_light_find_in(normalized);
}

rt_light_t *RT_LIGHT_Ensure(const char *name)
{
    rt_light_t *light;
    char        normalized[MAX_QPATH];

    if (!rt_light_initialized || !name || !name[0])
    {
        return NULL;
    }

    rt_light_norm(name, normalized, sizeof(normalized));
    light = rt_light_find_in(normalized);
    if (light)
    {
        return light;
    }
    if (rt_light_count >= RT_LIGHT_CAP)
    {
        return NULL;
    }

    light = &rt_lights[rt_light_count++];
    memset(light, 0, sizeof(*light));
    light->valid = true;
    light->group_edit = true; // part of its group until the flag says otherwise
    q_strlcpy(light->name, normalized, sizeof(light->name));
    return light;
}

void RT_LIGHT_MakeKey(const char *name, uint64_t uniqueID, char *out, size_t outsize)
{
    char normalized[MAX_QPATH];

    rt_light_norm(name, normalized, sizeof(normalized));
    q_snprintf(out, outsize, "%s#%llu", normalized, (unsigned long long)uniqueID);
}

rt_light_t *RT_LIGHT_FindInstance(const char *name, uint64_t uniqueID)
{
    char  key[MAX_QPATH];
    rt_light_t *light;

    if (!rt_light_initialized || !name || !name[0])
    {
        return NULL;
    }

    RT_LIGHT_MakeKey(name, uniqueID, key, sizeof(key));
    light = rt_light_find_in(key);
    return light ? light : RT_LIGHT_Find(name);
}

rt_light_t *RT_LIGHT_EnsureInstance(const char *name, uint64_t uniqueID)
{
    char key[MAX_QPATH];

    RT_LIGHT_MakeKey(name, uniqueID, key, sizeof(key));
    return RT_LIGHT_Ensure(key);
}

void RT_LIGHT_Remove(const char *name)
{
    char        normalized[MAX_QPATH];
    rt_light_t *light;
    int         index;

    if (!rt_light_initialized || !name || !name[0])
    {
        return;
    }

    rt_light_norm(name, normalized, sizeof(normalized));
    light = rt_light_find_in(normalized);
    if (!light)
    {
        return;
    }

    index = (int)(light - rt_lights);
    for (; index + 1 < rt_light_count; index++)
    {
        rt_lights[index] = rt_lights[index + 1];
    }
    rt_light_count--;
}

qboolean RT_LIGHT_HasFields(const rt_light_t *l)
{
    return l && (l->has_radius || l->has_intensity || l->has_offset || l->has_color ||
                 l->force_rasterize || !l->group_edit);
}

const char *RT_LIGHT_Header(void)
{
    return rt_light_header;
}

void RT_LIGHT_WriteEntry(FILE *f, const rt_light_t *l)
{
    fprintf(f, "  - name: %s\n", l->name);
    if (l->has_radius)
    {
        fprintf(f, "    light_radius: %.6g\n", l->radius);
    }
    if (l->has_intensity)
    {
        fprintf(f, "    light_intensity: %.6g\n", l->intensity);
    }
    if (l->has_offset)
    {
        fprintf(f, "    light_offset: %.6g %.6g %.6g\n", l->offset[0], l->offset[1], l->offset[2]);
    }
    if (l->has_color)
    {
        fprintf(f, "    light_color: %02x%02x%02x\n",
                (int)(l->color[0] * 255.0f + 0.5f) & 0xff,
                (int)(l->color[1] * 255.0f + 0.5f) & 0xff,
                (int)(l->color[2] * 255.0f + 0.5f) & 0xff);
    }
    if (l->force_rasterize)
    {
        fprintf(f, "    force_rasterize: true\n");
    }
    fprintf(f, "    group_edit: %s\n", l->group_edit ? "true" : "false");
}

// Trims a scalar: leading/trailing space and tab, and a pair of quotes.
static void rt_light_trim(char *text, size_t outsize)
{
    char *start = text;
    char *end;
    size_t len;

    while (*start == ' ' || *start == '\t')
    {
        start++;
    }
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
    {
        end--;
    }
    *end = '\0';

    len = (size_t)(end - start);
    if (len >= 2 && start[0] == '"' && start[len - 1] == '"')
    {
        start++;
        len -= 2;
        start[len] = '\0';
    }
    q_strlcpy(text, start, outsize);
}

// "rrggbb" (a leading # is accepted) -> rgb in 0..1.
static qboolean rt_light_parse_hex(const char *value, vec3_t out)
{
    int i, c;

    while (*value == ' ' || *value == '\t')
    {
        value++;
    }
    if (*value == '#')
    {
        value++;
    }
    if (strlen(value) != 6)
    {
        return false;
    }

    for (i = 0; i < 3; i++)
    {
        int hi = value[i * 2];
        int lo = value[i * 2 + 1];

        if (hi >= '0' && hi <= '9')       hi -= '0';
        else if (hi >= 'a' && hi <= 'f')  hi = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F')  hi = hi - 'A' + 10;
        else return false;

        if (lo >= '0' && lo <= '9')       lo -= '0';
        else if (lo >= 'a' && lo <= 'f')  lo = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F')  lo = lo - 'A' + 10;
        else return false;

        c = hi * 16 + lo;
        out[i] = (float)c / 255.0f;
    }
    return true;
}

static qboolean rt_light_parse_bool(const char *value)
{
    if (!q_strcasecmp(value, "true") || !q_strcasecmp(value, "yes") || !q_strcasecmp(value, "on"))
    {
        return true;
    }
    if (!q_strcasecmp(value, "false") || !q_strcasecmp(value, "no") || !q_strcasecmp(value, "off"))
    {
        return false;
    }
    return atoi(value) != 0;
}

static int rt_light_load_file(const char *path)
{
    FILE       *f = fopen(path, "r");
    char        line[1024];
    rt_light_t *cur = NULL;
    int         loaded = 0;

    if (!f)
    {
        return 0;
    }

    while (fgets(line, sizeof(line), f))
    {
        char *p = line;
        char *colon;

        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n')
        {
            continue;
        }

        if (!strncmp(p, "- name:", 7) || !strncmp(p, "name:", 5))
        {
            char name[MAX_QPATH];

            p = strchr(p, ':') + 1;
            q_strlcpy(name, p, sizeof(name));
            rt_light_trim(name, sizeof(name));
            q_strlwr(name);
            if (name[0])
            {
                rt_light_t *light = rt_light_find_in(name);

                if (light)
                {
                    // a later file replaces the fields of the earlier entry
                    memset(light, 0, sizeof(*light));
                }
                else if (rt_light_count < RT_LIGHT_CAP)
                {
                    light = &rt_lights[rt_light_count++];
                    memset(light, 0, sizeof(*light));
                }
                else
                {
                    light = NULL;
                }

                if (light)
                {
                    light->valid = true;
                    light->group_edit = true; // the default: a light follows its group
                    q_strlcpy(light->name, name, sizeof(light->name));
                }
                cur = light;
                loaded++;
            }
            continue;
        }

        if (!cur)
        {
            continue;
        }

        colon = strchr(p, ':');
        if (!colon)
        {
            continue;
        }
        *colon = '\0';
        {
            char key[64];
            char value[256];

            q_strlcpy(key, p, sizeof(key));
            rt_light_trim(key, sizeof(key));
            q_strlcpy(value, colon + 1, sizeof(value));
            rt_light_trim(value, sizeof(value));

            if (!q_strcasecmp(key, "light_radius"))
            {
                cur->radius = (float)atof(value);
                cur->has_radius = true;
            }
            else if (!q_strcasecmp(key, "light_intensity"))
            {
                cur->intensity = (float)atof(value);
                cur->has_intensity = true;
            }
            else if (!q_strcasecmp(key, "light_color"))
            {
                vec3_t rgb;

                if (rt_light_parse_hex(value, rgb))
                {
                    VectorCopy(rgb, cur->color);
                    cur->has_color = true;
                }
            }
            else if (!q_strcasecmp(key, "force_rasterize"))
            {
                cur->force_rasterize = rt_light_parse_bool(value);
            }
            else if (!q_strcasecmp(key, "group_edit"))
            {
                cur->group_edit = rt_light_parse_bool(value);
            }
            else if (!q_strcasecmp(key, "light_offset"))
            {
                float x = 0.0f, y = 0.0f, z = 0.0f;

                if (sscanf(value, "%f %f %f", &x, &y, &z) == 3)
                {
                    cur->offset[0] = x;
                    cur->offset[1] = y;
                    cur->offset[2] = z;
                    cur->has_offset = true;
                }
            }
        }
    }

    fclose(f);
    return loaded;
}

static void rt_light_load_directory(const char *dir)
{
    char path[MAX_OSPATH];

    q_snprintf(path, sizeof(path), "%s/lights.yaml", dir);
    if (Sys_FileTime(path) != -1)
    {
        int loaded = rt_light_load_file(path);

        if (loaded > 0)
        {
            Con_Printf("RT: loaded %d light overrides from %s\n", loaded, path);
        }
    }
}

void RT_LIGHT_Reload(void)
{
    char base[MAX_OSPATH];

    if (!rt_light_initialized)
    {
        return;
    }

    rt_light_count = 0;

    q_snprintf(base, sizeof(base), "%s/id1", com_basedir);
    if (q_strcasecmp(base, com_gamedir))
    {
        rt_light_load_directory(base);
    }
    rt_light_load_directory(com_gamedir);
}

void RT_LIGHT_Init(void)
{
    if (rt_light_initialized)
    {
        return;
    }
    rt_light_initialized = true;
    RT_LIGHT_Reload();
}

void RT_LIGHT_Shutdown(void)
{
    rt_light_initialized = false;
    rt_light_count = 0;
}

int RT_LIGHT_ReadNames(const char *path, char (*names)[MAX_QPATH], int max)
{
    FILE *f = fopen(path, "r");
    char  line[1024];
    int   count = 0;

    if (!f)
    {
        return 0;
    }

    while (count < max && fgets(line, sizeof(line), f))
    {
        char *p = line;
        char  name[MAX_QPATH];
        int   i, dup = 0;

        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p == '#' || !strncmp(p, "lights:", 7))
        {
            continue;
        }
        if (strncmp(p, "- name:", 7) != 0 && strncmp(p, "name:", 5) != 0)
        {
            continue;
        }

        p = strchr(p, ':') + 1;
        q_strlcpy(name, p, sizeof(name));
        rt_light_trim(name, sizeof(name));
        q_strlwr(name);
        if (!name[0])
        {
            continue;
        }

        for (i = 0; i < count; i++)
        {
            if (!strcmp(names[i], name))
            {
                dup = 1;
            }
        }
        if (!dup)
        {
            q_strlcpy(names[count++], name, MAX_QPATH);
        }
    }

    fclose(f);
    return count;
}

qboolean RT_LIGHT_Write(const char *path, char (*names)[MAX_QPATH], int count)
{
    FILE *f;
    int   i, written = 0;

    if (!path || !names || count <= 0)
    {
        return false;
    }

    f = fopen(path, "w");
    if (!f)
    {
        Con_DWarning("RT light: cannot write %s\n", path);
        return false;
    }

    fprintf(f, "%s", rt_light_header);
    fprintf(f, "lights:\n");

    for (i = 0; i < count; i++)
    {
        rt_light_t *light = rt_light_find_in(names[i]);

        if (RT_LIGHT_HasFields(light))
        {
            RT_LIGHT_WriteEntry(f, light);
            written++;
        }
    }

    if (written == 0)
    {
        fclose(f);
        remove(path); // an empty file is no file
        return false;
    }

    if (ferror(f) || fflush(f) != 0)
    {
        Con_DWarning("RT light: write error in %s\n", path);
        fclose(f);
        remove(path);
        return false;
    }

    fclose(f);
    Con_Printf("qr editor: wrote %d lights to %s\n", written, path);
    return true;
}

// ---------------------------------------------------------------------------
// Custom dlights: freely placed lights the editor authors, one section per level
// in <gamedir>/qray/lights.yaml. Only the current level's section is loaded.
// ---------------------------------------------------------------------------

const char *const rt_custom_style_names[RT_CUSTOM_STYLE_COUNT] = {
    "steady", "flicker", "slow strong pulse", "candle", "fast strobe", "gentle pulse",
    "flicker B", "candle B", "candle C", "slow strobe", "fluorescent", "slow pulse",
};

static rt_custom_light_t rt_custom_lights[RT_CUSTOM_LIGHTS_MAX];
static int               rt_custom_light_count;

// The fog of the loaded level section, and whether the file's fog has been
// pushed into the engine yet (it waits for the worldspawn keys, see
// RT_CustomLights_ApplyFog, and goes in once per map load).
static rt_custom_fog_t rt_custom_fog;
static qboolean        rt_custom_fog_applied;

void RT_CustomFog(rt_custom_fog_t *out)
{
    if (out)
        *out = rt_custom_fog;
}

void RT_CustomFogSet(const rt_custom_fog_t *fog)
{
    if (fog)
        rt_custom_fog = *fog;
    else
        memset(&rt_custom_fog, 0, sizeof(rt_custom_fog));
}

void RT_CustomLights_ApplyFog(void)
{
    if (!rt_custom_fog.has_fog || rt_custom_fog_applied)
        return;

    rt_custom_fog_applied = true;

    /* A section that states "enabled" owns rt_level_fog, the runtime switch of
       the fog drawing: the level remembers whether its fog is shown. A section
       without the key leaves the cvar as the user configured it. */
    if (rt_custom_fog.has_enabled)
        Cvar_Set("rt_level_fog", rt_custom_fog.enabled ? "1" : "0");

    /* The `fog` command is the path the console and the editor's fog widget
       use; it runs on the next command-buffer pump, after the worldspawn keys
       have been parsed, so the file's fog wins over the map's own. */
    Cbuf_AddText(va("fog %f %f %f %f\n", rt_custom_fog.density,
                    CLAMP(0.0f, rt_custom_fog.color[0], 1.0f),
                    CLAMP(0.0f, rt_custom_fog.color[1], 1.0f),
                    CLAMP(0.0f, rt_custom_fog.color[2], 1.0f)));
}

rt_custom_light_t *RT_CustomLights(int *outCount)
{
    if (outCount)
        *outCount = rt_custom_light_count;
    return rt_custom_lights;
}

void RT_CustomLights_SetCount(int count)
{
    rt_custom_light_count = CLAMP(0, count, RT_CUSTOM_LIGHTS_MAX);
}

rt_custom_light_t *RT_CustomLights_Ensure(void)
{
    rt_custom_light_t *l;

    if (rt_custom_light_count >= RT_CUSTOM_LIGHTS_MAX)
        return NULL;

    l = &rt_custom_lights[rt_custom_light_count++];
    memset(l, 0, sizeof(*l));
    l->radius = RT_CUSTOM_RADIUS_DEFAULT;
    l->intensity = 1.0f;
    l->color[0] = l->color[1] = l->color[2] = 1.0f;
    return l;
}

void RT_CustomLights_Remove(int index)
{
    int i;

    if (index < 0 || index >= rt_custom_light_count)
        return;

    for (i = index; i + 1 < rt_custom_light_count; i++)
        rt_custom_lights[i] = rt_custom_lights[i + 1];

    rt_custom_light_count--;
}

// "maps/start.bsp" and "start" both name the level "start".
void RT_CustomLights_LevelKey(const char *mapname, char *out, size_t outsize)
{
    const char *base = mapname, *dot, *p;
    size_t      len;

    if (!mapname)
        mapname = "";

    for (p = mapname; *p; p++)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= outsize)
        len = outsize - 1;

    memcpy(out, base, len);
    out[len] = 0;
    q_strlwr(out);
}

static int RT_CustomStyleFromString(const char *s)
{
    int i;

    for (i = 0; i < RT_CUSTOM_STYLE_COUNT; i++)
    {
        if (!q_strcasecmp(s, rt_custom_style_names[i]))
            return i;
    }
    return CLAMP(0, atoi(s), RT_CUSTOM_STYLE_COUNT - 1);
}

static qboolean RT_CustomColorFromString(const char *s, vec3_t out)
{
    unsigned r, g, b;

    while (*s == ' ' || *s == '#' || *s == '\t')
        s++;

    if (strlen(s) != 6 || sscanf(s, "%2x%2x%2x", &r, &g, &b) != 3)
        return false;

    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    return true;
}

// One light of a "lights:" sequence (and of the old plain-sequence form, where
// the level key maps straight to the lights). False when the list is full, so
// the caller stops reading the section.
static qboolean RT_CustomParseLight(yaml_document_t *document, yaml_node_t *node, const char *level)
{
    rt_custom_light_t *l;
    yaml_node_pair_t  *field;

    if (!node || node->type != YAML_MAPPING_NODE)
        return true;

    l = RT_CustomLights_Ensure();
    if (!l)
    {
        Con_DWarning("RT custom light: '%s' lists more than %d lights; the rest of the section is ignored\n",
                     level, RT_CUSTOM_LIGHTS_MAX);
        return false;
    }

    for (field = node->data.mapping.pairs.start; field < node->data.mapping.pairs.top; field++)
    {
        yaml_node_t *fk = yaml_document_get_node(document, field->key);
        yaml_node_t *fv = yaml_document_get_node(document, field->value);
        char         fkb[64], fvb[256];

        if (!fk || !fv || fk->type != YAML_SCALAR_NODE || fv->type != YAML_SCALAR_NODE)
            continue;
        if (fk->data.scalar.length >= sizeof(fkb) || fv->data.scalar.length >= sizeof(fvb))
            continue;

        memcpy(fkb, fk->data.scalar.value, fk->data.scalar.length);
        fkb[fk->data.scalar.length] = 0;
        memcpy(fvb, fv->data.scalar.value, fv->data.scalar.length);
        fvb[fv->data.scalar.length] = 0;

        if (!q_strcasecmp(fkb, "origin"))
            sscanf(fvb, "%f %f %f", &l->origin[0], &l->origin[1], &l->origin[2]);
        else if (!q_strcasecmp(fkb, "offset"))
        {
            if (sscanf(fvb, "%f %f %f", &l->offset[0], &l->offset[1], &l->offset[2]) == 3)
                l->has_offset = true;
        }
        else if (!q_strcasecmp(fkb, "radius"))
            l->radius = (float)atof(fvb);
        else if (!q_strcasecmp(fkb, "intensity"))
            l->intensity = (float)atof(fvb);
        else if (!q_strcasecmp(fkb, "color"))
            RT_CustomColorFromString(fvb, l->color);
        else if (!q_strcasecmp(fkb, "style"))
            l->style = RT_CustomStyleFromString(fvb);
    }

    return true;
}

static void RT_CustomParseLights(yaml_document_t *document, yaml_node_t *node, const char *level)
{
    yaml_node_item_t *item;

    if (!node || node->type != YAML_SEQUENCE_NODE)
        return;

    for (item = node->data.sequence.items.start; item < node->data.sequence.items.top; item++)
    {
        if (!RT_CustomParseLight(document, yaml_document_get_node(document, *item), level))
            break;
    }
}

// "true"/"false" (any case) and 1/0: the forms the file's booleans take. False
// when the text is neither, so the caller can leave the value unset.
static qboolean RT_CustomBoolFromString(const char *s, qboolean *out)
{
    while (*s == ' ' || *s == '\t')
        s++;

    if (!q_strcasecmp(s, "true") || !strcmp(s, "1"))
    {
        *out = true;
        return true;
    }
    if (!q_strcasecmp(s, "false") || !strcmp(s, "0"))
    {
        *out = false;
        return true;
    }
    return false;
}

// The "fog:" block of a level section: colour ("rrggbb"), density (>= 0) and
// the optional "enabled" (true/false, 1/0).
static void RT_CustomParseFog(yaml_document_t *document, yaml_node_t *node)
{
    rt_custom_fog_t  fog;
    yaml_node_pair_t *field;

    if (!node || node->type != YAML_MAPPING_NODE)
        return;

    memset(&fog, 0, sizeof(fog));
    fog.has_fog = true;
    fog.color[0] = fog.color[1] = fog.color[2] = 1.0f;

    for (field = node->data.mapping.pairs.start; field < node->data.mapping.pairs.top; field++)
    {
        yaml_node_t *fk = yaml_document_get_node(document, field->key);
        yaml_node_t *fv = yaml_document_get_node(document, field->value);
        char         fkb[64], fvb[256];

        if (!fk || !fv || fk->type != YAML_SCALAR_NODE || fv->type != YAML_SCALAR_NODE)
            continue;
        if (fk->data.scalar.length >= sizeof(fkb) || fv->data.scalar.length >= sizeof(fvb))
            continue;

        memcpy(fkb, fk->data.scalar.value, fk->data.scalar.length);
        fkb[fk->data.scalar.length] = 0;
        memcpy(fvb, fv->data.scalar.value, fv->data.scalar.length);
        fvb[fv->data.scalar.length] = 0;

        if (!q_strcasecmp(fkb, "color"))
            RT_CustomColorFromString(fvb, fog.color);
        else if (!q_strcasecmp(fkb, "density"))
            fog.density = q_max(0.0f, (float)atof(fvb));
        else if (!q_strcasecmp(fkb, "enabled"))
        {
            qboolean enabled;

            if (RT_CustomBoolFromString(fvb, &enabled))
            {
                fog.has_enabled = true;
                fog.enabled = enabled;
            }
        }
    }

    RT_CustomFogSet(&fog);
}

// A level section is either a mapping of "fog:" and "lights:" (the form the
// editor writes) or, from the older files, a plain sequence of lights.
static void RT_CustomLightsParse(const char *filebuf, int len, const char *level)
{
    yaml_parser_t   parser;
    yaml_document_t document;
    yaml_node_t    *root;
    yaml_node_pair_t *pair;

    if (!filebuf || len <= 0 || !yaml_parser_initialize(&parser))
        return;

    yaml_parser_set_input_string(&parser, (const unsigned char *)filebuf, (size_t)len);

    if (yaml_parser_load(&parser, &document))
    {
        root = yaml_document_get_root_node(&document);

        for (pair = (root && root->type == YAML_MAPPING_NODE) ? root->data.mapping.pairs.start : NULL;
             pair && pair < root->data.mapping.pairs.top; pair++)
        {
            yaml_node_t *k = yaml_document_get_node(&document, pair->key);
            yaml_node_t *v = yaml_document_get_node(&document, pair->value);
            char         key[128];

            if (!k || !v || k->type != YAML_SCALAR_NODE)
                continue;
            if (k->data.scalar.length >= sizeof(key))
                continue;

            memcpy(key, k->data.scalar.value, k->data.scalar.length);
            key[k->data.scalar.length] = 0;
            if (q_strcasecmp(key, level))
                continue;

            if (v->type == YAML_SEQUENCE_NODE)
            {
                RT_CustomParseLights(&document, v, level);
            }
            else if (v->type == YAML_MAPPING_NODE)
            {
                yaml_node_pair_t *field;

                for (field = v->data.mapping.pairs.start; field < v->data.mapping.pairs.top; field++)
                {
                    yaml_node_t *fk = yaml_document_get_node(&document, field->key);
                    yaml_node_t *fv = yaml_document_get_node(&document, field->value);
                    char         fkb[64];

                    if (!fk || !fv || fk->type != YAML_SCALAR_NODE)
                        continue;
                    if (fk->data.scalar.length >= sizeof(fkb))
                        continue;

                    memcpy(fkb, fk->data.scalar.value, fk->data.scalar.length);
                    fkb[fk->data.scalar.length] = 0;

                    if (!q_strcasecmp(fkb, "fog"))
                        RT_CustomParseFog(&document, fv);
                    else if (!q_strcasecmp(fkb, "lights"))
                        RT_CustomParseLights(&document, fv, level);
                }
            }

            break;
        }

        yaml_document_delete(&document);
    }

    yaml_parser_delete(&parser);
}

void RT_CustomLights_ChangeMap(const char *mapname)
{
    char  level[64];
    char  path[MAX_OSPATH];
    FILE *f;
    long  size;
    char *text;
    size_t got;

    rt_custom_light_count = 0;
    RT_CustomFogSet(NULL);
    rt_custom_fog_applied = false;

    if (!mapname || !mapname[0] || !com_gamedir[0])
        return;

    RT_CustomLights_LevelKey(mapname, level, sizeof(level));
    q_snprintf(path, sizeof(path), "%s/qray/lights.yaml", com_gamedir);

    f = fopen(path, "rb");
    if (!f)
        return;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 8 * 1024 * 1024)
    {
        fclose(f);
        return;
    }

    text = (char *)Mem_Alloc((size_t)size + 1);
    got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = 0;

    RT_CustomLightsParse(text, (int)got, level);
    Mem_Free(text);

    if (rt_custom_light_count > 0 || rt_custom_fog.has_fog)
        Con_Printf("qr custom lights: %d light(s)%s on '%s'\n",
                   rt_custom_light_count, rt_custom_fog.has_fog ? " and a fog" : "", level);
}

const char *RT_CustomLights_Header(void)
{
    return
        "# Custom dlights and fog authored with the light editor: one section per\n"
        "# level, named after the map (the file name without path or extension).\n"
        "# A section carries an optional fog and an optional list of lights. The fog\n"
        "# block may state whether the level's fog is drawn at all (rt_level_fog);\n"
        "# without \"enabled\" the file leaves the cvar as the user configured it:\n"
        "# start:\n"
        "#   fog:\n"
        "#     enabled: true          # true/false (or 1/0), the level's fog switch\n"
        "#     color: 8899aa          # rrggbb\n"
        "#     density: 1.5           # 0 turns the fog off\n"
        "#   lights:\n"
        "#     - origin: 512 -256 64  # x y z, Quake units\n"
        "#       radius: 0.4          # rt_dlight_radius units, 0..10\n"
        "#       intensity: 1.0       # a multiplier of the colour\n"
        "#       color: ff9900        # rrggbb\n"
        "#       offset: 0 0 16       # optional shift from the origin\n"
        "#       style: candle        # optional, a light style of the engine\n";
}

void RT_CustomLights_WriteEntry(FILE *f, const rt_custom_light_t *l)
{
    fprintf(f, "    - origin: %.6g %.6g %.6g\n", l->origin[0], l->origin[1], l->origin[2]);

    if (l->has_offset)
        fprintf(f, "      offset: %.6g %.6g %.6g\n", l->offset[0], l->offset[1], l->offset[2]);

    fprintf(f, "      radius: %.6g\n", l->radius);

    if (l->intensity != 1.0f)
        fprintf(f, "      intensity: %.6g\n", l->intensity);

    fprintf(f, "      color: %02x%02x%02x\n",
            (int)(CLAMP(0.0f, l->color[0], 1.0f) * 255.0f + 0.5f) & 0xff,
            (int)(CLAMP(0.0f, l->color[1], 1.0f) * 255.0f + 0.5f) & 0xff,
            (int)(CLAMP(0.0f, l->color[2], 1.0f) * 255.0f + 0.5f) & 0xff);

    if (l->style > 0)
        fprintf(f, "      style: %s\n", rt_custom_style_names[CLAMP(0, l->style, RT_CUSTOM_STYLE_COUNT - 1)]);
}
