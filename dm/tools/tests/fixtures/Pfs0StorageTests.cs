using System;
using System.Buffers.Binary;
using System.Linq;
using System.Text;
using System.IO;
using LibHac.Common;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;

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
        Console.WriteLine("PFS0 storage checks passed");
    }
}
