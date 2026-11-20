// Locate the play-history signing resource in a privately supplied friends NSO.
// This original decoder contains no firmware bytes, signing material or fixed
// firmware offsets. The caller must first validate the source NCA's title and
// integrity. Unsupported or ambiguous code shapes fail closed.
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;

internal static class FriendsPlayHistoryResources {
    const int HeaderSize = 0x100;
    const int MaximumInput = 64 * 1024 * 1024;
    const int MaximumSegment = 64 * 1024 * 1024;
    const int MaximumImage = 128 * 1024 * 1024;
    readonly record struct Segment(int File, int Address, int Length, int Stored);
    readonly record struct Candidate(int Table, int Hmac);

    static InvalidDataException Invalid() => new("The friends play-history resource could not be verified in this firmware.");
    static uint Word(ReadOnlySpan<byte> data, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(offset, 4));
    static int Bounded(uint value, int limit) { if (value > limit) throw Invalid(); return (int)value; }
    static bool Fits(int offset, int length, int total) => offset >= 0 && length >= 0 && offset <= total && length <= total - offset;
    static bool Overlaps(int a, int n, int b, int m) => n != 0 && m != 0 && (long)a < (long)b + m && (long)b < (long)a + n;

    public static byte[] Extract(byte[] nso) {
        if (nso == null || nso.Length < HeaderSize || nso.Length > MaximumInput ||
            Word(nso, 0) != 0x304f534e || Word(nso, 4) != 0) throw Invalid();
        uint flags = Word(nso, 12);
        if ((flags & ~63u) != 0) throw Invalid();
        var segments = new Segment[3]; int imageSize = 0;
        for (int i = 0; i < 3; i++) {
            int at = 16 + i * 16;
            var s = new Segment(Bounded(Word(nso, at), MaximumInput), Bounded(Word(nso, at + 4), MaximumImage),
                Bounded(Word(nso, at + 8), MaximumSegment), Bounded(Word(nso, 96 + i * 4), MaximumInput));
            if (s.File < HeaderSize || !Fits(s.File, s.Stored, nso.Length) || !Fits(s.Address, s.Length, MaximumImage) ||
                (s.Address & 3) != 0 || ((flags & (1u << i)) == 0 && s.Stored != s.Length)) throw Invalid();
            foreach (var previous in segments.AsSpan(0, i))
                if (Overlaps(s.File, s.Stored, previous.File, previous.Stored) ||
                    Overlaps(s.Address, s.Length, previous.Address, previous.Length)) throw Invalid();
            segments[i] = s; imageSize = Math.Max(imageSize, s.Address + s.Length);
        }
        int bss = Bounded(Word(nso, 60), MaximumSegment);
        if (!Fits(segments[2].Address, segments[2].Length + (long)bss, MaximumImage) ||
            segments[0].Length < 4 || (segments[0].Length & 3) != 0 || segments[1].Length < 256) throw Invalid();
        byte[] image = new byte[imageSize];
        try {
            Span<byte> digest = stackalloc byte[32];
            for (int i = 0; i < 3; i++) {
                var s = segments[i]; var destination = image.AsSpan(s.Address, s.Length);
                var source = nso.AsSpan(s.File, s.Stored);
                if ((flags & (1u << i)) != 0) DecodeLz4(source, destination); else source.CopyTo(destination);
                SHA256.HashData(destination, digest);
                if (!CryptographicOperations.FixedTimeEquals(digest, nso.AsSpan(160 + i * 32, 32))) throw Invalid();
            }
            return Find(image, segments[0], segments[1]);
        } finally { CryptographicOperations.ZeroMemory(image); }
    }

    static bool Fits(int offset, long length, int total) => length >= 0 && length <= int.MaxValue && Fits(offset, (int)length, total);

    // Raw LZ4 blocks as used by NSO segments. Bounds are checked before every
    // literal/match; overlapping matches deliberately copy forward.
    static void DecodeLz4(ReadOnlySpan<byte> source, Span<byte> output) {
        int input = 0, written = 0;
        while (input < source.Length) {
            byte token = source[input++];
            int literals = Length(source, ref input, token >> 4, output.Length - written);
            if (!Fits(input, literals, source.Length) || !Fits(written, literals, output.Length)) throw Invalid();
            source.Slice(input, literals).CopyTo(output.Slice(written)); input += literals; written += literals;
            if (input == source.Length) { if (written != output.Length) throw Invalid(); return; }
            if (!Fits(input, 2, source.Length)) throw Invalid();
            int distance = BinaryPrimitives.ReadUInt16LittleEndian(source.Slice(input, 2)); input += 2;
            if (distance == 0 || distance > written || output.Length - written < 4) throw Invalid();
            int match = Length(source, ref input, token & 15, output.Length - written - 4) + 4;
            for (int j = 0; j < match; j++) output[written + j] = output[written + j - distance];
            written += match;
        }
        if (written != output.Length) throw Invalid();
    }
    static int Length(ReadOnlySpan<byte> source, ref int cursor, int initial, int maximum) {
        int length = initial;
        if (length > maximum) throw Invalid();
        if (initial == 15) {
            byte extra;
            do {
                if (cursor == source.Length) throw Invalid(); extra = source[cursor++];
                if (extra > maximum - length) throw Invalid(); length += extra;
            } while (extra == 255);
        }
        return length;
    }

    static byte[] Find(byte[] image, Segment text, Segment ro) {
        var groups = new Dictionary<Candidate, List<int>>();
        var publicConstants = new HashSet<ushort>();
        int end = text.Address + text.Length;
        for (int pc = text.Address; pc < end; pc += 4) {
            uint instruction = Word(image, pc);
            // The public SHA-256 IV halves and HMAC pad values corroborate the
            // algorithm; these are not Nintendo signing keys.
            if ((instruction & 0xff800000u) == 0xd2800000u || (instruction & 0xff800000u) == 0xf2800000u ||
                (instruction & 0xff800000u) == 0x52800000u || (instruction & 0xff800000u) == 0x72800000u)
                publicConstants.Add((ushort)(instruction >> 5));
            if (!Adr(instruction, pc, out int table, out int register) ||
                table < ro.Address || !Fits(table - ro.Address, 256, ro.Length)) continue;
            bool indexed = false; int argumentSizes = 0, hmac = -1;
            for (int next = pc + 4; next < end && next <= pc + 18 * 4; next += 4) {
                uint w = Word(image, next);
                // ADD X4, table, index, LSL #5.
                if ((w & 0xffe0fc1fu) == 0x8b001404u && ((w >> 5) & 31) == register) indexed = true;
                if ((w & 0xffe00000u) == 0x52800000u && ((w >> 5) & 0xffff) == 32) {
                    int rd = (int)(w & 31); if (rd == 1) argumentSizes |= 1; if (rd == 3) argumentSizes |= 2; if (rd == 5) argumentSizes |= 4;
                }
                if ((w & 0xfc000000u) == 0x94000000u) {
                    int delta = (int)(w & 0x03ffffff); if ((delta & 0x02000000) != 0) delta -= 0x04000000;
                    long target = (long)next + (long)delta * 4;
                    if (target >= text.Address && target < end) hmac = (int)target;
                    break;
                }
            }
            if (!indexed || argumentSizes != 7 || hmac < 0 || !IndexGuard(image, Math.Max(text.Address, pc - 24 * 4), pc)) continue;
            var key = new Candidate(table, hmac);
            if (!groups.TryGetValue(key, out var uses)) groups[key] = uses = new List<int>();
            uses.Add(pc);
        }
        foreach (ushort constant in new ushort[] { 0x6a09, 0xbb67, 0x3c6e, 0xa54f, 0x510e, 0x9b05, 0x1f83, 0x5be0, 0x3636, 0x6a6a })
            if (!publicConstants.Contains(constant)) throw Invalid();
        Candidate? found = null;
        foreach (var pair in groups) if (pair.Value.Count == 3) {
            if (found != null) throw Invalid(); found = pair.Key;
        }
        if (found == null) throw Invalid();
        var material = image.AsSpan(found.Value.Table, 256);
        for (int i = 0; i < 8; i++) {
            var entry = material.Slice(i * 32, 32); bool nonzero = false;
            foreach (byte value in entry) nonzero |= value != 0;
            if (!nonzero) throw Invalid();
            for (int j = 0; j < i; j++) if (entry.SequenceEqual(material.Slice(j * 32, 32))) throw Invalid();
        }
        return material.ToArray();
    }
    static bool Adr(uint w, int pc, out int target, out int register) {
        target = 0; register = (int)(w & 31);
        if ((w & 0x9f000000u) != 0x10000000u) return false;
        int relative = (int)(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3));
        if ((relative & 0x100000) != 0) relative -= 0x200000;
        long address = (long)pc + relative;
        if (address < 0 || address > int.MaxValue) return false; target = (int)address; return true;
    }
    static bool IndexGuard(byte[] image, int start, int end) {
        for (int pc = start; pc < end; pc += 4) {
            uint w = Word(image, pc);
            // CMP Wn/Xn, #7 (SUBS immediate with destination zero register).
            if ((w & 0x7ffffc1fu) == 0x71001c1fu) return true;
            // AND Wd/Xd, Wn/Xn, #7: immr=0, imms=2; N is set for X.
            if ((w & 0xfffffc00u) == 0x12000800u || (w & 0xfffffc00u) == 0x92400800u) return true;
        }
        return false;
    }
}
