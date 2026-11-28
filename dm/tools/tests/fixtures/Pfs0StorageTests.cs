using System;
using System.Buffers.Binary;
using System.Linq;
using System.Text;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using Path = System.IO.Path;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;
using NcaFsHeader = LibHac.Tools.FsSystem.NcaUtils.NcaFsHeader;

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
    static byte[] SyntheticRomFsHeader(bool patch, int encEntries = 0, byte[] ivfc = null) {
        byte[] h = new byte[0x200];
        BinaryPrimitives.WriteInt16LittleEndian(h.AsSpan(0x00), 2);
        h[0x02] = (byte)NcaFormatType.Romfs;
        h[0x03] = (byte)NcaHashType.Ivfc;
        h[0x04] = (byte)(patch ? NcaEncryptionType.AesCtrEx : NcaEncryptionType.AesCtr);
        if (ivfc != null) ivfc.CopyTo(h, 0x08);
        else {
            "IVFC"u8.CopyTo(h.AsSpan(0x08));
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(0x0C), 0x20000);
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(0x10), 0x20);
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(0x14), 7);
            for (int i = 0x18; i < 0x100; i++) h[i] = (byte)(i ^ 0x5A);
        }
        if (patch) {
            BinaryPrimitives.WriteInt64LittleEndian(h.AsSpan(0x100), 0x4000);   // RelocationTreeOffset
            BinaryPrimitives.WriteInt64LittleEndian(h.AsSpan(0x108), 0x4000);   // RelocationTreeSize (> 0 => IsPatchSection)
            "BKTR"u8.CopyTo(h.AsSpan(0x110));
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(0x114), 1);
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(0x118), 1);
            BinaryPrimitives.WriteInt64LittleEndian(h.AsSpan(0x120), 0x8000);   // EncryptionTreeOffset
            BinaryPrimitives.WriteInt64LittleEndian(h.AsSpan(0x128), 0);        // EncryptionTreeSize
            "BKTR"u8.CopyTo(h.AsSpan(0x130));
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(0x134), 1);
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(0x138), encEntries);
        }
        return h;
    }
    static byte[] SyntheticNcaBytes(KeySet ks, NcaContentType type, int sectionIndex, byte[] fsHeader, bool includeSection = true, bool tamperFsHash = false, ulong tid = 0x0100000000001000UL) {
        const int bodySize = 0xC000;
        byte[] nca = new byte[0xC00 + bodySize];
        "NCA3"u8.CopyTo(nca.AsSpan(0x200));
        nca[0x205] = (byte)type;
        BinaryPrimitives.WriteInt64LittleEndian(nca.AsSpan(0x208), nca.Length);
        BinaryPrimitives.WriteUInt64LittleEndian(nca.AsSpan(0x210), tid);
        if (includeSection) {
            BinaryPrimitives.WriteInt32LittleEndian(nca.AsSpan(0x240 + 0x10 * sectionIndex), 0xC00 / 0x200);
            BinaryPrimitives.WriteInt32LittleEndian(nca.AsSpan(0x240 + 0x10 * sectionIndex + 4), nca.Length / 0x200);
            byte[] hash = SHA256.HashData(fsHeader);
            if (tamperFsHash) hash[0] ^= 0xFF;
            hash.CopyTo(nca, 0x280 + 0x20 * sectionIndex);
            fsHeader.CopyTo(nca, 0x400 + 0x200 * sectionIndex);
        }
        byte[] plainHeader = nca[..0xC00];
        using (var xts = new Aes128XtsStorage(new SubStorage(new MemoryStorage(nca), 0, 0xC00), ks.HeaderKey, 0x200, true, decryptRead: true))
            for (int off = 0; off < 0xC00; off += 0x200) xts.Write(off, plainHeader.AsSpan(off, 0x200)).ThrowIfFailure();
        return nca;
    }
    static Nca SyntheticProgramNca(KeySet ks, byte[] dataFsHeader, bool includeData = true, bool tamperFsHash = false)
        => new Nca(ks, new MemoryStorage(SyntheticNcaBytes(ks, NcaContentType.Program, 1, dataFsHeader, includeData, tamperFsHash)));
    static Nca SyntheticManualNca(KeySet ks, byte[] dataFsHeader)
        => new Nca(ks, new MemoryStorage(SyntheticNcaBytes(ks, NcaContentType.Manual, 0, dataFsHeader)));
    static byte[] BuildPfs0(params (string Name, byte[] Data)[] files) {
        using var names = new MemoryStream();
        uint[] nameOff = new uint[files.Length];
        for (int i = 0; i < files.Length; i++) {
            nameOff[i] = (uint)names.Length;
            names.Write(Encoding.ASCII.GetBytes(files[i].Name));
            names.WriteByte(0);
        }
        byte[] nameBytes = names.ToArray();
        int headerSize = 16 + 24 * files.Length + nameBytes.Length;
        long total = headerSize + files.Sum(f => (long)f.Data.Length);
        byte[] pfs = new byte[total];
        "PFS0"u8.CopyTo(pfs);
        BinaryPrimitives.WriteUInt32LittleEndian(pfs.AsSpan(4), (uint)files.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(pfs.AsSpan(8), (uint)nameBytes.Length);
        ulong off = 0;
        for (int i = 0; i < files.Length; i++) {
            BinaryPrimitives.WriteUInt64LittleEndian(pfs.AsSpan(16 + 24 * i), off);
            BinaryPrimitives.WriteUInt64LittleEndian(pfs.AsSpan(16 + 24 * i + 8), (ulong)files[i].Data.Length);
            BinaryPrimitives.WriteUInt32LittleEndian(pfs.AsSpan(16 + 24 * i + 16), nameOff[i]);
            files[i].Data.CopyTo(pfs, headerSize + (int)off);
            off += (ulong)files[i].Data.Length;
        }
        nameBytes.CopyTo(pfs, 16 + 24 * files.Length);
        return pfs;
    }
    static void CheckSparseHtmlExtraction(string tmp) {
        var ks = new KeySet();
        ks.HeaderKey.Data[0] = 1;
        ks.KeyAreaKeys[0][0].Data[0] = 1;

        byte[] fullFs = SyntheticRomFsHeader(patch: false);
        byte[] sparseFs = (byte[])fullFs.Clone(); new NcaFsHeader(sparseFs).GetSparseInfo().Generation = 1;
        byte[] patchFs = SyntheticRomFsHeader(patch: true, encEntries: 1);

        Nca baseSparse = SyntheticManualNca(ks, sparseFs);
        Nca baseSparse2 = SyntheticManualNca(ks, sparseFs);
        Nca baseLegal = SyntheticManualNca(ks, fullFs);
        Nca updPatch = SyntheticManualNca(ks, patchFs);
        Nca updLegalFull = SyntheticManualNca(ks, fullFs);

        byte[] romA = LiteralColonRomFs(Encoding.UTF8.GetBytes("patched-html"));
        byte[] romB = LiteralColonRomFs(Encoding.UTF8.GetBytes("update-legal"));
        byte[] romC = LiteralColonRomFs(Encoding.UTF8.GetBytes("base-legal"));
        Func<Nca, Func<IFileSystem>> openFull = n => () => new RomFsFileSystem(new MemoryStorage(ReferenceEquals(n, updLegalFull) ? romB : romC));
        Func<Nca, Nca, Func<IFileSystem>> matchOnlySparse = (b, u) => () => {
            if (ReferenceEquals(b, baseSparse) && ReferenceEquals(u, updPatch)) return new RomFsFileSystem(new MemoryStorage(romA));
            throw new HorizonResultException(ResultFs.NonRealDataVerificationFailed.Value);
        };
        Func<Nca, Nca, Func<IFileSystem>> matchOnlyLegal = (b, u) => () => {
            if (ReferenceEquals(b, baseLegal) && ReferenceEquals(u, updPatch)) return new RomFsFileSystem(new MemoryStorage(romA));
            throw new HorizonResultException(ResultFs.NonRealDataVerificationFailed.Value);
        };
        Func<Nca, Nca, Func<IFileSystem>> matchNone = (b, u) => () => throw new HorizonResultException(ResultFs.NonRealDataVerificationFailed.Value);

        (int Rc, string Log) RunCase(string name, System.Collections.Generic.List<Nca> bases, System.Collections.Generic.List<Nca> updates, Func<Nca, Nca, Func<IFileSystem>> openPatch) {
            string dir = Path.Combine(tmp, name);
            Directory.CreateDirectory(dir);
            TextWriter saved = Console.Out;
            using var log = new StringWriter();
            try { Console.SetOut(log); int rc = P.ExtractHtml(bases, updates, dir, openFull, openPatch); return (rc, log.ToString()); }
            finally { Console.SetOut(saved); }
        }

        // 1. Base-only with a sparse Manual NCA stops with exit 4 and writes no html directories (even when a non-sparse base precedes it).
        var c1 = RunCase("html_base_only_sparse", new() { baseLegal, baseSparse }, new(), matchNone);
        Check(c1.Rc == 4 && c1.Log.Contains("[RED] a sparse HtmlDocument NCA of the base game"));
        Check(Directory.GetFileSystemEntries(Path.Combine(tmp, "html_base_only_sparse")).Length == 0);

        // 2. An unrelated full update Manual NCA (e.g., LegalInformation) must NOT satisfy a sparse base Manual NCA.
        var c2 = RunCase("html_unrelated_full_update", new() { baseSparse, baseLegal }, new() { updLegalFull }, matchNone);
        Check(c2.Rc == 4 && c2.Log.Contains("[RED] a sparse HtmlDocument NCA of the base game"));
        Check(Directory.GetFileSystemEntries(Path.Combine(tmp, "html_unrelated_full_update")).Length == 0);

        // 3. A patch NCA that patches a DIFFERENT base Manual NCA must NOT satisfy an unpatched sparse base NCA.
        var c3 = RunCase("html_patch_other_base", new() { baseLegal, baseSparse }, new() { updPatch }, matchOnlyLegal);
        Check(c3.Rc == 4 && c3.Log.Contains("[RED] a sparse HtmlDocument NCA of the base game"));
        Check(Directory.GetFileSystemEntries(Path.Combine(tmp, "html_patch_other_base")).Length == 0);

        // 4. A failed patch NCA alongside a full update Manual NCA must still fail with exit 4 for the unpatched sparse base.
        var c4 = RunCase("html_failed_patch_plus_full", new() { baseSparse, baseLegal }, new() { updPatch, updLegalFull }, matchNone);
        Check(c4.Rc == 4 && c4.Log.Contains("patches none of the base game's") && c4.Log.Contains("[RED] a sparse HtmlDocument NCA of the base game"));
        Check(Directory.GetFileSystemEntries(Path.Combine(tmp, "html_failed_patch_plus_full")).Length == 0);

        // 5. Two sparse base Manual NCAs where only one is patched must fail with exit 4 for the second.
        var c5 = RunCase("html_second_sparse_unpatched", new() { baseSparse, baseSparse2 }, new() { updPatch }, matchOnlySparse);
        Check(c5.Rc == 4 && c5.Log.Contains("[RED] a sparse HtmlDocument NCA of the base game"));
        Check(Directory.GetFileSystemEntries(Path.Combine(tmp, "html_second_sparse_unpatched")).Length == 0);

        // 6. Matching patch over the sparse base + full update LegalInformation + non-sparse base LegalInformation -> 3 html dirs, exit 0.
        var c6 = RunCase("html_with_matching_patch", new() { baseSparse, baseLegal }, new() { updPatch, updLegalFull }, matchOnlySparse);
        Check(c6.Rc == 0 && c6.Log.Contains("a patch of the base game's NCA") && c6.Log.Contains("3 HtmlDocument NCAs"));
        Check(File.ReadAllText(Path.Combine(tmp, "html_with_matching_patch", "html0", "D:", "sample")) == "patched-html");
        Check(File.ReadAllText(Path.Combine(tmp, "html_with_matching_patch", "html1", "D:", "sample")) == "update-legal");
        Check(File.ReadAllText(Path.Combine(tmp, "html_with_matching_patch", "html2", "D:", "sample")) == "base-legal");

        // 7. Non-sparse base Manual NCA with no update still succeeds (exit 0).
        var c7 = RunCase("html_non_sparse_base_only", new() { baseLegal }, new(), matchNone);
        Check(c7.Rc == 0 && c7.Log.Contains("1 HtmlDocument NCAs"));
        Check(File.ReadAllText(Path.Combine(tmp, "html_non_sparse_base_only", "html0", "D:", "sample")) == "base-legal");

        // 8. End-to-end CLI (--html) on a synthetic base-only NSP containing a Program NCA and a sparse Manual NCA.
        string keysPath = Path.Combine(tmp, "dummy.keys"), baseNsp = Path.Combine(tmp, "sparse_base.nsp"), cliOut = Path.Combine(tmp, "html_cli_out");
        Directory.CreateDirectory(cliOut);
        string hkey = Convert.ToHexString(ks.HeaderKey.Data.ToArray()).ToLowerInvariant();
        string kkey = Convert.ToHexString(ks.KeyAreaKeys[0][0].Data.ToArray()).ToLowerInvariant();
        File.WriteAllText(keysPath, $"header_key = {hkey}\nkey_area_key_application_00 = {kkey}\n");
        byte[] progBytes = SyntheticNcaBytes(ks, NcaContentType.Program, 1, fullFs);
        byte[] sparseManualBytes = SyntheticNcaBytes(ks, NcaContentType.Manual, 0, sparseFs);
        File.WriteAllBytes(baseNsp, BuildPfs0(("prog.nca", progBytes), ("manual.nca", sparseManualBytes)));
        TextWriter savedCli = Console.Out;
        using var cliLog = new StringWriter();
        int cliRc;
        try {
            Console.SetOut(cliLog);
            cliRc = (int)typeof(P).GetMethod("Main", BindingFlags.NonPublic | BindingFlags.Static)
                .Invoke(null, new object[] { new[] { keysPath, baseNsp, "-", cliOut, "--html" } });
        } finally { Console.SetOut(savedCli); }
        Check(cliRc == 4 && cliLog.ToString().Contains("[RED] a sparse HtmlDocument NCA of the base game"));
        Check(Directory.GetFileSystemEntries(cliOut).Length == 0);
    }
    static void CheckPatchKeepsData() {
        // Synthetic dummy keys only (no game keys or data). Encrypting the NCA3 header exercises
        // LibHac's real OpenStorageWithPatch -> OpenAesCtrExStorage -> Aes128CtrExStorage path.
        var ks = new KeySet();
        ks.HeaderKey.Data[0] = 1;
        ks.KeyAreaKeys[0][0].Data[0] = 1;
        byte[] baseFs = SyntheticRomFsHeader(patch: false);
        byte[] ivfc = baseFs[0x08..0x100];
        byte[] updFs = SyntheticRomFsHeader(patch: true, encEntries: 0, ivfc: ivfc);
        Nca baseNca = SyntheticProgramNca(ks, baseFs);
        Nca updNca = SyntheticProgramNca(ks, updFs);

        // Reproduce LibHac 0.19's ResultFsInvalidArgument (2002-6001) on an empty AES-CTR-Ex table.
        bool sawInvalidArg = false;
        try { baseNca.OpenStorageWithPatch(updNca, NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid); }
        catch (LibHac.Common.HorizonResultException e) { sawInvalidArg = ResultFs.InvalidArgument.Includes(e.ResultValue); }
        Check(sawInvalidArg);

        // Matching IVFC + empty AES-CTR-Ex RomFS patch is accepted.
        Check(P.PatchKeepsData(baseNca, updNca));

        // Non-empty or negative AES-CTR-Ex entry count must stay on the normal patch path.
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, SyntheticRomFsHeader(patch: true, encEntries: 1, ivfc: ivfc))));
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, SyntheticRomFsHeader(patch: true, encEntries: -1, ivfc: ivfc))));

        // Invalid encryption tree header magic is rejected.
        byte[] badMagic = (byte[])updFs.Clone(); badMagic[0x130] = (byte)'X';
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, badMagic)));

        // Any 1-byte difference in IVFC levels, salt, or master hash is rejected.
        foreach (int pos in new[] { 0x20, 0xA0, 0xF0 }) {
            byte[] diffIvfc = (byte[])updFs.Clone(); diffIvfc[pos] ^= 1;
            Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, diffIvfc)));
        }

        // Non-patch update, non-AesCtrEx update, wrong format/hash type, or bad IVFC magic are rejected.
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, SyntheticRomFsHeader(patch: false, ivfc: ivfc))));
        byte[] nonCtrEx = (byte[])updFs.Clone(); nonCtrEx[0x04] = (byte)NcaEncryptionType.AesCtr;
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, nonCtrEx)));
        byte[] pfs0Upd = (byte[])updFs.Clone(); pfs0Upd[0x02] = (byte)NcaFormatType.Pfs0;
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, pfs0Upd)));
        byte[] shaUpd = (byte[])updFs.Clone(); shaUpd[0x03] = (byte)NcaHashType.Sha256;
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, shaUpd)));
        byte[] badIvfcBase = (byte[])baseFs.Clone(), badIvfcUpd = (byte[])updFs.Clone();
        badIvfcBase[0x08] = (byte)'X'; badIvfcUpd[0x08] = (byte)'X';
        Check(!P.PatchKeepsData(SyntheticProgramNca(ks, badIvfcBase), SyntheticProgramNca(ks, badIvfcUpd)));

        // Sparse or compressed layers on either NCA are rejected.
        byte[] sparseBase = (byte[])baseFs.Clone(); new NcaFsHeader(sparseBase).GetSparseInfo().Generation = 1;
        Check(!P.PatchKeepsData(SyntheticProgramNca(ks, sparseBase), updNca));
        byte[] sparseUpd = (byte[])updFs.Clone(); new NcaFsHeader(sparseUpd).GetSparseInfo().Generation = 1;
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, sparseUpd)));
        byte[] compBase = (byte[])baseFs.Clone();
        ref var cbi = ref new NcaFsHeader(compBase).GetCompressionInfo(); cbi.TableOffset = 0x1000; cbi.TableSize = 0x4000;
        Check(!P.PatchKeepsData(SyntheticProgramNca(ks, compBase), updNca));
        byte[] compUpd = (byte[])updFs.Clone();
        ref var ci = ref new NcaFsHeader(compUpd).GetCompressionInfo(); ci.TableOffset = 0x1000; ci.TableSize = 0x4000;
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, compUpd)));

        // Missing data sections are rejected; tampered FS header hash fails closed.
        Check(!P.PatchKeepsData(SyntheticProgramNca(ks, baseFs, includeData: false), updNca));
        Check(!P.PatchKeepsData(baseNca, SyntheticProgramNca(ks, updFs, includeData: false)));
        bool sawTamper = false;
        try { P.PatchKeepsData(SyntheticProgramNca(ks, baseFs, tamperFsHash: true), updNca); }
        catch (InvalidDataException) { sawTamper = true; }
        Check(sawTamper);
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
            CheckPatchKeepsData();
            CheckSparseHtmlExtraction(tmp);
        } finally { Console.SetOut(saved); Directory.Delete(tmp, true); }
        Console.WriteLine("PFS0 storage checks passed");
    }
}
