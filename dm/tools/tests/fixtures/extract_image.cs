// Synthetic RomFS inputs exercise the production WriteImage method, including its publication boundary.
using System;
using System.IO;
using System.Collections.Generic;
using System.Reflection;
using LibHac;
using LibHac.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.RomFs;

static class ExtractImageTests {
    static readonly BindingFlags Flags = BindingFlags.NonPublic | BindingFlags.Static;
    sealed class Probe : IStorage {
        readonly IStorage source; readonly bool fail, crash, raw;
        public Probe(IStorage source, bool fail, bool crash, bool raw) { this.source = source; this.fail = fail; this.crash = crash; this.raw = raw; }
        public override Result Read(long offset, Span<byte> data) {
            if (data.Length < 65536) return source.Read(offset, data);
            if (crash) throw new IOException("read failed");
            if (fail) {
                if (raw) ResultFs.NonRealDataVerificationFailed.Value.ThrowIfFailure();
                return ResultFs.NonRealDataVerificationFailed.Value;
            }
            return source.Read(offset, data);
        }
        public override Result GetSize(out long size) => source.GetSize(out size);
        public override Result Flush() => Result.Success;
        public override Result Write(long offset, ReadOnlySpan<byte> data) => throw new NotSupportedException();
        public override Result SetSize(long size) => throw new NotSupportedException();
        public override Result OperateRange(Span<byte> output, OperationId op, long offset, long size, ReadOnlySpan<byte> input) => throw new NotSupportedException();
    }
    static int Main(string[] args) {
        string mode = args[0], dst = args[1];
        bool raw = mode.StartsWith("raw-"); if (raw) mode = mode.Substring(4);
        bool damaged = mode == "damage" || mode == "allow", allow = mode == "allow", crash = mode == "failure";
        var builder = new RomFsBuilder();
        if (mode != "empty") {
            var bytes = new byte[(12 << 20) + 37];
            for (int i = 0; i < bytes.Length; i++) bytes[i] = (byte)(i * 17 + 3);
            builder.AddFile("/large", new MemoryStorage(bytes).AsFile(OpenMode.Read));
            builder.AddFile("/dir/small", new MemoryStorage(new byte[] { 1, 2, 3 }).AsFile(OpenMode.Read));
            builder.AddFile("/zero", new MemoryStorage(Array.Empty<byte>()).AsFile(OpenMode.Read));
        }
        using var source = builder.Build();
        Func<IntegrityCheckLevel, IStorage> factory = level => new Probe(source, damaged && level == IntegrityCheckLevel.ErrorOnInvalid, crash, raw);
        Func<LibHac.Fs.Fsa.IFileSystem> open = () => new RomFsFileSystem(factory(IntegrityCheckLevel.ErrorOnInvalid));
        typeof(P).GetField("g_allow", Flags).SetValue(null, allow);
        typeof(P).GetField("g_lenient", Flags).SetValue(null, (Func<LibHac.Fs.Fsa.IFileSystem>)(() => new RomFsFileSystem(factory(IntegrityCheckLevel.IgnoreOnInvalid))));
        var method = typeof(P).GetMethod("WriteImage", Flags);
        try {
            method.Invoke(null, new object[] { mode == "empty" ? null : open, dst });
            if (crash) throw new Exception("read failure was swallowed");
        } catch (TargetInvocationException e) when (crash && e.InnerException is IOException io && io.Message == "read failed") {
            Console.WriteLine("expected read failure: " + io.Message);
        }
        var bad = (List<string>)typeof(P).GetField("g_damaged", Flags).GetValue(null);
        if (bad.Count != (damaged ? 1 : 0) || (damaged && bad[0] != "/large")) throw new Exception("wrong damaged files");
        Console.WriteLine($"damaged {bad.Count}"); return 0;
    }
}
