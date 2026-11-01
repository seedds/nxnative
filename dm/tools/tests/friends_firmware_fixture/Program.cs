// Entirely synthetic NSO / ARM64 / LZ4 fixtures. No firmware, Nintendo code,
// identities or signing material is stored in this test project.
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;

static class Program {
    static int checks;
    static void Check(bool value, string label) { checks++; if (!value) throw new Exception(label); }
    static void Reject(byte[] input, string label) {
        try { FriendsPlayHistoryResources.Extract(input); }
        catch (InvalidDataException error) { Check(!error.Message.Contains("0x"), label + " has a metadata-only error"); return; }
        throw new Exception("Accepted invalid fixture: " + label);
    }
    static void Put(byte[] b, int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(offset, 4), value);
    static uint Get(byte[] b, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(offset, 4));
    static byte[] FixtureMaterial(int seed = 0) {
        var value = new byte[256];
        for (int i = 0; i < value.Length; i++) value[i] = (byte)(1 + (i + i / 32 * 17 + seed) % 251);
        return value;
    }
    static void Reference(byte[] text, int pc, int table, int hmac = 0x800, bool guard = true) {
        // Public ARM64 encodings for a non-executable metadata fixture.
        Put(text, pc - 4, guard ? 0xf1001d1fu : 0xd503201fu); // CMP X8,#7 or NOP
        uint relative = (uint)(table - pc) & 0x1fffff;
        Put(text, pc, 0x10000009u | ((relative & 3) << 29) | ((relative >> 2) << 5)); // ADR X9,table
        Put(text, pc + 4, 0x8b000004u | (8u << 16) | (5u << 10) | (9u << 5)); // ADD X4,X9,X8,LSL#5
        Put(text, pc + 8, 0x52800001u | (32u << 5));
        Put(text, pc + 12, 0x52800003u | (32u << 5));
        Put(text, pc + 16, 0x52800005u | (32u << 5));
        Put(text, pc + 20, 0x94000000u | ((uint)((hmac - pc - 20) / 4) & 0x03ffffffu));
    }
    static (byte[] Text, byte[] Ro, byte[] Data) Segments(bool ambiguous = false) {
        byte[] text = new byte[0x1000], ro = new byte[0x400], data = new byte[32];
        Array.Fill(data, (byte)65);
        for (int i = 0; i < 3; i++) Reference(text, 0x100 + i * 0x40, 0x2080);
        if (ambiguous) for (int i = 0; i < 3; i++) Reference(text, 0x300 + i * 0x40, 0x2180);
        int at = 0x900;
        foreach (ushort value in new ushort[] { 0x6a09, 0xbb67, 0x3c6e, 0xa54f, 0x510e, 0x9b05, 0x1f83, 0x5be0, 0x3636, 0x6a6a }) {
            Put(text, at, 0xd2800000u | ((uint)value << 5)); at += 4;
        }
        FixtureMaterial().CopyTo(ro, 0x80);
        if (ambiguous) FixtureMaterial(13).CopyTo(ro, 0x180);
        return (text, ro, data);
    }
    static byte[] Literal(byte[] input) {
        var output = new List<byte> { (byte)(Math.Min(input.Length, 15) << 4) };
        if (input.Length >= 15) { int left = input.Length - 15; while (left >= 255) { output.Add(255); left -= 255; } output.Add((byte)left); }
        output.AddRange(input); return output.ToArray();
    }
    static byte[] Pack(byte[] text, byte[] ro, byte[] data, byte[] compressedText = null, byte[] compressedData = null) {
        byte[][] decoded = { text, ro, data }, stored = { compressedText ?? text, ro, compressedData ?? data };
        var output = new byte[0x100 + stored[0].Length + stored[1].Length + stored[2].Length];
        Put(output, 0, 0x304f534e); Put(output, 12, (compressedText != null ? 1u : 0u) | (compressedData != null ? 4u : 0u));
        int file = 0x100;
        for (int i = 0; i < 3; i++) {
            Put(output, 16 + i * 16, (uint)file); Put(output, 20 + i * 16, (uint)(i == 0 ? 0 : i == 1 ? 0x2000 : 0x3000));
            Put(output, 24 + i * 16, (uint)decoded[i].Length); Put(output, 96 + i * 4, (uint)stored[i].Length);
            SHA256.HashData(decoded[i]).CopyTo(output, 160 + i * 32); stored[i].CopyTo(output, file); file += stored[i].Length;
        }
        return output;
    }
    static byte[] Pack((byte[] Text, byte[] Ro, byte[] Data) s) => Pack(s.Text, s.Ro, s.Data);
    static byte[] Mutate(byte[] source, int offset, uint value) { var b = (byte[])source.Clone(); Put(b, offset, value); return b; }

    static void Main() {
        var segments = Segments(); var valid = Pack(segments);
        byte[] before = (byte[])valid.Clone();
        Check(FriendsPlayHistoryResources.Extract(valid).AsSpan().SequenceEqual(FixtureMaterial()), "uncompressed semantic extraction");
        Check(valid.AsSpan().SequenceEqual(before), "input NSO stays unchanged");
        Check(FriendsPlayHistoryResources.Extract(Pack(segments.Text, segments.Ro, segments.Data, Literal(segments.Text)))
            .AsSpan().SequenceEqual(FixtureMaterial()), "LZ4 literal extension");
        // One literal and an overlapping distance-one match produce 32 A's.
        byte[] overlap = { 0x1f, 65, 1, 0, 12, 0 };
        Check(FriendsPlayHistoryResources.Extract(Pack(segments.Text, segments.Ro, segments.Data, null, overlap))
            .AsSpan().SequenceEqual(FixtureMaterial()), "LZ4 overlapping match");
        Reject(null, "null input"); Reject(Array.Empty<byte>(), "empty input");
        foreach (int n in new[] { 4, 0xff, 0x100, valid.Length - 1 }) Reject(valid.AsSpan(0, n).ToArray(), "truncated NSO");
        Reject(Mutate(valid, 0, 0), "wrong magic"); Reject(Mutate(valid, 4, 1), "unsupported NSO version");
        Reject(Mutate(valid, 12, 64), "unknown flags");
        Reject(Mutate(valid, 24, uint.MaxValue), "segment allocation bound");
        Reject(Mutate(valid, 20, 128u * 1024 * 1024), "image bound");
        Reject(Mutate(valid, 60, uint.MaxValue), "BSS allocation bound");
        Reject(Mutate(valid, 16, uint.MaxValue), "file offset bound");
        Reject(Mutate(valid, 16, 0), "segment overlaps header");
        Reject(Mutate(valid, 32, Get(valid, 16)), "file segments overlap");
        Reject(Mutate(valid, 36, 0), "virtual segments overlap");
        Reject(Mutate(valid, 20, 1), "unaligned text");
        for (int i = 0; i < 3; i++) {
            var bad = (byte[])valid.Clone(); bad[160 + 32 * i] ^= 1; Reject(bad, "each segment hash verified");
            bad = (byte[])valid.Clone(); bad[(int)Get(valid, 16 + i * 16)] ^= 1; Reject(bad, "segment corruption");
        }
        foreach (byte[] malformed in new[] {
            new byte[] { 0xf0 }, new byte[] { 0xf0, 255 }, new byte[] { 0x20, 65 },
            new byte[] { 0x10, 65, 0, 0 }, new byte[] { 0x10, 65, 2, 0 },
            new byte[] { 0x1f, 65, 1, 0, 255 }, new byte[] { 0x10, 65, 1 },
            new byte[] { 0x1f, 65, 1, 0, 28 }, new byte[] { 0x10, 65 }
        }) Reject(Pack(segments.Text, segments.Ro, segments.Data, null, malformed), "malformed LZ4");
        Reject(Pack(Segments(true)), "ambiguous resource tables");
        var changed = Segments(); Reference(changed.Text, 0x100, 0x2080, guard: false); Reject(Pack(changed), "index guard missing");
        changed = Segments(); Reference(changed.Text, 0x100, 0x2080, hmac: 0x804); Reject(Pack(changed), "HMAC targets disagree");
        changed = Segments(); Put(changed.Text, 0x10c, 0x52800003u | (31u << 5)); Reject(Pack(changed), "HMAC size mismatch");
        changed = Segments(); Reference(changed.Text, 0x100, 0x23f0); Reject(Pack(changed), "table crosses rodata boundary");
        changed = Segments(); Put(changed.Text, 0x900, 0); Reject(Pack(changed), "SHA-256 algorithm evidence missing");
        changed = Segments(); changed.Ro.AsSpan(0x80, 32).Clear(); Reject(Pack(changed), "empty entry");
        changed = Segments(); changed.Ro.AsSpan(0x80, 32).CopyTo(changed.Ro.AsSpan(0xa0, 32)); Reject(Pack(changed), "duplicate entry");
        Console.WriteLine($"Friends firmware extraction: {checks} synthetic checks passed.");
    }
}
