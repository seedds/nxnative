using System;
using System.Buffers.Binary;
using System.Linq;
using System.Text;
using System.IO;
using System.Reflection;
using Path = System.IO.Path;
using LibHac.Common;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.RomFs;

class Pfs0StorageTests {
    static void Check(bool ok) { if (!ok) throw new Exception("PFS0 storage check failed"); }
    static byte[] Package(bool terminated) {
        byte[] names = Encoding.ASCII.GetBytes("first.bin\0last.bin" + (terminated ? "\0" : ""));
        byte[] result = new byte[16 + 48 + names.Length + 6];
        "PFS0"u8.CopyTo(result); BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(4), 2);
        BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(8), (uint)names.Length);
        BinaryPrimitives.WriteUInt64LittleEndian(result.AsSpan(24), 3);
        BinaryPrimitives.WriteUInt64LittleEndian(result.AsSpan(40), 3);
        BinaryPrimitives.WriteUInt64LittleEndian(result.AsSpan(48), 3);
        BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(56), 10);
        names.CopyTo(result, 64); new byte[] {1, 2, 3, 4, 5, 6}.CopyTo(result, 64 + names.Length);
        return result;
    }
    static void ReadPackage(bool terminated) {
        byte[] bytes = Package(terminated), before = (byte[])bytes.Clone();
        using var original = new MemoryStorage(bytes);
        using var storage = P.PackageStorage(original);
        Check(ReferenceEquals(original, storage) == terminated);
        using var fs = new PartitionFileSystem(); fs.Initialize(storage).ThrowIfFailure();
        Check(fs.EnumerateEntries("*", SearchOptions.Default).Select(e => e.Name).SequenceEqual(new[] {"first.bin", "last.bin"}));
        foreach (var pair in new[] {("/first.bin", new byte[] {1, 2, 3}), ("/last.bin", new byte[] {4, 5, 6})}) {
            using var file = new UniqueRef<IFile>(); fs.OpenFile(ref file.Ref, pair.Item1.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            byte[] data = new byte[3]; file.Get.Read(out long read, 0, data, ReadOption.None).ThrowIfFailure();
            Check(read == 3 && data.SequenceEqual(pair.Item2));
        }
        Check(bytes.SequenceEqual(before));
        storage.GetSize(out long size).ThrowIfFailure();
        byte[] all = new byte[(int)size]; storage.Read(0, all).ThrowIfFailure();
        Check(all.AsSpan(all.Length - 6).SequenceEqual(new byte[] {1, 2, 3, 4, 5, 6}));
    }
    static byte[] LiteralColonRomFs(byte[] payload) {
        // Minimal Switch RomFS with root "", child directory "D:", and file "/D:/sample" (1 hash bucket each).
        byte[] rom = new byte[0x200 + payload.Length];
        int dh = 0x50, dm = 0x60, fh = 0xA0, fm = 0xB0, fd = 0x200;
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x00), 0x50);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x08), dh);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x10), 4);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x18), dm);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x20), 52);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x28), fh);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x30), 4);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x38), fm);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x40), 40);
        BinaryPrimitives.WriteInt64LittleEndian(rom.AsSpan(0x48), fd);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dh), 0);
        // Dir 0 (root): parent=0, sibling=none, firstDir=24, firstFile=none, hashSibling=24, nameLen=0
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 0), 0);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 4), 0xFFFFFFFFu);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 8), 24);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 12), 0xFFFFFFFFu);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 16), 24);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 20), 0);
        // Dir 1 ("D:" at offset 24): parent=0, sibling=none, firstDir=none, firstFile=0, hashSibling=none, nameLen=2
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 24), 0);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 28), 0xFFFFFFFFu);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 32), 0xFFFFFFFFu);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 36), 0);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 40), 0xFFFFFFFFu);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(dm + 44), 2);
        Encoding.UTF8.GetBytes("D:").CopyTo(rom, dm + 48);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(fh), 0);
        // File 0 ("sample" in "D:"): parent=24, sibling=none, off=0, size=payload.Length, hashSibling=none, nameLen=6
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(fm + 0), 24);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(fm + 4), 0xFFFFFFFFu);
        BinaryPrimitives.WriteUInt64LittleEndian(rom.AsSpan(fm + 8), 0);
        BinaryPrimitives.WriteUInt64LittleEndian(rom.AsSpan(fm + 16), (ulong)payload.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(fm + 24), 0xFFFFFFFFu);
        BinaryPrimitives.WriteUInt32LittleEndian(rom.AsSpan(fm + 28), 6);
        Encoding.UTF8.GetBytes("sample").CopyTo(rom, fm + 32);
        payload.CopyTo(rom, fd);
        return rom;
    }
    static void CheckLiteralRomFsModes(string tmp) {
        byte[] payload = Encoding.UTF8.GetBytes("hello-colon");
        byte[] rom = LiteralColonRomFs(payload);
        Func<IFileSystem> open = () => new RomFsFileSystem(new MemoryStorage(rom));
        string img = Path.Combine(tmp, "RomFS.img");
        P.WriteImage(open, img);
        Check(File.Exists(img) && File.ReadAllBytes(img).AsSpan().IndexOf(payload) >= 0);
        string folder = Path.Combine(tmp, "RomFS");
        P.Copy(open, folder, null);
        Check(File.ReadAllBytes(Path.Combine(folder, "D:", "sample")).SequenceEqual(payload));
        string manifest = Path.Combine(tmp, "romfs_manifest.tsv");
        P.Manifest(open, manifest);
        string expectedHex = Convert.ToHexString(payload).ToLowerInvariant();
        Check(File.ReadAllText(manifest).Contains($"/D:/sample\t{payload.Length}\t{expectedHex}"));
    }
    static void Main() {
        ReadPackage(true); ReadPackage(false);
        byte[] empty = new byte[16]; "PFS0"u8.CopyTo(empty);
        using (var storage = new MemoryStorage(empty)) Check(ReferenceEquals(storage, P.PackageStorage(storage)));
        foreach (var bytes in new[] {Package(false)[..20], Package(false)}) {
            if (bytes.Length > 20) BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(56), uint.MaxValue);
            using var storage = new MemoryStorage(bytes);
            try { P.PackageStorage(storage); throw new Exception("invalid header accepted"); }
            catch (InvalidDataException) { }
        }
        foreach (bool terminated in new[] {true, false}) {
            byte[] shortPayload = Package(terminated)[..^1];
            using var shortStorage = new MemoryStorage(shortPayload);
            try { P.PackageStorage(shortStorage); throw new Exception("truncated PFS0 payload accepted"); }
            catch (InvalidDataException e) { Check(e.Message.Contains("entry 1")); }
            foreach (var range in new[] {(7UL, 0UL), (5UL, 2UL), (ulong.MaxValue, 1UL), (1UL, ulong.MaxValue)}) {
                byte[] bad = Package(terminated);
                BinaryPrimitives.WriteUInt64LittleEndian(bad.AsSpan(40), range.Item1);
                BinaryPrimitives.WriteUInt64LittleEndian(bad.AsSpan(48), range.Item2);
                using var storage = new MemoryStorage(bad);
                try { P.PackageStorage(storage); throw new Exception("out-of-bounds PFS0 entry accepted"); }
                catch (InvalidDataException) { }
            }
            byte[] edge = Package(terminated);
            BinaryPrimitives.WriteUInt64LittleEndian(edge.AsSpan(40), 6);
            BinaryPrimitives.WriteUInt64LittleEndian(edge.AsSpan(48), 0);
            using var edgeStorage = new MemoryStorage(edge);
            using var accepted = P.PackageStorage(edgeStorage); // empty file exactly at EOF is valid
        }
        // Exercise the production CLI entry, not just its parser, without game files or keys.
        string tmp = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString()); Directory.CreateDirectory(tmp);
        TextWriter saved = Console.Out;
        try {
            string keys = Path.Combine(tmp, "empty.keys"), nsp = Path.Combine(tmp, "truncated.nsp");
            File.WriteAllText(keys, "");
            foreach (byte[] bad in new[] {Package(true)[..^1], Package(false)[..^1], "PFS0"u8.ToArray()}) {
                File.WriteAllBytes(nsp, bad);
                foreach (bool allow in new[] {false, true}) {
                    string[] args = new[] {keys, "--aoc", nsp, Path.Combine(tmp, "out")};
                    if (allow) args = args.Append("--allow-damaged").ToArray();
                    using var log = new StringWriter(); Console.SetOut(log);
                    int rc = (int)typeof(P).GetMethod("Main", BindingFlags.NonPublic | BindingFlags.Static).Invoke(null, new object[] {args});
                    Console.SetOut(saved);
                    Check(rc == 3 && log.ToString().Contains("[RED] the NSP is damaged") && log.ToString().Contains("truncated.nsp"));
                    Check(!Directory.Exists(Path.Combine(tmp, "out")));
                }
            }
            CheckLiteralRomFsModes(tmp);
        } finally { Console.SetOut(saved); Directory.Delete(tmp, true); }
        Console.WriteLine("PFS0 storage checks passed");
    }
}
