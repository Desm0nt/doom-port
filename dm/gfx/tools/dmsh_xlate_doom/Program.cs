// [DOOM] DOOM (2016, Switch) build of d2r-mobile's shader translator dmsh_xlate: original Maxwell shaders -> SPIR-V (Ryujinx.Graphics.Shader, Vulkan target),
// for src/dm/gfx/tools/dmsh_vk_doom.py, which packs the result into the xbg engine's XGS1 bundles. Derived from src/dm/gfx/tools/dmsh_xlate/Program.cs (D2R; unchanged, so the D2R
// chain keeps its version); built by tools/build_dmsh_xlate_doom.py.
// Portions ported from nxnative (https://github.com/seedds/nxnative) dm/gfx/tools/dmsh_xlate/Program.cs, Copyright (c) 2026 seedds, MIT License:
//   Accessor.Log (translator messages into the notes), Slot (c2 handle -> texture / image slot), the image-type re-translation (ImageQueryGuessed / ImageTypesOf),
//   UsesFloat64 + the softfloat64 retry.
//
// Differences from the D2R build, each one a query answered from facts instead of a default:
//   * NVN c2 handle layout (one layout for every glslc; nxnative dm/gfx/tools/dmsh_build.py): combined textures 0x20 + 8 r, images 0x120 + 8 r (8 slots), separate textures
//     0x168 + 8 r (slot 128 + r), separate samplers 0x568 + 8 r. D2R's build reads the 0x168 table as its texture table. DOOM's facts ("tex" / "img") are keyed by the GLSL
//     binding r of the combined texture / image, verified per stage by dmsh_vk_doom.py check_slots.
//   * QueryHostSupportsGeometryShaderPassthrough / QueryHostSupportsViewportMask = false: DOOM's GPU-culling geometry shaders are NV passthrough shaders writing gl_ViewportMask;
//     no SPV_NV_* extension is allowed by the engine (and Adreno refuses the PassthroughNV capability), so Ryujinx re-emits the triangle and drops the viewport-mask write.
//   * Stage linking from the real program pairs (job "next"): DOOM's .sb programs name every stage of a program, so a vertex shader knows which inputs its geometry / pixel
//     partners read, and a passthrough geometry shader which attributes it must pass through. The translator is given one "next stage" whose input usage is the union of all
//     partners of the key (TranslatorContext.SetNextStage, as Ryujinx's own runtime links stages). Without it, (a) the emulated passthrough geometry shader passed NO generic
//     attribute through (Ryujinx computes the passthrough mask from the next stage only), so the pixel shaders of DOOM's vsgsps programs read undefined varyings
//     (23 of 717 pairs), and (b) a component the pixel shader reads but the vertex shader never writes was undefined instead of Maxwell's (0, 0, 0, 1) (6 pairs).
//   * FP64: Adreno 7xx has no shaderFloat64; a shader whose SPIR-V declares a 64-bit float is translated again with QueryHostSupportsShaderFloat64 = false (Ryujinx's DoubleToFloat).
//   * Window origin upper-left, depth 0..1 (DOOM: nvnDeviceSetWindowOriginMode / SetDepthMode measured on the D2R side, same convention on the xbg engine).
// Usage: dmsh_xlate <job.json> <output dir>
//   Job: [{ "key", "stage": "vs|ps|gs|cs", "code": machine code file (graphics = 0x50-byte SPH + instructions, compute = instructions), "c1": hex,
//           "tex": { "slot": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", ... }, "img": { "slot": { "type", "fmt" } },
//           "attr": { "location": "float|sint|uint" }, "fout": { "target": "float|sint|uint" }, "local": [x, y, z], "shared": bytes,
//           "next": [{ "key", "code", "c1" }, ...] (the next-stage partners of this key in every program that uses it) }]
//   Output: <key>.spv and <key>.res.json; res "notes" lists queries answered by default and, marked "translator: ", the translator's own messages (e.g. a bindless read it
//   could not resolve and removed), "linked" the next-stage input attributes the translation was linked against.
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlate
{
    // NVN's handle tables in c[2], byte offsets, 8 bytes per slot (nxnative: one layout in every game)
    const int TexBase = 0x20, ImgBase = 0x120, ImgSlots = 8, SepTexBase = 0x168, SepTexSlot0 = 128, SampBase = 0x568;

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job, Dictionary<int, SamplerType> imageTypes = null) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); _imageTypes = imageTypes; }
        // A texture query on an image handle asks the type of that handle; when the facts do not answer it, the shader's own surface instructions do: the first translation records
        // that a query went unanswered (ImageQueryGuessed), and Main translates again with the types of the images it found, by c2 handle (ImageTypesOf) [nxnative]
        readonly Dictionary<int, SamplerType> _imageTypes; public bool ImageQueryGuessed;
        public static Dictionary<int, SamplerType> ImageTypesOf(ShaderProgram p)
        {
            var types = new Dictionary<int, SamplerType>();
            foreach (var im in p.Info.Images) if (im.CbufSlot == 2 || im.CbufSlot < 0) types[im.HandleIndex] = im.Type & (SamplerType.Mask | SamplerType.Array);
            return types;
        }
        // c1 constant segment (the bytes from control +0x700 to the end of the code area): the jump tables of indirect jumps (BRX, LDC c[1]) live here
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 read out of bounds +{offset:X} (c1 length {_c1.Length})"); return 0; }
        // The translator's own messages (a bindless read it could not resolve and removed, an invalid encoding, ...) go into the notes, marked [nxnative]
        public void Log(string message) => Notes.Add("translator: " + message);
        // All resources in set 0, each with its own binding number (dmsh_vk.py rewrite() later makes them binding * 6 + stage)
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        // A handle's texture or image slot [nxnative]: the handle ORs two c[2] words in no fixed order, so both are tried (only the first for a combined handle or a constant
        // sampler); -1 when neither is in the table
        static int Slot(int handle, int cbufSlot, bool image)
        {
            if (cbufSlot >= 0 && (cbufSlot & 0xffff) != 2) return -1;
            (int w0, int w1, TextureHandleType ty) = TextureHandle.UnpackOffsets(handle);
            foreach (int w in ty == TextureHandleType.CombinedSampler || ty == TextureHandleType.SeparateConstantSamplerHandle ? new[] { w0 } : new[] { w0, w1 })
            {
                int b = w * 4;
                if (b % 8 != 0) continue;
                if (image && b >= ImgBase && b < ImgBase + 8 * ImgSlots) return (b - ImgBase) / 8;
                if (!image && b >= TexBase && b < SepTexBase) return (b - TexBase) / 8;
                if (!image && b >= SepTexBase && b < SampBase) return SepTexSlot0 + (b - SepTexBase) / 8;
            }
            return -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, false), islot = Slot(handle, cbufSlot, true);
            string t = (slot >= 0 ? Get("tex", slot) : null) ?? (islot >= 0 ? Get("img", islot) : null);
            if (t == null && islot >= 0 && cbufSlot < 0)
            {
                if (_imageTypes != null && _imageTypes.TryGetValue(handle, out SamplerType it)) { Notes.Add($"texture handle {handle:X} is image slot {islot}: type {it} from the shader's image instructions"); return it; }
                ImageQueryGuessed = true;
            }
            if (t == null) { Notes.Add($"texture handle {handle:X} (constant buffer {cbufSlot}, slot {slot}) has no type in the facts, using 2D"); return SamplerType.Texture2D; }
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, true);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"image handle {handle:X} slot {slot} has no format in the facts, using R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // control +0x754 (tools/doom_shader_jobs.py, cross-checked with the GLSL)
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            return new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, false /* depth 0..1 */, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* no Y flip */, true /* origin upper-left */, false);
        }
        public bool QueryHostSupportsTextureShadowLod() => true;
        public bool QueryHostSupportsGeometryShaderPassthrough() => false;   // no SPV_NV_geometry_shader_passthrough (see the file header)
        public bool QueryHostSupportsViewportMask() => false;                // no SPV_NV_viewport_array2
        // Job reduced = true: the translator marks NoContraction only on operations that must be exact
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // Job softfloat64 = true: lower double operations to float (Ryujinx DoubleToFloat). Set only on the retry of a shader whose first translation declared a 64-bit float [nxnative]
        public bool QueryHostSupportsShaderFloat64() => !(_job["softfloat64"] is JsonNode n && (bool)n);
    }

    // True when the SPIR-V declares a 64-bit float type (OpTypeFloat 64) [nxnative]
    static bool UsesFloat64(byte[] spirv)
    {
        for (int i = 20; i + 12 <= spirv.Length; )
        {
            uint word = BitConverter.ToUInt32(spirv, i);
            if ((word & 0xffff) == 22 && (word >> 16) == 3 && BitConverter.ToUInt32(spirv, i + 8) == 64) return true;
            i += Math.Max(1, (int)(word >> 16)) * 4;
        }
        return false;
    }

    // ---- stage linking (job "next") ----
    // Ryujinx keeps a stage's attribute usage in the internal TranslatorContext.AttributeUsage; the public SetNextStage takes it from one context. The union over every partner
    // is built on the first partner's context through reflection (AttributeUsage.SetInputUserAttribute is what the decoder itself calls for each input component read).
    static readonly PropertyInfo AttributeUsageOf = typeof(TranslatorContext).GetProperty("AttributeUsage", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public)
        ?? throw new MissingMemberException("TranslatorContext.AttributeUsage (Ryujinx changed: update dmsh_xlate_doom Program.cs)");
    static UInt128 InputComponents(TranslatorContext ctx)
    {
        object au = AttributeUsageOf.GetValue(ctx);
        return (UInt128)(au.GetType().GetProperty("ThisInputAttributesComponents") ?? throw new MissingMemberException("AttributeUsage.ThisInputAttributesComponents")).GetValue(au);
    }
    static void AddInputComponents(TranslatorContext ctx, UInt128 comps)
    {
        object au = AttributeUsageOf.GetValue(ctx);
        MethodInfo set = au.GetType().GetMethod("SetInputUserAttribute") ?? throw new MissingMemberException("AttributeUsage.SetInputUserAttribute");
        for (int i = 0; i < 128; i++) if (((comps >> i) & UInt128.One) != UInt128.Zero) set.Invoke(au, new object[] { i / 4, i % 4 });
    }
    static string Mask(UInt128 comps)   // "location:components ..." e.g. "0:xyzw 3:xy"
    {
        var parts = new List<string>();
        for (int l = 0; l < 32; l++)
        {
            int m = (int)((comps >> (4 * l)) & 0xf), c;
            if (m == 0) continue;
            string s = "";
            for (c = 0; c < 4; c++) if ((m & (1 << c)) != 0) s += "xyzw"[c];
            parts.Add($"{l}:{s}");
        }
        return string.Join(" ", parts);
    }
    // -> the context to pass to SetNextStage (null without partners), and the union of the partners' input components
    static (TranslatorContext, UInt128) NextStage(JsonObject job, TranslationFlags flags)
    {
        if (job["next"] is not JsonArray next || next.Count == 0) return (null, UInt128.Zero);
        TranslatorContext carrier = null; UInt128 all = UInt128.Zero;
        foreach (JsonObject n in next)
        {
            byte[] code = File.ReadAllBytes((string)n["code"]);
            var ctx = Translator.CreateContext(0, new Accessor(code, n), new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags & TranslationFlags.DebugMode));
            UInt128 c = InputComponents(ctx); all |= c;
            carrier ??= ctx;
        }
        AddInputComponents(carrier, all);
        return (carrier, all);
    }

    static (ShaderProgram, UInt128?) Translate(byte[] code, JsonObject job, Accessor acc, TranslationFlags flags)
    {
        var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
        (TranslatorContext next, UInt128 linked) = NextStage(job, flags);
        if (next != null) ctx.SetNextStage(next);
        return (ctx.Translate(), next != null ? linked : null);
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--source-sha")   // first 16 hex digits of the sha256 of the embedded Program.cs + csproj (dmsh_vk_doom.py records it in the chain manifest)
        {
            var asm = typeof(SwshXlate).Assembly; var ms = new MemoryStream();
            foreach (var n in new[] { "src0", "src1" }) asm.GetManifestResourceStream(n).CopyTo(ms);
            Console.WriteLine(Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(ms.ToArray()))[..16].ToLowerInvariant());
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string key = (string)job["key"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer; semantics unchanged)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0);
                var acc = new Accessor(code, job);
                (ShaderProgram p, UInt128? linked) = Translate(code, job, acc, flags);
                Dictionary<int, SamplerType> imageTypes = null;
                if (acc.ImageQueryGuessed && (imageTypes = Accessor.ImageTypesOf(p)).Count > 0)   // a query on an image handle went unanswered: again with the images' types
                {
                    acc = new Accessor(code, job, imageTypes);
                    (p, linked) = Translate(code, job, acc, flags);
                }
                if (UsesFloat64(p.BinaryCode))   // no shaderFloat64 on Adreno 7xx
                {
                    job["softfloat64"] = true; acc = new Accessor(code, job, imageTypes);
                    (p, linked) = Translate(code, job, acc, flags); acc.Notes.Add("FP64 lowered to FP32 (DoubleToFloat)");
                }
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), p.BinaryCode);
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                };
                if (linked is UInt128 lk) res["linked"] = Mask(lk);
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add(new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                ok++;
            }
            catch (Exception e) { bad++; Console.Error.WriteLine($"[RED] {key}: {e.GetType().Name}: {e.Message.Split('\n')[0]}"); if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); }
        }
        Console.WriteLine($"translated: {ok} ok, {bad} failed");
        return bad == 0 ? 0 : 1;
    }
}
